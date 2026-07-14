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
                             std::chrono::seconds timeout,
                             StepTarget target) {
    // Reset the break flag before triggering — avoids a stale notification.
    {
        std::lock_guard<std::mutex> lock(_breakMutex);
        _breakOccurred = false;
    }

    // Dispatch step setup to the Qt main thread (non-blocking — the emulator
    // must be free to run; a blocking dispatch would deadlock).
    // step_cpu / step_smp select which processor the break lands on. Both are
    // set explicitly so the target is deterministic regardless of prior state
    // (a preceding SMP step leaves step_smp set, and vice versa).
    dispatch([type, stepOverNew, target]() {
        SNES::debugger.step_type    = type;
        SNES::debugger.step_cpu     = (target == StepTarget::CPU);
        SNES::debugger.step_smp     = (target == StepTarget::SMP);
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

// ── getSmpStateJson ───────────────────────────────────────────────────────────
// SPC700 (S-SMP) state. Must be called from the Qt main thread (emulator paused).

json BsnesApiServer::getSmpStateJson() {
    using R = SNES::SMPDebugger::Register;
    using F = SNES::SMPDebugger;
    auto& smp = SNES::smp;

    json regs = {
        {"pc", hexStr(smp.getRegister(R::RegisterPC), 4)},
        {"a",  hexStr(smp.getRegister(R::RegisterA),  2)},
        {"x",  hexStr(smp.getRegister(R::RegisterX),  2)},
        {"y",  hexStr(smp.getRegister(R::RegisterY),  2)},
        {"sp", hexStr(smp.getRegister(R::RegisterS),  2)},
        {"p",  hexStr(smp.getRegister(R::RegisterP),  2)},
    };

    json flags = {
        {"n", smp.getFlag(F::FlagN)},
        {"v", smp.getFlag(F::FlagV)},
        {"p", smp.getFlag(F::FlagP)},
        {"b", smp.getFlag(F::FlagB)},
        {"h", smp.getFlag(F::FlagH)},
        {"i", smp.getFlag(F::FlagI)},
        {"z", smp.getFlag(F::FlagZ)},
        {"c", smp.getFlag(F::FlagC)},
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
        {"smp",           getSmpStateJson()},
    };
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

// ── dumpMemoryToFile ──────────────────────────────────────────────────────────
// Writes 'count' bytes from memory bus 'source' starting at 'addr' into the
// file at 'path' as raw binary. No 4096-byte cap — this is the whole point.
// Returns bytes written, -1 on file-open failure, -2 on unknown source.
// Must be called from the Qt main thread.

long BsnesApiServer::dumpMemoryToFile(const std::string& source,
                                       uint32_t addr, uint32_t count,
                                       const std::string& path) {
    SNES::Debugger::MemorySource src;
    if (!parseSource(source, src)) return -2;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return -1;

    const uint32_t CHUNK = 8192;
    std::vector<uint8_t> buf;
    buf.reserve(CHUNK);
    uint32_t written = 0;
    while (written < count) {
        uint32_t n = std::min(CHUNK, count - written);
        buf.clear();
        for (uint32_t i = 0; i < n; ++i)
            buf.push_back((uint8_t)SNES::debugger.read(src, addr + written + i));
        fwrite(buf.data(), 1, n, f);
        written += n;
    }
    fclose(f);
    return (long)written;
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

    // ── POST /state/load ─────────────────────────────────────────────────────
    _svr->Post("/state/load", [this](const httplib::Request& req,
                                     httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            sendError(res, 400, "INVALID_BODY", "Expected JSON body."); return;
        }
        if (!body.contains("slot") || !body["slot"].is_number_integer()) {
            sendError(res, 400, "MISSING_PARAM", "'slot' (integer) is required."); return;
        }
        int slot = body["slot"].get<int>();
        if (slot < 1) {
            sendError(res, 400, "INVALID_SLOT",
                      "'slot' must be >= 1 (matches the -N.bst filename suffix)."); return;
        }

        bool ready = false, ok = false;
        dispatch([&]() {
            if (!SNES::cartridge.loaded() || !application.power) return;
            ready = true;
            // State::load is 0-based; slot N loads <rom>-N.bst (State::name adds slot+1).
            ok = state.load((unsigned)(slot - 1));
        }, true);

        if (!ready) {
            sendError(res, 409, "NOT_READY",
                      "Load state requires a loaded cartridge with power on.");
            return;
        }
        if (!ok) {
            sendError(res, 422, "LOAD_FAILED",
                      "Failed to load state slot " + std::to_string(slot) +
                      " (the -N.bst file may not exist, or save states are unsupported "
                      "for this cartridge).");
            return;
        }
        sendJson(res, {{"loaded", true}, {"slot", slot}});
    });

    // ── POST /state/load-file ────────────────────────────────────────────────
    _svr->Post("/state/load-file", [this](const httplib::Request& req,
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

        bool ready = false, ok = false;
        dispatch([&]() {
            if (!SNES::cartridge.loaded() || !application.power) return;
            ready = true;
            ok = state.loadFromPath(path.c_str());
        }, true);

        if (!ready) {
            sendError(res, 409, "NOT_READY",
                      "Load state requires a loaded cartridge with power on.");
            return;
        }
        if (!ok) {
            sendError(res, 422, "LOAD_FAILED",
                      "Failed to load state from '" + path + "' (file may not exist, "
                      "or is not a valid save state for the loaded cartridge).");
            return;
        }
        sendJson(res, {{"loaded", true}, {"path", path}});
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

    // ── GET /cpu/disassemble — CURRENT INSTRUCTION ONLY ──────────────────────
    // Returns only the single instruction at the current PC, disassembled with
    // the live M/X state at that PC. No look-ahead window: reading ahead and
    // statically decoding future instructions is unreliable and not permitted.
    // To see the next instruction, step and call this again.
    _svr->Get("/cpu/disassemble", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;

        // The look-ahead window was removed. Reject the old parameters loudly
        // rather than silently ignoring them.
        if (req.has_param("addr") || req.has_param("lines")) {
            sendError(res, 400, "PARAMS_REMOVED",
                      "/cpu/disassemble no longer accepts 'addr' or 'lines'. "
                      "It returns only the instruction at the current PC. "
                      "Step to the instruction you want, then call with no params.");
            return;
        }

        json result;
        dispatch([this, &result]() {
            uint32_t pc = SNES::cpu.opcode_pc & 0xFFFFFF;

            char buf[256];
            SNES::cpu.disassemble_opcode(buf, pc, false);

            // Instruction length from the usage map (current PC is, by
            // definition, an executed opcode).
            int len = 1;
            for (int k = 1; k <= 4; ++k) {
                uint32_t next = (pc + k) & 0xFFFFFF;
                if (SNES::cpu.usage[next] & SNES::CPUDebugger::UsageOpcode) {
                    len = k;
                    break;
                }
            }

            json bytes = json::array();
            for (int b = 0; b < len; ++b)
                bytes.push_back((int)SNES::debugger.read(
                    SNES::Debugger::MemorySource::CPUBus, (pc + b) & 0xFFFFFF));

            result = {
                {"addr",  hexStr(pc, 6)},
                {"bytes", bytes},
                {"text",  std::string(buf)},
            };
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

    // ── GET /smp/registers ───────────────────────────────────────────────────
    _svr->Get("/smp/registers", [this](const httplib::Request&, httplib::Response& res) {
        if (!requirePaused(res)) return;
        json result;
        dispatch([this, &result]() {
            result = getSmpStateJson();
        }, true);
        sendJson(res, result);
    });

    // ── PUT /smp/registers ───────────────────────────────────────────────────
    _svr->Put("/smp/registers", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;
        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_JSON", "Body is not valid JSON."); return; }

        json result;
        dispatch([this, &body, &result]() {
            using R = SNES::SMPDebugger::Register;
            using F = SNES::SMPDebugger;
            auto& smp = SNES::smp;

            // Parse hex-string register values
            auto setReg = [&](const std::string& key, R reg) {
                if (body.contains(key) && body[key].is_string()) {
                    unsigned val = (unsigned)std::stoul(body[key].get<std::string>(), nullptr, 16);
                    smp.setRegister((unsigned)reg, val);
                }
            };
            setReg("pc", R::RegisterPC);
            setReg("a",  R::RegisterA);
            setReg("x",  R::RegisterX);
            setReg("y",  R::RegisterY);
            setReg("sp", R::RegisterS);
            setReg("p",  R::RegisterP);

            // Parse boolean flag values from nested "flags" object
            if (body.contains("flags") && body["flags"].is_object()) {
                auto& f = body["flags"];
                if (f.contains("n") && f["n"].is_boolean()) smp.setFlag(F::FlagN, f["n"].get<bool>());
                if (f.contains("v") && f["v"].is_boolean()) smp.setFlag(F::FlagV, f["v"].get<bool>());
                if (f.contains("p") && f["p"].is_boolean()) smp.setFlag(F::FlagP, f["p"].get<bool>());
                if (f.contains("b") && f["b"].is_boolean()) smp.setFlag(F::FlagB, f["b"].get<bool>());
                if (f.contains("h") && f["h"].is_boolean()) smp.setFlag(F::FlagH, f["h"].get<bool>());
                if (f.contains("i") && f["i"].is_boolean()) smp.setFlag(F::FlagI, f["i"].get<bool>());
                if (f.contains("z") && f["z"].is_boolean()) smp.setFlag(F::FlagZ, f["z"].get<bool>());
                if (f.contains("c") && f["c"].is_boolean()) smp.setFlag(F::FlagC, f["c"].get<bool>());
            }
            result = getSmpStateJson();
        }, true);
        sendJson(res, result);
    });

    // ── GET /smp/disassemble — CURRENT INSTRUCTION ONLY ──────────────────────
    // Returns only the single instruction at the current SMP PC. Same strict
    // no-look-ahead contract as /cpu/disassemble: to see the next instruction,
    // step the SMP and call again.
    _svr->Get("/smp/disassemble", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;

        // Reject the old look-ahead parameters loudly rather than ignoring them.
        if (req.has_param("addr") || req.has_param("lines")) {
            sendError(res, 400, "PARAMS_REMOVED",
                      "/smp/disassemble does not accept 'addr' or 'lines'. "
                      "It returns only the instruction at the current SMP PC. "
                      "Step to the instruction you want, then call with no params.");
            return;
        }

        json result;
        dispatch([this, &result]() {
            uint16_t pc = SNES::smp.opcode_pc;

            char buf[256];
            SNES::smp.disassemble_opcode(buf, pc);

            // Instruction length from the usage map (current PC is, by
            // definition, an executed opcode). Wraps within the 16-bit space.
            int len = 1;
            for (int k = 1; k <= 4; ++k) {
                uint16_t next = (uint16_t)(pc + k);
                if (SNES::smp.usage[next] & SNES::SMPDebugger::UsageOpcode) {
                    len = k;
                    break;
                }
            }

            json bytes = json::array();
            for (int b = 0; b < len; ++b)
                bytes.push_back((int)SNES::debugger.read(
                    SNES::Debugger::MemorySource::APURAM, (uint16_t)(pc + b)));

            result = {
                {"addr",  hexStr(pc, 4)},
                {"bytes", bytes},
                {"text",  std::string(buf)},
            };
        }, true);

        sendJson(res, result);
    });

    // ── POST /smp/step/into ──────────────────────────────────────────────────
    _svr->Post("/smp/step/into", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepInto,
                             false, std::chrono::seconds(30), StepTarget::SMP);
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /smp/step/over ──────────────────────────────────────────────────
    _svr->Post("/smp/step/over", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepOver,
                             /*stepOverNew=*/true, std::chrono::seconds(30), StepTarget::SMP);
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── POST /smp/step/out ───────────────────────────────────────────────────
    _svr->Post("/smp/step/out", [this](const httplib::Request&, httplib::Response& res) {
        auto result = doStep(SNES::Debugger::StepType::StepOut,
                             false, std::chrono::seconds(30), StepTarget::SMP);
        int status = result.contains("code") ? 408 : 200;
        sendJson(res, result, status);
    });

    // ── GET /wram/provenance?addr=7E8000&count=256 ───────────────────────────
    _svr->Get("/wram/provenance", [this](const httplib::Request& req,
                                          httplib::Response& res) {
        if (!requirePaused(res)) return;
        if (!req.has_param("addr")) {
            sendError(res, 400, "MISSING_PARAM", "'addr' is required.");
            return;
        }

        uint32_t addr;
        int count = 256;
        try {
            addr  = (uint32_t)std::stoul(req.get_param_value("addr"),
                                         nullptr, 16);
            if (req.has_param("count"))
                count = std::stoi(req.get_param_value("count"));
        } catch (...) {
            sendError(res, 400, "INVALID_PARAM", "Cannot parse addr or count.");
            return;
        }

        if (wramOffset(addr) < 0) {
            sendError(res, 400, "NOT_WRAM",
                      "Address " + hexStr(addr, 6) +
                      " is not a WRAM address. Provide a $7Exxxx/$7Fxxxx address.");
            return;
        }

        count = std::max(1, std::min(count, 4096));

        json result;
        dispatch([this, &result, addr, count]() {
            json provenance = json::array();
            for (int i = 0; i < count; i++) {
                uint32_t wramAddr = addr + i;
                int32_t  woff     = wramOffset(wramAddr);
                if (woff < 0 || woff >= (int32_t)WRAM_SIZE) {
                    provenance.push_back(nullptr);
                    continue;
                }
                uint32_t src = SNES::cpu.wramShadow[woff];
                if (src == SHADOW_SENTINEL)
                    provenance.push_back(nullptr);
                else
                    provenance.push_back(hexStr(src, 6));
            }
            result = {
                {"addr",       hexStr(addr, 6)},
                {"count",      count},
                {"provenance", provenance}
            };
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

    // ── POST /memory/{source}/dump  body: {"addr":"7E8000","count":22227,"path":"/tmp/out.bin"} ──
    _svr->Post("/memory/:source/dump", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requirePaused(res)) return;
        std::string srcName = req.path_params.at("source");
        SNES::Debugger::MemorySource src;
        if (!parseSource(srcName, src)) {
            sendError(res, 400, "INVALID_SOURCE", "Unknown memory source: " + srcName);
            return;
        }

        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_BODY", "Expected JSON body."); return; }

        if (!body.contains("addr") || !body.contains("count") || !body.contains("path")) {
            sendError(res, 400, "MISSING_PARAM", "'addr', 'count', and 'path' are required.");
            return;
        }

        uint32_t addr, count;
        std::string path;
        try {
            addr  = (uint32_t)std::stoul(body["addr"].get<std::string>(), nullptr, 16);
            count = body["count"].get<uint32_t>();
            path  = body["path"].get<std::string>();
        } catch (...) {
            sendError(res, 400, "INVALID_PARAM",
                      "addr must be a hex string, count must be an integer, path must be a string.");
            return;
        }
        if (count == 0 || count > 0x1000000) {
            sendError(res, 400, "INVALID_PARAM", "count must be between 1 and 16777216.");
            return;
        }

        long result = 0;
        dispatch([this, &result, srcName, addr, count, path]() {
            result = dumpMemoryToFile(srcName, addr, count, path);
        }, true);

        if (result == -1) {
            sendError(res, 500, "FILE_ERROR", "Could not open file for writing: " + path);
            return;
        }
        if (result == -2) {
            sendError(res, 400, "BAD_SOURCE", "Unknown memory source.");
            return;
        }
        sendJson(res, {
            {"source",  srcName},
            {"addr",    hexStr(addr, 6)},
            {"count",   (int)count},
            {"path",    path},
            {"written", (int)result}
        });
    });

    // ── POST /screen/dump  body: {"path":"/tmp/screen.png"} ──────────────────
    // Writes the most recently rendered frame as a PNG. Does not require paused
    // state — a dump while running captures the last rendered frame.
    _svr->Post("/screen/dump", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireLoaded(res)) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_BODY", "Expected JSON body."); return; }

        if (!body.contains("path")) {
            sendError(res, 400, "MISSING_PARAM", "'path' is required.");
            return;
        }
        std::string path = body["path"].get<std::string>();

        bool ok = false;
        bool hadFrame = false;
        int w = 0, h = 0;
        dispatch([&]() {
            hadFrame = !interface.lastFrame.isNull();
            if (hadFrame) {
                w = interface.lastFrame.width();
                h = interface.lastFrame.height();
                ok = interface.saveScreenToFile(path.c_str());
            }
        }, true);

        if (!hadFrame) {
            sendError(res, 409, "NO_FRAME",
                      "No frame has been rendered yet. Run the emulator briefly, "
                      "then pause and retry.");
            return;
        }
        if (!ok) {
            sendError(res, 500, "FILE_ERROR", "Could not write PNG to: " + path);
            return;
        }
        sendJson(res, { {"path", path}, {"width", w}, {"height", h} });
    });

    // ── POST /input/press ────────────────────────────────────────────────────
    // Holds buttons on controller port 1 for `frames` frames while running the
    // emulator so the game actually polls the held input, then releases.
    _svr->Post("/input/press", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireLoaded(res)) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_BODY", "Expected JSON body."); return; }

        if (!body.contains("buttons") || !body["buttons"].is_array()
                || body["buttons"].empty()) {
            sendError(res, 400, "MISSING_PARAM",
                      "'buttons' (non-empty array of names) is required.");
            return;
        }
        int frames = body.value("frames", 4);
        if (frames < 1 || frames > 600) {
            sendError(res, 400, "INVALID_PARAM", "'frames' must be 1..600.");
            return;
        }

        static const std::map<std::string,int> BTN = {
            {"B",0},{"Y",1},{"Select",2},{"Start",3},
            {"Up",4},{"Down",5},{"Left",6},{"Right",7},
            {"A",8},{"X",9},{"L",10},{"R",11}
        };
        uint16_t mask = 0;
        for (auto& b : body["buttons"]) {
            if (!b.is_string()) {
                sendError(res, 400, "BAD_BUTTON", "Button names must be strings.");
                return;
            }
            auto it = BTN.find(b.get<std::string>());
            if (it == BTN.end()) {
                sendError(res, 400, "BAD_BUTTON",
                          "Unknown button: " + b.get<std::string>() +
                          ". Valid: B Y Select Start Up Down Left Right A X L R");
                return;
            }
            mask |= (1u << it->second);
        }
        if (((mask>>4&1) && (mask>>5&1)) || ((mask>>6&1) && (mask>>7&1))) {
            sendError(res, 400, "OPPOSING_DIRS",
                      "Cannot hold Up+Down or Left+Right simultaneously.");
            return;
        }

        dispatch([mask]() { interface.setInputOverride(mask); }, /*blocking=*/true);

        // Advance one frame (StepToVBlank) at a time. A breakpoint can fire mid-frame
        // — e.g. code that only runs once the held input reaches the game. The doStep
        // result is a snapshot built (buildBreakResult) before break_event is reset at
        // application.cpp, so its "breakEvent" reliably reports "BreakpointHit" here.
        // On a breakpoint we stop early, leave the emulator paused there, and do not
        // count that frame; on timeout we bail with 408.
        bool timedOut      = false;
        bool hitBreakpoint = false;
        int  framesRun     = 0;
        json breakResult;
        for (int i = 0; i < frames; ++i) {
            auto result = doStep(SNES::Debugger::StepType::StepToVBlank,
                                 false, std::chrono::seconds(5));
            if (result.contains("code")) {
                timedOut = true;
                break;
            }
            if (result.value("breakEvent", std::string()) == "BreakpointHit") {
                hitBreakpoint = true;
                breakResult   = std::move(result);
                break;   // do not re-arm — leave the emulator paused at the breakpoint
            }
            ++framesRun;   // vblank reached (CPUStep) = one frame completed
        }

        // press_buttons keeps its "hold then release" contract: release on every exit,
        // breakpoint included. Use /input/hold + /resume to hold input across a break.
        dispatch([]() { interface.clearInputOverride(); }, /*blocking=*/true);

        if (timedOut) {
            sendError(res, 408, "FRAME_TIMEOUT",
                      "Timed out waiting for a frame boundary. "
                      "The emulator may be stuck or in an infinite loop.");
            return;
        }
        if (hitBreakpoint) {
            sendJson(res, {
                {"held",            body["buttons"]},
                {"framesRequested", frames},
                {"framesRun",       framesRun},
                {"stopped",         "breakpoint"},
                {"breakpointHit",   breakResult["breakpointHit"]},
                {"opcodeAddr",      breakResult["opcodeAddr"]},
                {"disasm",          breakResult["disasm"]},
                {"cpu",             breakResult["cpu"]},
            });
            return;
        }
        sendJson(res, {
            {"held",            body["buttons"]},
            {"framesRequested", frames},
            {"framesRun",       framesRun},
            {"stopped",         "completed"},
        });
    });

    // ── POST /input/hold ─────────────────────────────────────────────────────
    // Sets the input override without advancing frames. The override stays
    // active until POST /input/release. Use /input/press for the common case.
    _svr->Post("/input/hold", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireLoaded(res)) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { sendError(res, 400, "INVALID_BODY", "Expected JSON body."); return; }

        if (!body.contains("buttons") || !body["buttons"].is_array()
                || body["buttons"].empty()) {
            sendError(res, 400, "MISSING_PARAM",
                      "'buttons' (non-empty array of names) is required.");
            return;
        }

        static const std::map<std::string,int> BTN = {
            {"B",0},{"Y",1},{"Select",2},{"Start",3},
            {"Up",4},{"Down",5},{"Left",6},{"Right",7},
            {"A",8},{"X",9},{"L",10},{"R",11}
        };
        uint16_t mask = 0;
        for (auto& b : body["buttons"]) {
            if (!b.is_string()) {
                sendError(res, 400, "BAD_BUTTON", "Button names must be strings.");
                return;
            }
            auto it = BTN.find(b.get<std::string>());
            if (it == BTN.end()) {
                sendError(res, 400, "BAD_BUTTON",
                          "Unknown button: " + b.get<std::string>());
                return;
            }
            mask |= (1u << it->second);
        }
        if (((mask>>4&1) && (mask>>5&1)) || ((mask>>6&1) && (mask>>7&1))) {
            sendError(res, 400, "OPPOSING_DIRS",
                      "Cannot hold Up+Down or Left+Right simultaneously.");
            return;
        }

        dispatch([mask]() { interface.setInputOverride(mask); }, /*blocking=*/true);
        sendJson(res, { {"holding", body["buttons"]} });
    });

    // ── POST /input/release ──────────────────────────────────────────────────
    // Clears the input override set by /input/hold.
    _svr->Post("/input/release", [this](const httplib::Request&, httplib::Response& res) {
        if (!requireLoaded(res)) return;
        dispatch([]() { interface.clearInputOverride(); }, /*blocking=*/true);
        sendJson(res, { {"released", true} });
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

      "SmpRegisters": {
        "type": "object",
        "description": "SPC700 (S-SMP) register values as uppercase hex strings.",
        "properties": {
          "pc": { "type": "string", "example": "0200", "description": "16-bit program counter." },
          "a":  { "type": "string", "example": "12", "description": "8-bit accumulator." },
          "x":  { "type": "string", "example": "00", "description": "8-bit X index." },
          "y":  { "type": "string", "example": "00", "description": "8-bit Y index." },
          "sp": { "type": "string", "example": "EF", "description": "8-bit stack pointer (stack lives in page $01xx)." },
          "p":  { "type": "string", "example": "02", "description": "Processor status word (PSW) byte." }
        }
      },

      "SmpFlags": {
        "type": "object",
        "description": "SPC700 processor status word (PSW) flags.",
        "properties": {
          "n": { "type": "boolean", "description": "Negative" },
          "v": { "type": "boolean", "description": "Overflow" },
          "p": { "type": "boolean", "description": "Direct page select (true=page $01xx)" },
          "b": { "type": "boolean", "description": "Break" },
          "h": { "type": "boolean", "description": "Half-carry" },
          "i": { "type": "boolean", "description": "Interrupt enable" },
          "z": { "type": "boolean", "description": "Zero" },
          "c": { "type": "boolean", "description": "Carry" }
        }
      },

      "SmpState": {
        "type": "object",
        "properties": {
          "registers": { "$ref": "#/components/schemas/SmpRegisters" },
          "flags":     { "$ref": "#/components/schemas/SmpFlags" }
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
          "cpu":           { "$ref": "#/components/schemas/CpuState" },
          "smp":           { "$ref": "#/components/schemas/SmpState",
                             "description": "SPC700 state at the break. opcodeAddr/disasm above always describe the CPU; for the SMP instruction at smp.registers.pc use GET /smp/disassemble." }
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

    "/state/load": {
      "post": {
        "summary": "Load a quick-save state by slot",
        "operationId": "loadState",
        "description": "Loads a bsnes-plus quick-save state. Slot N loads the '<rom>-N.bst' file from the configured state directory (slot 1 = <rom>-1.bst). Requires a loaded cartridge with power on; restores full CPU/PPU/APU/WRAM/VRAM/CGRAM/OAM state.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["slot"],
              "properties": {
                "slot": { "type": "integer", "minimum": 1,
                          "description": "Quick-save slot number matching the -N.bst filename suffix.",
                          "example": 1 }
              }}
          }}
        },
        "responses": {
          "200": { "description": "State loaded",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "loaded": { "type": "boolean" },
                         "slot":   { "type": "integer" }
                       }}
                   }}},
          "400": { "description": "Missing or invalid slot." },
          "409": { "description": "No cartridge loaded, or power is off." },
          "422": { "description": "State file does not exist or could not be loaded." }
        }
      }
    },

    "/state/load-file": {
      "post": {
        "summary": "Load a save state from an arbitrary path",
        "operationId": "loadStateFile",
        "description": "Loads a bsnes-plus save state (.bst) from an arbitrary filesystem path, not restricted to the numbered quick-save slots. Requires a loaded cartridge with power on; restores full CPU/PPU/APU/WRAM/VRAM/CGRAM/OAM state.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["path"],
              "properties": {
                "path": { "type": "string",
                          "description": "Absolute filesystem path to a .bst save-state file.",
                          "example": "/home/user/som-ending.bst" }
              }}
          }}
        },
        "responses": {
          "200": { "description": "State loaded",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "loaded": { "type": "boolean" },
                         "path":   { "type": "string" }
                       }}
                   }}},
          "400": { "description": "Missing or invalid path." },
          "409": { "description": "No cartridge loaded, or power is off." },
          "422": { "description": "State file does not exist or could not be loaded." }
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
        "summary": "Disassemble the current instruction",
        "operationId": "disassemble",
        "description": "Returns the single instruction at the current program counter, disassembled with the live M/X state at that PC. There is no look-ahead window — to see the next instruction, step and call again. Takes no parameters; passing 'addr' or 'lines' returns 400.",
        "responses": {
          "200": { "description": "The current instruction",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/DisassemblyLine" }
                   }}},
          "400": { "description": "The removed 'addr' or 'lines' parameter was supplied.",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/Error" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" }
        }
      }
    },

    "/smp/registers": {
      "get": {
        "summary": "Read SPC700 registers and flags",
        "operationId": "getSmpRegisters",
        "description": "Returns all SPC700 (S-SMP) registers (PC, A, X, Y, SP, P) and PSW flags. Requires paused state.",
        "responses": {
          "200": { "description": "OK",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/SmpState" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" },
          "503": { "$ref": "#/components/responses/NoCartridge" }
        }
      },
      "put": {
        "summary": "Write SPC700 registers and/or flags",
        "operationId": "putSmpRegisters",
        "description": "Sets one or more SPC700 registers or PSW flags. Omitted fields are unchanged. Flags must be nested inside a 'flags' object. Requires paused state.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": {
              "type": "object",
              "description": "Any combination of register (hex string) fields and an optional 'flags' object.",
              "properties": {
                "pc": { "type": "string" }, "a": { "type": "string" },
                "x":  { "type": "string" }, "y": { "type": "string" },
                "sp": { "type": "string" }, "p": { "type": "string" },
                "flags": {
                  "type": "object",
                  "description": "PSW flags to update.",
                  "properties": {
                    "n": { "type": "boolean" }, "v": { "type": "boolean" },
                    "p": { "type": "boolean" }, "b": { "type": "boolean" },
                    "h": { "type": "boolean" }, "i": { "type": "boolean" },
                    "z": { "type": "boolean" }, "c": { "type": "boolean" }
                  }
                }
              }
            }
          }}
        },
        "responses": {
          "200": { "description": "Updated state",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/SmpState" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" }
        }
      }
    },

    "/smp/disassemble": {
      "get": {
        "summary": "Disassemble the current SPC700 instruction",
        "operationId": "disassembleSmp",
        "description": "Returns the single instruction at the current SPC700 program counter. There is no look-ahead window — to see the next instruction, step the SMP and call again. Takes no parameters; passing 'addr' or 'lines' returns 400.",
        "responses": {
          "200": { "description": "The current SMP instruction",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/DisassemblyLine" }
                   }}},
          "400": { "description": "The unsupported 'addr' or 'lines' parameter was supplied.",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/Error" }
                   }}},
          "409": { "$ref": "#/components/responses/NotPaused" }
        }
      }
    },

    "/smp/step/into": {
      "post": {
        "summary": "Step the SPC700 into one instruction",
        "operationId": "smpStepInto",
        "description": "Executes exactly one SPC700 instruction and returns the resulting state (both cpu and smp blocks). The break lands on the next SMP instruction; the S-CPU runs freely in the meantime. Blocks until complete.",
        "responses": {
          "200": { "description": "Step complete",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/BreakResult" }
                   }}},
          "408": { "description": "Step timed out (30s)" }
        }
      }
    },

    "/smp/step/over": {
      "post": {
        "summary": "Step the SPC700 over one instruction",
        "operationId": "smpStepOver",
        "description": "Executes one SPC700 instruction, stepping over CALL/PCALL/TCALL rather than entering them. Blocks until complete.",
        "responses": {
          "200": { "description": "Step complete",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/BreakResult" }
                   }}},
          "408": { "description": "Step timed out (30s)" }
        }
      }
    },

    "/smp/step/out": {
      "post": {
        "summary": "Step the SPC700 out of subroutine",
        "operationId": "smpStepOut",
        "description": "Runs the SPC700 until the current subroutine returns (RET). Blocks until complete.",
        "responses": {
          "200": { "description": "Step complete",
                   "content": { "application/json": {
                     "schema": { "$ref": "#/components/schemas/BreakResult" }
                   }}},
          "408": { "description": "Step timed out (30s)" }
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

    "/wram/provenance": {
      "get": {
        "summary": "WRAM byte provenance (ROM source addresses)",
        "operationId": "getWramProvenance",
        "description": "For each byte in a WRAM address range, returns the 24-bit ROM address it was originally copied from, or null if the byte has no ROM origin (written by game code, zeroed, or never written). Provenance chains (WRAM-to-WRAM copies) are resolved to their ultimate ROM source. Use this to find the correct ROM address to annotate in Diz when stepping through code executing from WRAM. Requires the emulator to be paused.",
        "parameters": [
          { "name": "addr", "in": "query", "required": true,
            "schema": { "type": "string" }, "example": "7E8000",
            "description": "Start WRAM address in hex. Must be in WRAM ($7Exxxx, $7Fxxxx, or low mirror $00-$1FFF)." },
          { "name": "count", "in": "query", "required": false,
            "schema": { "type": "integer", "default": 256, "maximum": 4096 },
            "description": "Number of bytes to query (default 256, max 4096)." }
        ],
        "responses": {
          "200": {
            "description": "Provenance data",
            "content": { "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "addr":  { "type": "string", "example": "7E8000" },
                  "count": { "type": "integer" },
                  "provenance": {
                    "type": "array",
                    "description": "One entry per requested byte. String = 6-digit hex ROM source address. null = no ROM origin.",
                    "items": {
                      "oneOf": [
                        { "type": "string", "example": "C78000" },
                        { "type": "null" }
                      ]
                    }
                  }
                }
              }
            }}
          },
          "400": { "description": "Address is not in WRAM, or invalid parameters." },
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

    "/memory/{source}/dump": {
      "post": {
        "summary": "Dump memory to a binary file",
        "operationId": "dumpMemory",
        "description": "Writes a raw binary file containing the requested memory range on the machine running bsnes. Unlike GET /memory/{source}, there is no 4096-byte cap — use this for large regions (kilobytes or more). The file is written to the bsnes host filesystem; provide an absolute path. Requires paused state.",
        "parameters": [
          { "name": "source", "in": "path", "required": true,
            "schema": { "type": "string",
                        "enum": ["cpu","apu","apuram","dsp","vram","oam","cgram","cartrom","cartram"] } }
        ],
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["addr", "count", "path"],
              "properties": {
                "addr":  { "type": "string", "description": "Start address as a hex string.", "example": "7E8000" },
                "count": { "type": "integer", "description": "Number of bytes to dump (1–16777216).", "minimum": 1, "maximum": 16777216 },
                "path":  { "type": "string", "description": "Absolute file path on the bsnes host to write raw bytes to.", "example": "/tmp/wram_dump.bin" }
              }}
          }}
        },
        "responses": {
          "200": { "description": "File written successfully",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "source":  { "type": "string" },
                         "addr":    { "type": "string" },
                         "count":   { "type": "integer" },
                         "path":    { "type": "string" },
                         "written": { "type": "integer", "description": "Actual bytes written." }
                       }}
                   }}},
          "400": { "description": "Missing or invalid parameters, or unknown source." },
          "409": { "$ref": "#/components/responses/NotPaused" },
          "500": { "description": "Could not open the destination file for writing." }
        }
      }
    },

    "/screen/dump": {
      "post": {
        "summary": "Dump the rendered screen to a PNG file",
        "operationId": "dumpScreen",
        "description": "Writes the most recently rendered frame to a PNG file on the machine running bsnes, so a client can SEE the screen instead of inferring game state from registers and memory. The image is the native-resolution rendered screen (typically 256x224, no upscaling filter). Does not require paused state — a dump while running captures the last rendered frame. Requires a cartridge loaded and at least one frame rendered.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["path"],
              "properties": {
                "path": { "type": "string", "description": "Absolute file path on the bsnes host to write the PNG to.", "example": "/render/screen.png" }
              }}
          }}
        },
        "responses": {
          "200": { "description": "PNG written successfully",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "path":   { "type": "string" },
                         "width":  { "type": "integer", "description": "Image width in pixels." },
                         "height": { "type": "integer", "description": "Image height in pixels." }
                       }}
                   }}},
          "400": { "description": "Missing or invalid 'path', or malformed JSON body." },
          "409": { "description": "No frame has been rendered yet. Run the emulator briefly, then retry." },
          "500": { "description": "Could not write the PNG to the destination path." }
        }
      }
    },

    "/input/press": {
      "post": {
        "summary": "Hold buttons and run N frames",
        "operationId": "inputPress",
        "description": "Holds the specified buttons on controller port 1 while running the emulator for 'frames' frames, then releases them. The emulator must execute during those frames so the game polls the held input — a button held while paused does nothing. Runs one frame at a time and stops early if a breakpoint fires during a frame (e.g. code that only runs once the held input reaches the game): the response then has stopped='breakpoint' with the break details and the emulator is left paused at the breakpoint (that frame is not counted). Otherwise stopped='completed'. The input override is always released on return; to hold input across a breakpoint use POST /input/hold + POST /resume. After the call the emulator is paused. Use this to drive the game: press Start to leave the title screen, navigate menus, or enter gameplay to reach code that only runs in those states.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["buttons"],
              "properties": {
                "buttons": {
                  "type": "array",
                  "minItems": 1,
                  "items": { "type": "string",
                             "enum": ["B","Y","Select","Start","Up","Down","Left","Right","A","X","L","R"] },
                  "description": "Buttons to hold simultaneously. Cannot combine Up+Down or Left+Right.",
                  "example": ["Start"]
                },
                "frames": {
                  "type": "integer", "minimum": 1, "maximum": 600, "default": 4,
                  "description": "Frames to run while holding (default 4 ≈ 1/15 s; 60 ≈ 1 s)."
                }
              }}
          }}
        },
        "responses": {
          "200": { "description": "Frames advanced, or stopped early at a breakpoint",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": {
                         "held":            { "type": "array", "items": { "type": "string" } },
                         "framesRequested": { "type": "integer" },
                         "framesRun":       { "type": "integer", "description": "Frames actually completed; < framesRequested if a breakpoint stopped it early." },
                         "stopped":         { "type": "string", "enum": ["completed", "breakpoint"] },
                         "breakpointHit":   { "type": "integer", "description": "Index of the breakpoint that fired (only when stopped='breakpoint')." },
                         "opcodeAddr":      { "type": "string", "description": "PC at the breakpoint (only when stopped='breakpoint')." },
                         "disasm":          { "type": "string", "description": "Disassembly at the breakpoint (only when stopped='breakpoint')." },
                         "cpu":             { "type": "object", "description": "CPU registers/flags at the breakpoint (only when stopped='breakpoint')." }
                       }}
                   }}},
          "400": { "description": "Unknown button, opposing directions, or bad frame count." },
          "408": { "description": "Timed out waiting for a frame boundary." },
          "503": { "$ref": "#/components/responses/NoCartridge" }
        }
      }
    },

    "/input/hold": {
      "post": {
        "summary": "Arm input override without advancing",
        "operationId": "inputHold",
        "description": "Sets the controller port 1 override to the given buttons and leaves it active. The game will read these buttons on every subsequent input poll until POST /input/release is called. Use POST /input/press for the common press-and-run pattern.",
        "requestBody": {
          "required": true,
          "content": { "application/json": {
            "schema": { "type": "object",
              "required": ["buttons"],
              "properties": {
                "buttons": { "type": "array", "minItems": 1,
                             "items": { "type": "string",
                                        "enum": ["B","Y","Select","Start","Up","Down","Left","Right","A","X","L","R"] } }
              }}
          }}
        },
        "responses": {
          "200": { "description": "Override armed",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": { "holding": { "type": "array", "items": { "type": "string" } } } }
                   }}},
          "400": { "description": "Unknown button or opposing directions." }
        }
      }
    },

    "/input/release": {
      "post": {
        "summary": "Disarm input override",
        "operationId": "inputRelease",
        "description": "Clears the input override set by POST /input/hold. Physical input resumes immediately.",
        "responses": {
          "200": { "description": "Override cleared",
                   "content": { "application/json": {
                     "schema": { "type": "object",
                       "properties": { "released": { "type": "boolean" } } }
                   }}}
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
