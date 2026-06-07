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
    _svr = std::make_unique<httplib::Server>();
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
    using R = CPUDebugger::Register;
    using F = CPUDebugger;
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
// Returns an array of { addr, bytes, text } for 'lines' instructions starting
// at 'addr'. Uses the CPU usage array to find instruction boundaries; if no
// usage data exists for a location, treats each unknown byte as 1-byte.
// Must be called from the Qt main thread.

json BsnesApiServer::disassembleAt(uint32_t addr, int lines) {
    json result = json::array();

    for (int i = 0; i < lines; ++i) {
        addr &= 0xFFFFFF;
        char buf[256];
        SNES::cpu.disassemble_opcode(buf, addr, false);

        // Determine instruction length by scanning the usage array for the
        // next UsageOpcode marker within the next 1-4 bytes.
        int len = 1;
        for (int k = 1; k <= 4; ++k) {
            uint32_t next = (addr + k) & 0xFFFFFF;
            if (SNES::cpu.usage[next] & CPUDebugger::UsageOpcode) {
                len = k;
                break;
            }
        }

        json bytes = json::array();
        for (int b = 0; b < len; ++b) {
            bytes.push_back((int)SNES::debugger.read(
                SNES::Debugger::MemorySource::CPUBus, (addr + b) & 0xFFFFFF));
        }

        result.push_back({
            {"addr",  hexStr(addr, 6)},
            {"bytes", bytes},
            {"text",  std::string(buf)},
        });

        addr = (addr + len) & 0xFFFFFF;
    }
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
            using R = CPUDebugger::Register;
            using F = CPUDebugger;
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
            sendError(res, 400, "MISSING_PARAM", "Query param 'addr' is required."); return;
        }
        uint32_t addr;
        int lines = 10;
        try {
            addr  = (uint32_t)std::stoul(req.get_param_value("addr"), nullptr, 16);
            if (req.has_param("lines"))
                lines = std::stoi(req.get_param_value("lines"));
        } catch (...) {
            sendError(res, 400, "INVALID_PARAM", "Could not parse addr or lines."); return;
        }
        lines = std::min(std::max(lines, 1), 256);
        json result;
        dispatch([this, &result, addr, lines]() {
            result = disassembleAt(addr, lines);
        }, true);
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
            using U = CPUDebugger;
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
        dispatch([this, srcName, addr, data]() {
            writeMemory(srcName, addr, data);
        }, true);
        sendJson(res, {{"written", (int)data.size()}, {"addr", hexStr(addr, 6)}});
    });
