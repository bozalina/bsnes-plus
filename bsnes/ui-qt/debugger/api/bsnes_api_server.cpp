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
