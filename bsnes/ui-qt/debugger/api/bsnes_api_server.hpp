#pragma once
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <memory>

// Forward-declare so the header doesn't pull in all of httplib.h.
// The full definition is only needed in bsnes_api_server.cpp.
namespace httplib { class Server; }

// json.hpp is small enough to include in the header so route helpers
// can use json return types without forward-declaring everything.
#include "json.hpp"
using json = nlohmann::json;

class BsnesApiServer {
public:
    BsnesApiServer();
    ~BsnesApiServer();

    // Called once after the Qt debugger window is constructed.
    void start(int port = 5744);
    void stop();

    // Called by Debugger::event() on the Qt main thread after a break fires.
    void notifyBreak();

private:
    // ── HTTP server ──────────────────────────────────────────────────────
    std::unique_ptr<httplib::Server> _svr;
    std::thread _serverThread;

    // ── Break synchronisation ────────────────────────────────────────────
    std::mutex              _breakMutex;
    std::condition_variable _breakCv;
    bool                    _breakOccurred = false;
    json                    _lastBreakResult;

    // ── Route registration ───────────────────────────────────────────────
    void setupRoutes();

    // ── State helpers (must be called from Qt main thread) ───────────────
    json getCpuStateJson();
    json buildBreakResult();
    json disassembleAt(uint32_t addr, int lines);
    json readMemory(const std::string& source, uint32_t addr, int count);
    void writeMemory(const std::string& source, uint32_t addr,
                     const std::vector<uint8_t>& data);
    // Returns bytes written, -1 on file-open failure, -2 on bad source.
    // Must be called from the Qt main thread.
    long dumpMemoryToFile(const std::string& source, uint32_t addr,
                          uint32_t count, const std::string& path);

    // ── Source name → MemorySource enum ─────────────────────────────────
    // Returns false if name is invalid (leaves 'out' untouched).
    bool parseSource(const std::string& name,
                     SNES::Debugger::MemorySource& out);

    // ── Step helper ──────────────────────────────────────────────────────
    // Triggers a step type on the Qt thread (non-blocking), then waits on
    // _breakCv until the emulator breaks or timeout expires.
    json doStep(SNES::Debugger::StepType type,
                bool stepOverNew = false,
                std::chrono::seconds timeout = std::chrono::seconds(30));

    // ── Breakpoint serialisation ─────────────────────────────────────────
    json breakpointToJson(int index, const SNES::Debugger::Breakpoint& bp);

    // ── Qt main-thread dispatch ──────────────────────────────────────────
    // blocking=true: waits for fn to complete before returning.
    // blocking=false: queues fn and returns immediately.
    void dispatch(std::function<void()> fn, bool blocking = true);

    // ── Guard helpers ────────────────────────────────────────────────────
    // Write a 409/503 and return false when preconditions aren't met.
    bool requirePaused(httplib::Response& res);
    bool requireLoaded(httplib::Response& res);

    // ── Response helpers ─────────────────────────────────────────────────
    void sendJson(httplib::Response& res, const json& j, int status = 200);
    void sendError(httplib::Response& res, int status,
                   const std::string& code, const std::string& message);
};

// Global singleton — initialised in Debugger::Debugger().
extern BsnesApiServer* bsnesApiServer;
