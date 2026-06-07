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
