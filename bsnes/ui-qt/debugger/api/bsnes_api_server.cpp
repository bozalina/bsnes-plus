// NOTE: httplib.h must be included before any bsnes headers that define macros
// interfering with Winsock / POSIX socket headers. Since this file is Unity-
// built from debugger.cpp (all bsnes headers already in scope), we include
// httplib.h here and rely on its own include guards.
#include "httplib.h"
#include "bsnes_api_server.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>
#include <sstream>
#include <iomanip>
#include <stdexcept>

// ── Global singleton ─────────────────────────────────────────────────────────
BsnesApiServer* bsnesApiServer = nullptr;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string hexStr(uint32_t val, int width) {
    std::ostringstream ss;
    ss << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << val;
    return ss.str();
}

// ── Constructor / destructor ─────────────────────────────────────────────────

BsnesApiServer::BsnesApiServer() = default;

BsnesApiServer::~BsnesApiServer() {
    stop();
}

// ── start / stop ─────────────────────────────────────────────────────────────

void BsnesApiServer::start(int port) {
    _svr.reset(new httplib::Server());
    setupRoutes();
    _serverThread = std::thread([this, port]() {
        _svr->listen("127.0.0.1", port);
    });
}

void BsnesApiServer::stop() {
    if (_svr) {
        _svr->stop();
    }
    if (_serverThread.joinable()) {
        _serverThread.join();
    }
}

// ── dispatch ─────────────────────────────────────────────────────────────────

void BsnesApiServer::dispatch(std::function<void()> fn, bool blocking) {
    if (blocking) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [fn]() { fn(); },
            Qt::BlockingQueuedConnection);
    } else {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [fn]() { fn(); },
            Qt::QueuedConnection);
    }
}

// ── notifyBreak ───────────────────────────────────────────────────────────────
// Called on the Qt main thread from Debugger::event(), so the emulator is
// paused and it is safe to read registers/memory directly.

void BsnesApiServer::notifyBreak() {
    auto result = buildBreakResult();   // safe: Qt thread, emulator halted
    {
        std::lock_guard<std::mutex> lock(_breakMutex);
        _breakOccurred = true;
        _lastBreakResult = std::move(result);
    }
    _breakCv.notify_one();
}

// ── doStep ────────────────────────────────────────────────────────────────────
// Triggers a step on the Qt thread, then blocks the HTTP-handler thread until
// the emulator fires the next break event (or timeout).

json BsnesApiServer::doStep(SNES::Debugger::StepType type,
                             bool stepOverNew,
                             std::chrono::seconds timeout) {
    // Reset the break flag before triggering — avoids a stale notification.
    {
        std::lock_guard<std::mutex> lock(_breakMutex);
        _breakOccurred = false;
    }

    // Dispatch step setup to the Qt main thread (non-blocking — the emulator
    // must be free to run; a blocking dispatch would deadlock).
    dispatch([type, stepOverNew]() {
        SNES::debugger.step_type    = type;
        SNES::debugger.step_cpu     = true;   // ensure CPU stepping is on
        SNES::debugger.call_count   = 0;
        if (stepOverNew) {
            SNES::debugger.step_over_new = true;
        }
        application.debug    = true;
        application.debugrun = true;
    }, /*blocking=*/false);

    // Block until break or timeout.
    std::unique_lock<std::mutex> lock(_breakMutex);
    bool fired = _breakCv.wait_for(lock, timeout,
                                    [this] { return _breakOccurred; });
    if (!fired) {
        lock.unlock();   // release before blocking dispatch to avoid deadlock
        // Timeout — force a break so the emulator doesn't run forever.
        dispatch([]() {
            application.debug    = true;
            application.debugrun = false;
        }, /*blocking=*/true);
        return {{"error", "step timed out"}, {"code", "TIMEOUT"}};
    }
    return _lastBreakResult;
}

// ── getCpuStateJson ───────────────────────────────────────────────────────────
// Must be called from the Qt main thread (emulator paused).

json BsnesApiServer::getCpuStateJson() {
    using R = SNES::CPUDebugger::Register;
    using F = SNES::CPUDebugger;
    auto& cpu = SNES::cpu;

    json regs = {
        {"pc", hexStr(cpu.getRegister(R::RegisterPC), 6)},
        {"a",  hexStr(cpu.getRegister(R::RegisterA),  4)},
        {"x",  hexStr(cpu.getRegister(R::RegisterX),  4)},
        {"y",  hexStr(cpu.getRegister(R::RegisterY),  4)},
        {"s",  hexStr(cpu.getRegister(R::RegisterS),  4)},
        {"d",  hexStr(cpu.getRegister(R::RegisterD),  4)},
        {"db", hexStr(cpu.getRegister(R::RegisterDB), 2)},
        {"p",  hexStr(cpu.getRegister(R::RegisterP),  2)},
    };

    json flags = {
        {"e", cpu.getFlag(F::FlagE)},
        {"n", cpu.getFlag(F::FlagN)},
        {"v", cpu.getFlag(F::FlagV)},
        {"m", cpu.getFlag(F::FlagM)},
        {"x", cpu.getFlag(F::FlagX)},
        {"d", cpu.getFlag(F::FlagD)},
        {"i", cpu.getFlag(F::FlagI)},
        {"z", cpu.getFlag(F::FlagZ)},
        {"c", cpu.getFlag(F::FlagC)},
    };

    return {{"registers", regs}, {"flags", flags}};
}

// ── buildBreakResult ──────────────────────────────────────────────────────────
// Assembles the JSON payload returned by break/step endpoints.
// Must be called from the Qt main thread.

json BsnesApiServer::buildBreakResult() {
    char buf[256];
    SNES::cpu.disassemble_opcode(buf, SNES::cpu.opcode_pc, false);

    std::string breakEvent;
    switch (SNES::debugger.break_event) {
        case SNES::Debugger::BreakEvent::BreakpointHit: breakEvent = "BreakpointHit"; break;
        case SNES::Debugger::BreakEvent::CPUStep:       breakEvent = "CPUStep";       break;
        case SNES::Debugger::BreakEvent::SMPStep:       breakEvent = "SMPStep";       break;
        case SNES::Debugger::BreakEvent::SA1Step:       breakEvent = "SA1Step";       break;
        case SNES::Debugger::BreakEvent::SFXStep:       breakEvent = "SFXStep";       break;
        case SNES::Debugger::BreakEvent::SGBStep:       breakEvent = "SGBStep";       break;
        default:                                         breakEvent = "None";          break;
    }

    json result = {
        {"paused",        true},
        {"breakEvent",    breakEvent},
        {"breakpointHit", (int)SNES::debugger.breakpoint_hit},
        {"opcodeAddr",    hexStr(SNES::cpu.opcode_pc, 6)},
        {"disasm",        std::string(buf)},
        {"cpu",           getCpuStateJson()},
    };
    return result;
}

// ── disassembleAt ─────────────────────────────────────────────────────────────
// Returns a JSON object with disassembly results. The starting address must
// have been executed (UsageOpcode set); returns a startError sentinel otherwise.
// Must be called from the Qt main thread.

json BsnesApiServer::disassembleAt(uint32_t addr, int lines) {
    addr &= 0xFFFFFF;

    if (!(SNES::cpu.usage[addr] & SNES::CPUDebugger::UsageOpcode)) {
        return {
            {"startError", true},
            {"message",
             "Address " + hexStr(addr, 6) +
             " has not been executed; M/X state is unknown. "
             "Run to this address first."}
        };
    }

    json instructions = json::array();
    bool truncated    = false;
    uint32_t truncAt  = 0;

    for (int i = 0; i < lines; ++i) {
        addr &= 0xFFFFFF;

        if (i > 0 && !(SNES::cpu.usage[addr] & SNES::CPUDebugger::UsageOpcode)) {
            truncated = true;
            truncAt   = addr;
            break;
        }

        char buf[256];
        SNES::cpu.disassemble_opcode(buf, addr, false);

        int len = 1;
        for (int k = 1; k <= 4; ++k) {
            uint32_t next = (addr + k) & 0xFFFFFF;
            if (SNES::cpu.usage[next] & SNES::CPUDebugger::UsageOpcode) {
                len = k;
                break;
            }
        }

        json bytes = json::array();
        for (int b = 0; b < len; ++b)
            bytes.push_back((int)SNES::debugger.read(
                SNES::Debugger::MemorySource::CPUBus, (addr + b) & 0xFFFFFF));

        instructions.push_back({
            {"addr",  hexStr(addr, 6)},
            {"bytes", bytes},
            {"text",  std::string(buf)},
        });

        addr = (addr + len) & 0xFFFFFF;
    }

    json result = {
        {"startError",   false},
        {"instructions", instructions},
        {"truncated",    truncated},
    };
    if (truncated)
        result["truncatedAt"] = hexStr(truncAt, 6);
    return result;
}

// ── parseSource ───────────────────────────────────────────────────────────────

bool BsnesApiServer::parseSource(const std::string& name,
                                  SNES::Debugger::MemorySource& out) {
    using MS = SNES::Debugger::MemorySource;
    if      (name == "cpu")     { out = MS::CPUBus;  return true; }
    else if (name == "apu")     { out = MS::APUBus;  return true; }
    else if (name == "apuram")  { out = MS::APURAM;  return true; }
    else if (name == "dsp")     { out = MS::DSP;     return true; }
    else if (name == "vram")    { out = MS::VRAM;    return true; }
    else if (name == "oam")     { out = MS::OAM;     return true; }
    else if (name == "cgram")   { out = MS::CGRAM;   return true; }
    else if (name == "cartrom") { out = MS::CartROM; return true; }
    else if (name == "cartram") { out = MS::CartRAM; return true; }
    return false;
}

// ── readMemory ────────────────────────────────────────────────────────────────
// Must be called from the Qt main thread.

json BsnesApiServer::readMemory(const std::string& source,
                                 uint32_t addr, int count) {
    SNES::Debugger::MemorySource src;
    parseSource(source, src);

    count = std::min(count, 4096);
    json data = json::array();
    for (int i = 0; i < count; ++i) {
        data.push_back((int)SNES::debugger.read(src, addr + i));
    }
    return {
        {"source", source},
        {"addr",   hexStr(addr, 6)},
        {"count",  count},
        {"data",   data},
    };
}

// ── writeMemory ───────────────────────────────────────────────────────────────
// Must be called from the Qt main thread.

void BsnesApiServer::writeMemory(const std::string& source,
                                  uint32_t addr,
                                  const std::vector<uint8_t>& data) {
    SNES::Debugger::MemorySource src;
    parseSource(source, src);
    for (size_t i = 0; i < data.size(); ++i) {
        SNES::debugger.write(src, addr + i, data[i]);
    }
}

// ── sendJson / sendError ──────────────────────────────────────────────────────

void BsnesApiServer::sendJson(httplib::Response& res,
                               const json& j, int status) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

void BsnesApiServer::sendError(httplib::Response& res, int status,
                                const std::string& code,
                                const std::string& message) {
    sendJson(res, {{"error", code}, {"message", message}}, status);
}

// ── requirePaused / requireLoaded ─────────────────────────────────────────────

bool BsnesApiServer::requireLoaded(httplib::Response& res) {
    if (!SNES::cartridge.loaded()) {
        sendError(res, 503, "NO_CARTRIDGE", "No cartridge is loaded.");
        return false;
    }
    return true;
}

bool BsnesApiServer::requirePaused(httplib::Response& res) {
    if (!requireLoaded(res)) return false;
    if (!application.debug || application.debugrun) {
        sendError(res, 409, "NOT_PAUSED",
                  "Emulator is not paused. POST /break first.");
        return false;
    }
    return true;
}

// ── breakpointToJson ──────────────────────────────────────────────────────────

json BsnesApiServer::breakpointToJson(int index,
                                       const SNES::Debugger::Breakpoint& bp) {
    using Mode   = SNES::Debugger::Breakpoint::Mode;
    using Source = SNES::Debugger::Breakpoint::Source;

    json modeArr = json::array();
    if (bp.mode & (unsigned)Mode::Exec)  modeArr.push_back("Exec");
    if (bp.mode & (unsigned)Mode::Read)  modeArr.push_back("Read");
    if (bp.mode & (unsigned)Mode::Write) modeArr.push_back("Write");

    std::string srcStr;
    switch (bp.source) {
        case Source::CPUBus:  srcStr = "CPUBus";  break;
        case Source::APURAM:  srcStr = "APURAM";  break;
        case Source::DSP:     srcStr = "DSP";     break;
        case Source::VRAM:    srcStr = "VRAM";    break;
        case Source::OAM:     srcStr = "OAM";     break;
        case Source::CGRAM:   srcStr = "CGRAM";   break;
        case Source::SA1Bus:  srcStr = "SA1Bus";  break;
        case Source::SFXBus:  srcStr = "SFXBus";  break;
        case Source::SGBBus:  srcStr = "SGBBus";  break;
        default:              srcStr = "Unknown";  break;
    }

    std::string cmpStr;
    switch (bp.compare) {
        case SNES::Debugger::Breakpoint::Compare::Equal:        cmpStr = "Equal";        break;
        case SNES::Debugger::Breakpoint::Compare::NotEqual:     cmpStr = "NotEqual";     break;
        case SNES::Debugger::Breakpoint::Compare::Less:         cmpStr = "Less";         break;
        case SNES::Debugger::Breakpoint::Compare::LessEqual:    cmpStr = "LessEqual";    break;
        case SNES::Debugger::Breakpoint::Compare::Greater:      cmpStr = "Greater";      break;
        case SNES::Debugger::Breakpoint::Compare::GreaterEqual: cmpStr = "GreaterEqual"; break;
    }

    json j = {
        {"index",   index},
        {"addr",    hexStr(bp.addr, 6)},
        {"mode",    modeArr},
        {"source",  srcStr},
        {"compare", cmpStr},
        {"data",    bp.data},
        {"counter", bp.counter},
    };
    if (bp.addr_end > 0)
        j["addrEnd"] = hexStr(bp.addr_end, 6);
    return j;
}

// ── setupRoutes ───────────────────────────────────────────────────────────────

void BsnesApiServer::setupRoutes() {
    // ── GET /status ──────────────────────────────────────────────────────────
    // Does NOT require paused. Safe to call at any time.
    _svr->Get("/status", [this](const httplib::Request&, httplib::Response& res) {
        json result;
        dispatch([this, &result]() {
            bool paused = application.debug && !application.debugrun;
            std::string evtStr;
            switch (SNES::debugger.break_event) {
                case SNES::Debugger::BreakEvent::BreakpointHit: evtStr = "BreakpointHit"; break;
                case SNES::Debugger::BreakEvent::CPUStep:       evtStr = "CPUStep";       break;
                case SNES::Debugger::BreakEvent::SMPStep:       evtStr = "SMPStep";       break;
                case SNES::Debugger::BreakEvent::SA1Step:       evtStr = "SA1Step";       break;
                case SNES::Debugger::BreakEvent::SFXStep:       evtStr = "SFXStep";       break;
                case SNES::Debugger::BreakEvent::SGBStep:       evtStr = "SGBStep";       break;
                default:                                         evtStr = "None";          break;
            }
            result = {
                {"paused",     paused},
                {"loaded",     (bool)SNES::cartridge.loaded()},
                {"breakEvent", evtStr},
                {"cpu",        paused ? getCpuStateJson() : json(nullptr)},
            };
        }, /*blocking=*/true);
        sendJson(res, result);
    });

    // ── POST /break ──────────────────────────────────────────────────────────
    _svr->Post("/break", [this](const httplib::Request&, httplib::Response& res) {
        json result;
        dispatch([this, &result]() {
            application.debug    = true;
            application.debugrun = false;
            result = buildBreakResult();
        }, /*blocking=*/true);
        sendJson(res, result);
    });

    // ── POST /resume ─────────────────────────────────────────────────────────
    // Non-blocking: the emulator is released to run; we don't wait for a break.
    _svr->Post("/resume", [this](const httplib::Request&, httplib::Response& res) {
        dispatch([]() {
            application.debug    = true;
            application.debugrun = true;
        }, /*blocking=*/false);
        sendJson(res, {{"accepted", true}});
    });

    // ── POST /run ────────────────────────────────────────────────────────────
    // Exits debug mode entirely — emulator runs at full speed.
    _svr->Post("/run", [this](const httplib::Request&, httplib::Response& res) {
        dispatch([]() {
            application.debug    = false;
            application.debugrun = false;
        }, /*blocking=*/false);
        sendJson(res, {{"accepted", true}});
    });

    // ── POST /step/into ──────────────────────────────────────────────────────
    _svr->Post("/step/into", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepInto);
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /step/over ──────────────────────────────────────────────────────
    _svr->Post("/step/over", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepOver,
                             /*stepOverNew=*/true);
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /step/out ───────────────────────────────────────────────────────
    _svr->Post("/step/out", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepOut);
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /step/vblank ────────────────────────────────────────────────────
    _svr->Post("/step/vblank", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepToVBlank,
                             false, std::chrono::seconds(30));
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /step/hblank ────────────────────────────────────────────────────
    _svr->Post("/step/hblank", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepToHBlank,
                             false, std::chrono::seconds(30));
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /step/nmi ───────────────────────────────────────────────────────
    _svr->Post("/step/nmi", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepToNMI,
                             false, std::chrono::seconds(30));
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /step/irq ───────────────────────────────────────────────────────
    _svr->Post("/step/irq", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepToIRQ,
                             false, std::chrono::seconds(30));
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /reset ──────────────────────────────────────────────────────────
    _svr->Post("/reset", [this](const httplib::Request&, httplib::Response& res) {
        bool ok = false;
        dispatch([&ok]() {
            if (!SNES::cartridge.loaded() || !application.power) return;
            utility.modifySystemState(Utility::Reset);
            ok = true;
        }, true);
        if (!ok) {
            sendError(res, 409, "NOT_READY",
                      "Reset requires a loaded cartridge with power on.");
            return;
        }
        sendJson(res, {{"accepted", true}});
    });

    // ── POST /reload ─────────────────────────────────────────────────────────
    _svr->Post("/reload", [this](const httplib::Request&, httplib::Response& res) {
        bool ok = false;
        dispatch([&ok]() {
            if (application.currentRom == "") return;
            utility.modifySystemState(Utility::ReloadCartridge);
            ok = true;
        }, true);
        if (!ok) {
            sendError(res, 409, "NO_ROM", "No ROM is currently loaded.");
            return;
        }
        sendJson(res, {{"accepted", true}, {"name", (const char*)cartridge.name}});
    });

    // ── POST /power-cycle ────────────────────────────────────────────────────
    _svr->Post("/power-cycle", [this](const httplib::Request&, httplib::Response& res) {
        bool ok = false;
        dispatch([&ok]() {
            if (!SNES::cartridge.loaded()) return;
            utility.modifySystemState(Utility::PowerCycle);
            ok = true;
        }, true);
        if (!ok) {
            sendError(res, 409, "NO_ROM", "No cartridge is loaded.");
            return;
        }
        sendJson(res, {{"accepted", true}});
    });

    // ── POST /cartridge/load ─────────────────────────────────────────────────
    _svr->Post("/cartridge/load", [this](const httplib::Request& req,
                                         httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            sendError(res, 400, "INVALID_BODY", "Expected JSON body."); return;
        }
        if (!body.contains("path") || !body["path"].is_string()) {
            sendError(res, 400, "MISSING_PARAM", "'path' (string) is required."); return;
        }

        std::string path = body["path"].get<std::string>();
        bool ok = false;
        std::string loadedName;

        dispatch([&]() {
            if (cartridge.loadNormal(path.c_str())) {
                ok = true;
                loadedName = (const char*)cartridge.name;
            }
        }, true);

        if (!ok) {
            sendError(res, 422, "LOAD_FAILED",
                      "Failed to load '" + path + "'. File may not exist or is not a valid ROM.");
            return;
        }
        sendJson(res, {{"loaded", true}, {"name", loadedName}, {"path", path}});
    });

    // ── GET /cpu/registers ───────────────────────────────────────────────────
    _svr->Get("/cpu/registers", [this](const httplib::Request&, httplib::Response& res) {
        if (!requirePaused(res)) return;
        json result;
        dispatch([this, &result]() {
            result = getCpuStateJson();
        }, true);
        sendJson(res, result);
    });

    // ── PUT /cpu/registers ───────────────────────────────────────────────────
    _svr->Put("/cpu/registers", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;
        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_JSON", "Body is not valid JSON."); return; }

        json result;
        dispatch([this, &body, &result]() {
            using R = SNES::CPUDebugger::Register;
            using F = SNES::CPUDebugger;
            auto& cpu = SNES::cpu;

            // Parse hex-string register values
            auto setReg = [&](const std::string& key, R reg) {
                if (body.contains(key) && body[key].is_string()) {
                    unsigned val = (unsigned)std::stoul(body[key].get<std::string>(), nullptr, 16);
                    cpu.setRegister((unsigned)reg, val);
                }
            };
            setReg("pc", R::RegisterPC);
            setReg("a",  R::RegisterA);
            setReg("x",  R::RegisterX);
            setReg("y",  R::RegisterY);
            setReg("s",  R::RegisterS);
            setReg("d",  R::RegisterD);
            setReg("db", R::RegisterDB);
            setReg("p",  R::RegisterP);

            // Parse boolean flag values from nested "flags" object
            if (body.contains("flags") && body["flags"].is_object()) {
                auto& f = body["flags"];
                if (f.contains("e") && f["e"].is_boolean()) cpu.setFlag(F::FlagE, f["e"].get<bool>());
                if (f.contains("n") && f["n"].is_boolean()) cpu.setFlag(F::FlagN, f["n"].get<bool>());
                if (f.contains("v") && f["v"].is_boolean()) cpu.setFlag(F::FlagV, f["v"].get<bool>());
                if (f.contains("m") && f["m"].is_boolean()) cpu.setFlag(F::FlagM, f["m"].get<bool>());
                if (f.contains("x") && f["x"].is_boolean()) cpu.setFlag(F::FlagX, f["x"].get<bool>());
                if (f.contains("d") && f["d"].is_boolean()) cpu.setFlag(F::FlagD, f["d"].get<bool>());
                if (f.contains("i") && f["i"].is_boolean()) cpu.setFlag(F::FlagI, f["i"].get<bool>());
                if (f.contains("z") && f["z"].is_boolean()) cpu.setFlag(F::FlagZ, f["z"].get<bool>());
                if (f.contains("c") && f["c"].is_boolean()) cpu.setFlag(F::FlagC, f["c"].get<bool>());
            }
            result = getCpuStateJson();
        }, true);
        sendJson(res, result);
    });

    // ── GET /cpu/disassemble?addr=C08000&lines=10 ────────────────────────────
    _svr->Get("/cpu/disassemble", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;
        if (!req.has_param("addr")) {
            sendError(res, 400, "MISSING_PARAM", "'addr' is required."); return;
        }
        uint32_t addr;
        int lines = 10;
        try {
            addr  = (uint32_t)std::stoul(req.get_param_value("addr"), nullptr, 16);
            if (req.has_param("lines"))
                lines = std::stoi(req.get_param_value("lines"));
        } catch (...) {
            sendError(res, 400, "INVALID_PARAM", "Cannot parse addr or lines."); return;
        }
        lines = std::max(1, std::min(lines, 256));

        json result;
        dispatch([this, &result, addr, lines]() {
            result = disassembleAt(addr, lines);
        }, true);

        if (result.value("startError", false)) {
            sendError(res, 422, "UNTRACED_ADDRESS",
                      result.value("message", "Address has not been executed."));
            return;
        }
        result.erase("startError");
        sendJson(res, result);
    });

    // ── GET /cpu/usage?addr=C08000&count=256 ─────────────────────────────────
    _svr->Get("/cpu/usage", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;
        if (!req.has_param("addr")) {
            sendError(res, 400, "MISSING_PARAM", "Query param 'addr' is required."); return;
        }
        uint32_t addr;
        int count = 256;
        try {
            addr  = (uint32_t)std::stoul(req.get_param_value("addr"), nullptr, 16);
            if (req.has_param("count"))
                count = std::stoi(req.get_param_value("count"));
        } catch (...) {
            sendError(res, 400, "INVALID_PARAM", "Could not parse addr or count."); return;
        }
        count = std::min(std::max(count, 1), 65536);
        json result;
        dispatch([this, &result, addr, count]() {
            using U = SNES::CPUDebugger;
            result = json::array();
            for (int i = 0; i < count; ++i) {
                uint32_t a = (addr + i) & 0xFFFFFF;
                uint8_t  u = SNES::cpu.usage[a];
                result.push_back({
                    {"addr",   hexStr(a, 6)},
                    {"read",   (bool)(u & U::UsageRead)},
                    {"write",  (bool)(u & U::UsageWrite)},
                    {"exec",   (bool)(u & U::UsageExec)},
                    {"opcode", (bool)(u & U::UsageOpcode)},
                    {"flagM",  (bool)(u & U::UsageFlagM)},
                    {"flagX",  (bool)(u & U::UsageFlagX)},
                });
            }
        }, true);
        sendJson(res, result);
    });

    // ── GET /memory/{source}?addr=7E0000&count=16 ────────────────────────────
    _svr->Get("/memory/:source", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;
        std::string srcName = req.path_params.at("source");
        SNES::Debugger::MemorySource src;
        if (!parseSource(srcName, src)) {
            sendError(res, 400, "INVALID_SOURCE", "Unknown memory source: " + srcName);
            return;
        }
        if (!req.has_param("addr")) {
            sendError(res, 400, "MISSING_PARAM", "'addr' is required."); return;
        }
        uint32_t addr;
        int count = 256;
        try {
            addr  = (uint32_t)std::stoul(req.get_param_value("addr"), nullptr, 16);
            if (req.has_param("count"))
                count = std::stoi(req.get_param_value("count"));
        } catch (...) {
            sendError(res, 400, "INVALID_PARAM", "Could not parse addr or count."); return;
        }
        json result;
        dispatch([this, &result, srcName, addr, count]() {
            result = readMemory(srcName, addr, count);
        }, true);
        sendJson(res, result);
    });

    // ── PUT /memory/{source}?addr=7E0000  body: {"data":[0,1,2,...]} ─────────
    _svr->Put("/memory/:source", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;
        std::string srcName = req.path_params.at("source");
        SNES::Debugger::MemorySource src;
        if (!parseSource(srcName, src)) {
            sendError(res, 400, "INVALID_SOURCE", "Unknown memory source: " + srcName);
            return;
        }
        if (!req.has_param("addr")) {
            sendError(res, 400, "MISSING_PARAM", "'addr' is required."); return;
        }
        uint32_t addr;
        try { addr = (uint32_t)std::stoul(req.get_param_value("addr"), nullptr, 16); }
        catch (...) { sendError(res, 400, "INVALID_PARAM", "Could not parse addr."); return; }

        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_JSON", "Body is not valid JSON."); return; }

        if (!body.contains("data") || !body["data"].is_array()) {
            sendError(res, 400, "MISSING_FIELD", "'data' array is required."); return;
        }
        std::vector<uint8_t> data;
        for (auto& elem : body["data"]) {
            if (!elem.is_number_integer()) {
                sendError(res, 400, "INVALID_DATA", "data elements must be integers."); return;
            }
            data.push_back((uint8_t)elem.get<int>());
        }
        if (data.size() > 4096) {
            sendError(res, 400, "TOO_LARGE", "data array exceeds 4096-byte limit."); return;
        }
        dispatch([this, srcName, addr, data]() {
            writeMemory(srcName, addr, data);
        }, true);
        sendJson(res, {{"written", (int)data.size()}, {"addr", hexStr(addr, 6)}});
    });

    // ── GET /breakpoints ─────────────────────────────────────────────────────
    _svr->Get("/breakpoints", [this](const httplib::Request&, httplib::Response& res) {
        json result;
        dispatch([this, &result]() {
            result = json::array();
            for (unsigned i = 0; i < SNES::debugger.breakpoint.size(); ++i) {
                result.push_back(breakpointToJson(i, SNES::debugger.breakpoint[i]));
            }
        }, true);
        sendJson(res, result);
    });

    // ── POST /breakpoints ────────────────────────────────────────────────────
    // Required body fields: mode (array of "Exec"/"Read"/"Write"), source (string)
    // Optional: addr (hex string, default "000000"), addrEnd (hex string),
    //           data (int -1..255, default -1), compare (string, default "Equal")
    _svr->Post("/breakpoints", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_JSON", "Body is not valid JSON."); return; }

        if (!body.contains("mode") || !body["mode"].is_array() || body["mode"].empty()) {
            sendError(res, 400, "MISSING_FIELD", "'mode' array is required."); return;
        }
        if (!body.contains("source") || !body["source"].is_string()) {
            sendError(res, 400, "MISSING_FIELD", "'source' string is required."); return;
        }

        SNES::Debugger::Breakpoint bp;

        // source
        using BPSource = SNES::Debugger::Breakpoint::Source;
        std::string srcStr = body["source"].get<std::string>();
        if      (srcStr == "CPUBus")  bp.source = BPSource::CPUBus;
        else if (srcStr == "APURAM")  bp.source = BPSource::APURAM;
        else if (srcStr == "DSP")     bp.source = BPSource::DSP;
        else if (srcStr == "VRAM")    bp.source = BPSource::VRAM;
        else if (srcStr == "OAM")     bp.source = BPSource::OAM;
        else if (srcStr == "CGRAM")   bp.source = BPSource::CGRAM;
        else if (srcStr == "SA1Bus")  bp.source = BPSource::SA1Bus;
        else if (srcStr == "SFXBus")  bp.source = BPSource::SFXBus;
        else if (srcStr == "SGBBus")  bp.source = BPSource::SGBBus;
        else { sendError(res, 400, "INVALID_SOURCE", "Unknown source: " + srcStr); return; }

        // mode flags
        using BPMode = SNES::Debugger::Breakpoint::Mode;
        bp.mode = 0;
        for (auto& m : body["mode"]) {
            if (!m.is_string()) { sendError(res, 400, "INVALID_MODE", "mode elements must be strings."); return; }
            std::string ms = m.get<std::string>();
            if      (ms == "Exec")  bp.mode |= (unsigned)BPMode::Exec;
            else if (ms == "Read")  bp.mode |= (unsigned)BPMode::Read;
            else if (ms == "Write") bp.mode |= (unsigned)BPMode::Write;
            else { sendError(res, 400, "INVALID_MODE", "Unknown mode: " + ms); return; }
        }
        if (bp.mode == 0) { sendError(res, 400, "INVALID_MODE", "At least one mode is required."); return; }

        // addr (optional, default 0)
        if (body.contains("addr") && body["addr"].is_string()) {
            try { bp.addr = (unsigned)std::stoul(body["addr"].get<std::string>(), nullptr, 16); }
            catch (...) { sendError(res, 400, "INVALID_PARAM", "Cannot parse addr."); return; }
        }

        // addrEnd (optional)
        if (body.contains("addrEnd") && body["addrEnd"].is_string()) {
            try { bp.addr_end = (unsigned)std::stoul(body["addrEnd"].get<std::string>(), nullptr, 16); }
            catch (...) { sendError(res, 400, "INVALID_PARAM", "Cannot parse addrEnd."); return; }
        }

        // data (optional, default -1)
        if (body.contains("data") && body["data"].is_number_integer())
            bp.data = body["data"].get<int>();

        // compare (optional, default Equal)
        if (body.contains("compare") && body["compare"].is_string()) {
            using Cmp = SNES::Debugger::Breakpoint::Compare;
            std::string cmpStr = body["compare"].get<std::string>();
            if      (cmpStr == "Equal")        bp.compare = Cmp::Equal;
            else if (cmpStr == "NotEqual")     bp.compare = Cmp::NotEqual;
            else if (cmpStr == "Less")         bp.compare = Cmp::Less;
            else if (cmpStr == "LessEqual")    bp.compare = Cmp::LessEqual;
            else if (cmpStr == "Greater")      bp.compare = Cmp::Greater;
            else if (cmpStr == "GreaterEqual") bp.compare = Cmp::GreaterEqual;
            else { sendError(res, 400, "INVALID_COMPARE", "Unknown compare: " + cmpStr); return; }
        }

        json result;
        dispatch([this, bp, &result]() mutable {
            SNES::debugger.breakpoint.append(bp);
            int idx = (int)SNES::debugger.breakpoint.size() - 1;
            result = breakpointToJson(idx, SNES::debugger.breakpoint[idx]);
        }, true);
        sendJson(res, result, 201);
    });

    // ── DELETE /breakpoints/:index ───────────────────────────────────────────
    _svr->Delete("/breakpoints/:index", [this](const httplib::Request& req, httplib::Response& res) {
        int idx;
        try { idx = std::stoi(req.path_params.at("index")); }
        catch (...) { sendError(res, 400, "INVALID_PARAM", "index must be an integer."); return; }

        json result;
        bool found = false;
        dispatch([this, idx, &result, &found]() {
            if (idx < 0 || (unsigned)idx >= SNES::debugger.breakpoint.size()) return;
            found = true;
            result = breakpointToJson(idx, SNES::debugger.breakpoint[idx]);
            SNES::debugger.breakpoint.remove(idx);
        }, true);
        if (!found) { sendError(res, 404, "NOT_FOUND", "No breakpoint at index."); return; }
        sendJson(res, {{"deleted", result}});
    });

    // ── DELETE /breakpoints ──────────────────────────────────────────────────
    _svr->Delete("/breakpoints", [this](const httplib::Request&, httplib::Response& res) {
        int cleared = 0;
        dispatch([this, &cleared]() {
            cleared = (int)SNES::debugger.breakpoint.size();
            SNES::debugger.breakpoint.reset();
        }, true);
        sendJson(res, {{"cleared", cleared}});
    });

    // ── GET /openapi.json ────────────────────────────────────────────────────
    _svr->Get("/openapi.json", [](const httplib::Request&, httplib::Response& res) {
        static const std::string openapi = R"OPENAPI(
{
  "openapi": "3.0.3",
  "info": {
    "title": "bsnes-plus Debug API",
    "version": "1.0.0",
    "description": "Embedded HTTP debug API for bsnes-plus. Exposes CPU state, memory, breakpoints and execution control for external tooling. All SNES addresses are uppercase hex strings zero-padded to 6 digits (e.g. \"C0A3F2\"). Register values are uppercase hex strings. Flag values are booleans."
  },
  "servers": [{ "url": "http://127.0.0.1:5744" }],

  "components": {
    "schemas": {

      "CpuRegisters": {
        "type": "object",
        "description": "65C816 register values as uppercase hex strings.",
        "properties": {
          "pc": { "type": "string", "example": "C0A3F2" },
          "a":  { "type": "string", "example": "0042" },
          "x":  { "type": "string", "example": "0001" },
          "y":  { "type": "string", "example": "0000" },
          "s":  { "type": "string", "example": "01FF" },
          "d":  { "type": "string", "example": "0000" },
          "db": { "type": "string", "example": "C0" },
          "p":  { "type": "string", "example": "30" }
        }
      },

      "CpuFlags": {
        "type": "object",
        "description": "65C816 processor status flags.",
        "properties": {
          "e": { "type": "boolean", "description": "Emulation mode" },
          "n": { "type": "boolean", "description": "Negative" },
          "v": { "type": "boolean", "description": "Overflow" },
          "m": { "type": "boolean", "description": "Accumulator width (true=8-bit)" },
          "x": { "type": "boolean", "description": "Index width (true=8-bit)" },
          "d": { "type": "boolean", "description": "Decimal mode" },
          "i": { "type": "boolean", "description": "IRQ disable" },
          "z": { "type": "boolean", "description": "Zero" },
          "c": { "type": "boolean", "description": "Carry" }
        }
      },

      "CpuState": {
        "type": "object",
        "properties": {
          "registers": { "$ref": "#/components/schemas/CpuRegisters" },
          "flags":     { "$ref": "#/components/schemas/CpuFlags" }
        }
      },

      "BreakResult": {
        "type": "object",
        "description": "Returned by break and all step endpoints after execution halts.",
        "properties": {
          "paused":        { "type": "boolean" },
          "breakEvent":    { "type": "string",
                             "enum": ["CPUStep","BreakpointHit","SMPStep","SA1Step","SFXStep","SGBStep","None"] },
          "breakpointHit": { "type": "integer",
                             "description": "Index of triggered breakpoint, or -1." },
          "opcodeAddr":    { "type": "string", "example": "C0A3F2" },
          "disasm":        { "type": "string",
                             "example": "C0/A3F2 LDA #$01" },
          "cpu":           { "$ref": "#/components/schemas/CpuState" }
        }
      },

      "StatusResult": {
        "type": "object",
        "properties": {
          "paused":     { "type": "boolean" },
          "loaded":     { "type": "boolean",
                          "description": "True if a cartridge is loaded." },
          "breakEvent": { "type": "string",
                          "enum": ["CPUStep","BreakpointHit","SMPStep","SA1Step","SFXStep","SGBStep","None"] },
          "cpu":        { "description": "Null when not paused.",
                          "oneOf": [
                            { "$ref": "#/components/schemas/CpuState" },
                            { "type": "null" }
                          ]}
        }
      },

      "DisassemblyLine": {
        "type": "object",
        "properties": {
          "addr":  { "type": "string", "example": "C0A3F2" },
          "bytes": { "type": "array", "items": { "type": "integer" },
                     "description": "Raw opcode and operand bytes." },
          "text":  { "type": "string", "example": "C0/A3F2 LDA #$01" }
        }
      },

      "DisassemblyResult": {
        "type": "object",
        "properties": {
          "instructions": { "type": "array",
                            "items": { "$ref": "#/components/schemas/DisassemblyLine" } },
          "truncated":    { "type": "boolean",
                            "description": "True if the walk hit an untraced address before completing all requested lines." },
          "truncatedAt":  { "type": "string",
                            "description": "Address where truncation occurred. Present only when truncated is true.",
                            "example": "C0A412" }
        }
      },

      "UsageFlagEntry": {
        "type": "object",
        "properties": {
          "addr":   { "type": "string", "example": "C0A3F2" },
          "exec":   { "type": "boolean" },
          "opcode": { "type": "boolean" },
          "read":   { "type": "boolean" },
          "write":  { "type": "boolean" },
          "flagM":  { "type": "boolean",
                      "description": "M flag value recorded at last execution." },
          "flagX":  { "type": "boolean",
                      "description": "X flag value recorded at last execution." }
        }
      },

      "MemoryReadResult": {
        "type": "object",
        "properties": {
          "source": { "type": "string", "example": "cpu" },
          "addr":   { "type": "string", "example": "7E0000" },
          "count":  { "type": "integer" },
          "data":   { "type": "array", "items": { "type": "integer",
                                                   "minimum": 0,
                                                   "maximum": 255 } }
        }
      },

      "Breakpoint": {
        "type": "object",
        "properties": {
          "index":   { "type": "integer" },
          "addr":    { "type": "string", "example": "C0A3F2" },
          "addrEnd": { "type": "string",
                       "description": "End of address range. Omitted for single-address breakpoints." },
          "data":    { "type": "integer",
                       "description": "-1 means no data condition." },
          "compare": { "type": "string",
                       "enum": ["Equal","NotEqual","Less","LessEqual","Greater","GreaterEqual"] },
          "mode":    { "type": "array",
                       "items": { "type": "string", "enum": ["Exec","Read","Write"] } },
          "source":  { "type": "string",
                       "enum": ["CPUBus","APURAM","DSP","VRAM","OAM","CGRAM","SA1Bus","SFXBus","SGBBus"] },
          "counter": { "type": "integer",
                       "description": "Number of times this breakpoint has been hit." }
        }
      },

      "Error": {
        "type": "object",
        "properties": {
          "error":   { "type": "string", "description": "Machine-readable error code." },
          "message": { "type": "string", "description": "Human-readable description." }
        }
      }
    },

    "responses": {
      "NotPaused": {
        "description": "Emulator is not paused. POST /break first.",
        "content": { "application/json": {
          "schema": { "$ref": "#/components/schemas/Error" }
        }}
      },
      "NoCartridge": {
        "description": "No cartridge is loaded.",
        "content": { "application/json": {
          "schema": { "$ref": "#/components/schemas/Error" }
        }}
      }
    }
  },

  "paths": {

    "/status": {
      "get": {
        "summary": "Emulator status",
        "operationId": "getStatus",
        "description": "Returns current execution state. Safe to call at any time regardless of pause state.",
        "responses": {
          "200": { "description": "OK",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/StatusResult" }
                   }}}
        }
      }
    },

    "/break": {
      "post": {
        "summary": "Break execution",
        "operationId": "postBreak",
        "description": "Pauses the emulator immediately and returns the current CPU state.",
        "responses": {
          "200": { "description": "Execution paused",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/BreakResult" }
                   }}}
        }
      }
    },

    "/resume": {
      "post": {
        "summary": "Resume execution",
        "operationId": "postResume",
        "description": "Releases the emulator to run until the next break event. Returns immediately without waiting.",
        "responses": {
          "200": { "description": "Accepted",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                                 "properties": { "accepted": { "type": "boolean" } } }
                   }}}
        }
      }
    },

    "/run": {
      "post": {
        "summary": "Run at full speed",
        "operationId": "postRun",
        "description": "Exits debug mode entirely. The emulator runs without breaking on steps or breakpoints.",
        "responses": {
          "200": { "description": "Accepted",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                                 "properties": { "accepted": { "type": "boolean" } } }
                   }}}
        }
      }
    },

    "/step/into": {
      "post": {
        "summary": "Step into one instruction",
        "operationId": "stepInto",
        "description": "Executes exactly one CPU instruction and returns the resulting state. Blocks until complete.",
        "responses": {
          "200": { "description": "Step complete",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/BreakResult" }
                   }}},
          "408": { "description": "Step timed out (30s)" }
        }
      }
    },

    "/step/over": {
      "post": {
        "summary": "Step over one instruction",
        "operationId": "stepOver",
        "description": "Executes one instruction, stepping over JSR/JSL calls rather than entering them.",
        "responses": {
          "200": { "description": "Step complete",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/BreakResult" }
                   }}},
          "408": { "description": "Step timed out (30s)" }
        }
      }
    },

    "/step/out": {
      "post": {
        "summary": "Step out of subroutine",
        "operationId": "stepOut",
        "description": "Runs until the current subroutine returns (RTS/RTL/RTI).",
        "responses": {
          "200": { "description": "Step complete",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/BreakResult" }
                   }}},
          "408": { "description": "Step timed out (30s)" }
        }
      }
    },

    "/step/vblank": {
      "post": { "summary": "Run to next VBlank", "operationId": "stepVBlank",
        "responses": { "200": { "description": "Reached VBlank",
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/BreakResult" } } } },
          "408": { "description": "Timed out" } } }
    },
    "/step/hblank": {
      "post": { "summary": "Run to next HBlank", "operationId": "stepHBlank",
        "responses": { "200": { "description": "Reached HBlank",
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/BreakResult" } } } },
          "408": { "description": "Timed out" } } }
    },
    "/step/nmi": {
      "post": { "summary": "Run to next NMI", "operationId": "stepNMI",
        "responses": { "200": { "description": "Reached NMI",
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/BreakResult" } } } },
          "408": { "description": "Timed out" } } }
    },
    "/step/irq": {
      "post": { "summary": "Run to next IRQ", "operationId": "stepIRQ",
        "responses": { "200": { "description": "Reached IRQ",
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/BreakResult" } } } },
          "408": { "description": "Timed out" } } }
    },

    "/reset": {
      "post": {
        "summary": "Reset the SNES",
        "operationId": "reset",
        "description": "Sends a soft reset signal to the SNES. Requires a loaded cartridge with power on. The emulator continues running after reset — use POST /break to pause at the reset vector.",
        "responses": {
          "200": { "description": "Reset sent",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": { "accepted": { "type": "boolean" } } }
                   }}},
          "409": { "description": "No cartridge loaded or power is off." }
        }
      }
    },

    "/reload": {
      "post": {
        "summary": "Reload the current ROM from disk",
        "operationId": "reload",
        "description": "Reloads the currently loaded cartridge file from disk, resetting all emulator state. Useful after modifying the ROM file externally.",
        "responses": {
          "200": { "description": "Reloaded",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "accepted": { "type": "boolean" },
                         "name": { "type": "string" }
                       }}
                   }}},
          "409": { "description": "No ROM currently loaded." }
        }
      }
    },

    "/power-cycle": {
      "post": {
        "summary": "Power cycle the SNES",
        "operationId": "powerCycle",
        "description": "Performs a hard reset (power off then power on). Equivalent to flipping the physical power switch. Requires a loaded cartridge.",
        "responses": {
          "200": { "description": "Power cycled",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": { "accepted": { "type": "boolean" } } }
                   }}},
          "409": { "description": "No cartridge is loaded." }
        }
      }
    },

    "/cartridge/load": {
      "post": {
        "summary": "Load a cartridge from a filesystem path",
        "operationId": "loadCartridge",
        "description": "Loads a ROM file by absolute filesystem path. Unloads any currently loaded cartridge first, saving its SRAM. Supports .sfc files. Applies BPS/UPS/IPS patches automatically if present alongside the ROM.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["path"],
              "properties": {
                "path": { "type": "string",
                          "description": "Absolute filesystem path to the ROM file.",
                          "example": "/home/user/roms/secret_of_mana.sfc" }
              }}
          }}
        },
        "responses": {
          "200": { "description": "Cartridge loaded",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "loaded": { "type": "boolean" },
                         "name":   { "type": "string",
                                     "description": "Game title decoded from the ROM header." },
                         "path":   { "type": "string" }
                       }}
                   }}},
          "400": { "description": "Missing or invalid request body." },
          "422": { "description": "File not found or not a valid ROM." }
        }
      }
    },

    "/cpu/registers": {
      "get": {
        "summary": "Read CPU registers and flags",
        "operationId": "getRegisters",
        "description": "Returns all 65C816 registers and processor flags. Requires paused state.",
        "responses": {
          "200": { "description": "OK",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/CpuState" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" },
          "503": { "$ref": "#/components/responses/NoCartridge" }
        }
      },
      "put": {
        "summary": "Write CPU registers and/or flags",
        "operationId": "putRegisters",
        "description": "Sets one or more registers or flags. Omitted fields are unchanged. Flags must be nested inside a 'flags' object. Requires paused state.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": {
              "type": "object",
              "description": "Any combination of register (hex string) fields and an optional 'flags' object.",
              "properties": {
                "pc": { "type": "string" }, "a": { "type": "string" },
                "x":  { "type": "string" }, "y": { "type": "string" },
                "s":  { "type": "string" }, "d": { "type": "string" },
                "db": { "type": "string" }, "p": { "type": "string" },
                "flags": {
                  "type": "object",
                  "description": "Processor status flags to update.",
                  "properties": {
                    "e": { "type": "boolean" }, "n": { "type": "boolean" },
                    "v": { "type": "boolean" }, "m": { "type": "boolean" },
                    "x": { "type": "boolean" }, "d": { "type": "boolean" },
                    "i": { "type": "boolean" }, "z": { "type": "boolean" },
                    "c": { "type": "boolean" }
                  }
                }
              }
            }
          }}
        },
        "responses": {
          "200": { "description": "Updated state",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/CpuState" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" }
        }
      }
    },

    "/cpu/disassemble": {
      "get": {
        "summary": "Disassemble instructions at a SNES address",
        "operationId": "disassemble",
        "description": "Returns up to 'lines' disassembled instructions starting at 'addr'. The starting address must have been executed at least once (has UsageOpcode set). Returns 422 if the address is untraced. The walk stops early and sets truncated=true if it reaches an untraced address.",
        "parameters": [
          { "name": "addr", "in": "query", "required": true,
            "schema": { "type": "string" }, "example": "C0A3F2",
            "description": "SNES address in hex." },
          { "name": "lines", "in": "query", "required": false,
            "schema": { "type": "integer", "default": 10, "maximum": 256 },
            "description": "Maximum number of instructions to return." }
        ],
        "responses": {
          "200": { "description": "Disassembly result",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/DisassemblyResult" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" },
          "422": { "description": "Address has not been executed. Run to it first.",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/Error" }
                   }}}
        }
      }
    },

    "/cpu/usage": {
      "get": {
        "summary": "CPU usage map",
        "operationId": "getUsage",
        "description": "Returns per-byte execution history flags for a range of SNES addresses.",
        "parameters": [
          { "name": "addr",  "in": "query", "required": true,
            "schema": { "type": "string" }, "description": "Start SNES address in hex." },
          { "name": "count", "in": "query", "required": true,
            "schema": { "type": "integer" }, "description": "Number of bytes to return." }
        ],
        "responses": {
          "200": { "description": "Usage flags",
                   "content": { "application/json": {
                     "schema": { "type": "array",
                                 "items": { "$ref": "#/components/schemas/UsageFlagEntry" } }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" }
        }
      }
    },

    "/memory/{source}": {
      "get": {
        "summary": "Read memory",
        "operationId": "readMemory",
        "description": "Reads bytes from the specified memory bus. Requires paused state. Count is capped at 4096.",
        "parameters": [
          { "name": "source", "in": "path", "required": true,
            "schema": { "type": "string",
                        "enum": ["cpu","apu","apuram","dsp","vram","oam","cgram","cartrom","cartram"] } },
          { "name": "addr",  "in": "query", "required": true,
            "schema": { "type": "string" }, "description": "Start address in hex." },
          { "name": "count", "in": "query", "required": false,
            "schema": { "type": "integer", "default": 256, "maximum": 4096 } }
        ],
        "responses": {
          "200": { "description": "Memory contents",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/MemoryReadResult" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" }
        }
      },
      "put": {
        "summary": "Write memory",
        "operationId": "writeMemory",
        "description": "Writes bytes to the specified memory bus. Requires paused state. Limited to 4096 bytes per call.",
        "parameters": [
          { "name": "source", "in": "path", "required": true,
            "schema": { "type": "string",
                        "enum": ["cpu","apu","apuram","dsp","vram","oam","cgram","cartrom","cartram"] } },
          { "name": "addr", "in": "query", "required": true,
            "schema": { "type": "string" }, "description": "Start address in hex." }
        ],
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["data"],
              "properties": {
                "data": { "type": "array",
                          "items": { "type": "integer", "minimum": 0, "maximum": 255 },
                          "maxItems": 4096 }
              }}
          }}
        },
        "responses": {
          "200": { "description": "Write confirmed",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "written": { "type": "integer" },
                         "addr":    { "type": "string" }
                       }}
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" }
        }
      }
    },

    "/breakpoints": {
      "get": {
        "summary": "List all breakpoints",
        "operationId": "listBreakpoints",
        "responses": {
          "200": { "description": "Breakpoint list",
                   "content": { "application/json": {
                     "schema": { "type": "array",
                                 "items": { "$ref": "#/components/schemas/Breakpoint" } }
                   }}}
        }
      },
      "post": {
        "summary": "Add a breakpoint",
        "operationId": "addBreakpoint",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": {
              "type": "object",
              "required": ["mode", "source"],
              "properties": {
                "addr":    { "type": "string", "description": "Hex address (default 000000)." },
                "addrEnd": { "type": "string", "description": "End of range (optional)." },
                "data":    { "type": "integer", "description": "Data condition value. -1 disables.", "default": -1 },
                "compare": { "type": "string",
                             "enum": ["Equal","NotEqual","Less","LessEqual","Greater","GreaterEqual"],
                             "default": "Equal" },
                "mode":    { "type": "array",
                             "items": { "type": "string", "enum": ["Exec","Read","Write"] },
                             "minItems": 1 },
                "source":  { "type": "string",
                             "enum": ["CPUBus","APURAM","DSP","VRAM","OAM","CGRAM","SA1Bus","SFXBus","SGBBus"] }
              }
            }
          }}
        },
        "responses": {
          "201": { "description": "Breakpoint created",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/Breakpoint" }
                   }}}
        }
      },
      "delete": {
        "summary": "Clear all breakpoints",
        "operationId": "clearBreakpoints",
        "responses": {
          "200": { "description": "All cleared",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": { "cleared": { "type": "integer" } } }
                   }}}
        }
      }
    },

    "/breakpoints/{index}": {
      "delete": {
        "summary": "Remove a breakpoint by index",
        "operationId": "deleteBreakpoint",
        "parameters": [
          { "name": "index", "in": "path", "required": true,
            "schema": { "type": "integer" } }
        ],
        "responses": {
          "200": { "description": "Breakpoint removed",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": { "deleted": { "$ref": "#/components/schemas/Breakpoint" } } }
                   }}},
          "404": { "description": "No breakpoint at that index." }
        }
      }
    }
  }
}
)OPENAPI";
        res.status = 200;
        res.set_content(openapi, "application/json");
    });

    // ── GET /scalar ──────────────────────────────────────────────────────────
    _svr->Get("/scalar", [](const httplib::Request&, httplib::Response& res) {
        static const std::string html = R"(<!DOCTYPE html>
<html>
<head>
  <title>bsnes-plus Debug API</title>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
</head>
<body>
  <script id="api-reference" data-url="/openapi.json"></script>
  <script src="https://cdn.jsdelivr.net/npm/@scalar/api-reference"></script>
</body>
</html>)";
        res.status = 200;
        res.set_content(html, "text/html");
    });

} // end setupRoutes()
