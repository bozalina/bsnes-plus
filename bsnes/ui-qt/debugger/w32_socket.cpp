// Cross-platform socket and multithreaded trace log streaming.
//
// 3 threads work together to handle the volume of data from binary trace output:
// 1) main thread (emulation thread): batches trace items into ByteBuffers and hands
//    them to the compression thread.
// 2) compression thread: zlib-compresses each buffer and queues it for sending.
// 3) socket send thread: sends compressed buffers over TCP to DiztinGUIsh.
//
// Achieves ~45 FPS realtime with tracing enabled. With trace masking on, performance
// is a non-issue. Without masking the full firehose goes through this pipeline.
// Emulation pauses if the network can't keep up, which limits max FPS rather than
// losing data.
//
// Threading: std::thread / std::mutex / std::condition_variable (C++11, all platforms).
// Sockets:   POSIX BSD socket API on Linux/macOS; Winsock2 on Windows (same API,
//            thin platform macros in w32_socket.h absorb the differences).

#include "w32_socket.h"
#include <chrono>
#include <zlib/zlib.h>

inline QTextStream& qStdout() {
    static QTextStream r{stdout};
    return r;
}

// ---------------------------------------------------------------------------
// SocketServer
// ---------------------------------------------------------------------------

void SocketServer::ReportError(const char *errorMsg, bool printLastError) {
    qStdout() << "Error: " << errorMsg;
    if (printLastError)
        qStdout() << " (error# " << SOCK_LAST_ERROR() << ")";
    qStdout() << endl;
}

void SocketServer::Die(const char* errorMsg, bool printLastError) {
    ReportError(errorMsg, printLastError);
    Shutdown();
}

void SocketServer::Shutdown() {
    if (!ClientHadConnected())
        return;

    _connected = false;
    SOCK_SHUTDOWN(_clientSocket);
    SOCK_CLOSE(_clientSocket);
    _clientSocket = INVALID_SOCK_VAL;
    sock_platform_cleanup();
}

bool SocketServer::Send(const uint8_t* buf, int len, bool &shouldRetryOut) {
    shouldRetryOut = false;

    if (!ClientHadConnected() || len == 0 || !buf)
        return false;

    int iResult = (int)send(_clientSocket, (const char*)buf, len, 0);
    if (iResult != SOCK_ERROR_VAL)
        return true;

    int err = SOCK_LAST_ERROR();
    if (err == SOCK_NOBUFS || err == SOCK_WOULDBLOCK) {
        shouldRetryOut = true;
    } else if (err == SOCK_CONNRESET) {
        shouldRetryOut = true;
        Die("Connection reset");
    } else {
        Die("Send failed.");
    }
    return false;
}

bool SocketServer::WaitForClientConnect(const char* servname) {
    if (ClientHadConnected())
        return true;

    socket_t listenSocket = INVALID_SOCK_VAL;
    struct addrinfo *result = nullptr;

    bool success =
        DoOpen(servname, listenSocket, result) &&
        BlockAndWaitForClientConnect(listenSocket);

    if (listenSocket != INVALID_SOCK_VAL)
        SOCK_CLOSE(listenSocket);

    if (result)
        freeaddrinfo(result);

    if (success)
        return true;

    sock_platform_cleanup();
    if (ClientHadConnected())
        SOCK_CLOSE(_clientSocket);
    return false;
}

bool SocketServer::BlockAndWaitForClientConnect(socket_t listenSocket, bool enableAsyncIO) {
    // Poll instead of blocking in accept(), so RequestAbort() can cancel a wait for a
    // client that never arrives. A thread parked in accept() can't be joined, and
    // ThreadedSocketServer::Shutdown() joins from the UI thread.
    while (!_abortRequested) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);
        struct timeval timeout = { 0, 200 * 1000 };

        int ready = select((int)listenSocket + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready == SOCK_ERROR_VAL) {
            if (SOCK_LAST_ERROR() == SOCK_INTERRUPTED)
                continue;
            ReportError("select() failed");
            return false;
        }
        if (ready == 0)
            continue;

        _clientSocket = accept(listenSocket, nullptr, nullptr);
        if (_clientSocket == INVALID_SOCK_VAL) {
            ReportError("accept() failed");
            return false;
        }

        if (enableAsyncIO)
            EnableAsyncIO();

        _connected = true;
        return true;
    }

    return false;
}

bool SocketServer::DoOpen(const char *servname, socket_t &listenSocket, struct addrinfo *&result) {
    sock_platform_init();

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(nullptr, servname, &hints, &result) != 0) {
        ReportError("getaddrinfo() failed", false);
        return false;
    }

    listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listenSocket == INVALID_SOCK_VAL) {
        ReportError("socket() failed");
        return false;
    }

#ifndef _WIN32
    // bsnes closes the connection first, so the port sits in TIME_WAIT for about a
    // minute afterwards. Without SO_REUSEADDR, re-enabling Trace in that window fails bind().
    // (Windows' SO_REUSEADDR means something else -- port sharing -- and isn't needed there.)
    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    if (bind(listenSocket, result->ai_addr, (int)result->ai_addrlen) == SOCK_ERROR_VAL) {
        ReportError("bind() failed");
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCK_ERROR_VAL) {
        ReportError("listen() failed");
        return false;
    }

    return true;
}

void SocketServer::EnableAsyncIO() const {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(_clientSocket, FIONBIO, &mode);
#else
    int flags = fcntl(_clientSocket, F_GETFL, 0);
    fcntl(_clientSocket, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool SocketServer::ClientHadConnected() const {
    return _clientSocket != INVALID_SOCK_VAL;
}

// ---------------------------------------------------------------------------
// ByteBuffer
// ---------------------------------------------------------------------------

bool ByteBuffer::CanAdd(int len) {
    return _bufferLenUsed + len <= _maxSize;
}

void ByteBuffer::Clear() {
    _bufferLenUsed = 0;
}

bool ByteBuffer::Append(const uint8_t* buf, int len) {
    if (!CanAdd(len))
        return false;
    memcpy(_buffer + _bufferLenUsed, buf, len);
    _bufferLenUsed += len;
    return true;
}

ByteBuffer::ByteBuffer() {}

ByteBuffer::ByteBuffer(const ByteBuffer &from) {
    memcpy(_buffer, from._buffer, from._bufferLenUsed);
    _bufferLenUsed = from._bufferLenUsed;
}

// ---------------------------------------------------------------------------
// BufferedServer
// ---------------------------------------------------------------------------

bool BufferedServer::Init(const char* servName) {
    if (_initialized)
        return false;

    _workingBuffer.Clear();
    _initialized = true;
    return _thread.Init(servName);
}

bool BufferedServer::FlushWorkingBuffer() {
    if (!_initialized)
        return false;

    if (_workingBuffer._bufferLenUsed == 0)
        return true;

    if (!_thread.Push(_workingBuffer))
        return false;

    _workingBuffer.Clear();
    return true;
}

void BufferedServer::Shutdown() {
    if (!_initialized)
        return;

    // With no client there's nobody to deliver the partial buffer to, and Push() could
    // block forever on a full queue that nothing drains.
    if (_thread.IsClientConnected())
        FlushWorkingBuffer();
    _thread.Shutdown();
    _workingBuffer.Clear();
    _initialized = false;
}

bool BufferedServer::Push(const uint8_t* buf, int len) {
    if (!_initialized || len == 0 || !buf)
        return false;

    if (len > _workingBuffer._maxSize)
        return false;

    while (!_workingBuffer.Append(buf, len))
        FlushWorkingBuffer();

    return true;
}

// ---------------------------------------------------------------------------
// ThreadedSocketServer
// ---------------------------------------------------------------------------

ThreadedSocketServer::ThreadedSocketServer() {}

ThreadedSocketServer::~ThreadedSocketServer() {
    Shutdown();
}

bool ThreadedSocketServer::Init(const char *servName) {
    if (_compressThread.joinable())
        return false;

    _servName = servName;
    _socketServer.ClearAbort();
    _compressThreadShouldContinue = true;
    _sendThreadShouldContinue     = true;

    _compressThread = std::thread([this]{ CompressThreadMain(); });
    _sendThread     = std::thread([this]{ SendThreadMain(); });

    return true;
}

// Drains rather than drops: everything already queued is compressed and sent before
// the socket closes, so the client reads the whole trace and then a clean EOF.
// With no client connected there's nobody to drain to, so the queues are discarded.
void ThreadedSocketServer::Shutdown() {
    // Stop waiting for a client, whether none has connected yet or the current one drops
    // mid-drain. An already-connected socket keeps sending.
    _socketServer.RequestAbort();

    if (!_socketServer.IsConnected())
        _sendThreadShouldContinue = false;

    // Stage 1: the compress thread finishes its queue (handing each packet to the send
    // queue), then exits.
    _compressThreadShouldContinue = false;
    _compressQueueItemAdded.notify_all();
    _compressQueueOKToAdd.notify_all();
    _sendQueueOKToAdd.notify_all();
    if (_compressThread.joinable()) _compressThread.join();

    // Stage 2: the send thread finishes its queue, closes the socket, then exits.
    _sendThreadShouldContinue = false;
    _sendQueueItemAdded.notify_all();
    _sendQueueOKToAdd.notify_all();
    if (_sendThread.joinable()) _sendThread.join();

    ClearQueues();
}

// Discards whatever a no-client shutdown left behind, so a later Init() starts empty.
// Both worker threads have exited by the time this runs.
void ThreadedSocketServer::ClearQueues() {
    {
        std::lock_guard<std::mutex> lock(_compressQueueMutex);
        std::queue<ByteBuffer>().swap(_readyToCompressQueue);
    }
    {
        std::lock_guard<std::mutex> lock(_sendQueueMutex);
        while (!_readyToSendQueue.empty()) {
            delete[] reinterpret_cast<const Bytef*>(std::get<1>(_readyToSendQueue.front()));
            _readyToSendQueue.pop();
        }
    }
}

// Called from main thread (producer).
bool ThreadedSocketServer::Push(ByteBuffer &buffer) {
    {
        std::unique_lock<std::mutex> lock(_compressQueueMutex);
        // Block if the queue is full; unblock when the compress thread drains an item
        // or when we're shutting down.
        _compressQueueOKToAdd.wait(lock, [this]{
            return (int)_readyToCompressQueue.size() < _maxQueueLengthAllowed
                || !_compressThreadShouldContinue;
        });
        if (!_compressThreadShouldContinue)
            return false;
        _readyToCompressQueue.push(buffer);
    }
    _compressQueueItemAdded.notify_one();
    return true;
}

// Called from compress thread. Pops one buffer, compresses it, hands to send queue.
bool ThreadedSocketServer::ProcessNextCompressedItem() {
    ByteBuffer buffer;

    {
        std::unique_lock<std::mutex> lock(_compressQueueMutex);
        bool hasItem = _compressQueueItemAdded.wait_for(lock, std::chrono::milliseconds(500),
            [this]{ return !_readyToCompressQueue.empty() || !_compressThreadShouldContinue; });

        if (!hasItem)
            return true; // timeout — loop and try again

        if (_readyToCompressQueue.empty())
            return false; // shutting down, nothing left

        buffer = std::move(_readyToCompressQueue.front());
        _readyToCompressQueue.pop();
    }
    _compressQueueOKToAdd.notify_one();

    // -------------------------------------------------------------------------
    // Compress the buffer.
    // zlib requires the output buffer to be at least 1.1x the input + 12 bytes.
    // -------------------------------------------------------------------------
    const int headerSize = 1 + sizeof(uint32_t) + sizeof(uint32_t); // 'Z' + orig_len + comp_len

    uLong zlibCompressBufferSize = (uLong)(ByteBuffer::_maxSize * 1.1f) + 12;
    uLong packetFullSize         = zlibCompressBufferSize + headerSize;

    Bytef* packetBuffer      = new Bytef[packetFullSize];
    Bytef* zlibCompressBuffer = packetBuffer + headerSize;

    uLong zlibCompressedDataSizeOut = zlibCompressBufferSize;
    int z_result = compress(zlibCompressBuffer, &zlibCompressedDataSizeOut,
                            reinterpret_cast<const Bytef*>(buffer._buffer),
                            buffer._bufferLenUsed);

    if (z_result == Z_MEM_ERROR || z_result == Z_BUF_ERROR) {
        delete[] packetBuffer;
        return false;
    }

    // Packet header: 1-byte marker + 4-byte original length + 4-byte compressed length
    packetBuffer[0] = 'Z';
    packetBuffer[1] = (buffer._bufferLenUsed >> 0)  & 0xFF;
    packetBuffer[2] = (buffer._bufferLenUsed >> 8)  & 0xFF;
    packetBuffer[3] = (buffer._bufferLenUsed >> 16) & 0xFF;
    packetBuffer[4] = (buffer._bufferLenUsed >> 24) & 0xFF;
    packetBuffer[5] = (zlibCompressedDataSizeOut >> 0)  & 0xFF;
    packetBuffer[6] = (zlibCompressedDataSizeOut >> 8)  & 0xFF;
    packetBuffer[7] = (zlibCompressedDataSizeOut >> 16) & 0xFF;
    packetBuffer[8] = (zlibCompressedDataSizeOut >> 24) & 0xFF;

    int len = headerSize + (int)zlibCompressedDataSizeOut;
    if (!PushDecompressedData(reinterpret_cast<const uint8_t*>(packetBuffer), len)) {
        delete[] packetBuffer;
        return false;
    }
    return true;
}

// Called from compress thread. Queues a compressed packet for the send thread.
bool ThreadedSocketServer::PushDecompressedData(const uint8_t *buffer, int len) {
    {
        std::unique_lock<std::mutex> lock(_sendQueueMutex);
        // Also give up once shutdown is draining and the client has gone: nothing will
        // empty the send queue. A dropped client doesn't signal this condition variable,
        // so re-check on a timeout as well as on notify.
        auto canProceed = [this]{
            return (int)_readyToSendQueue.size() < _maxQueueLengthAllowed
                || !_sendThreadShouldContinue
                || (!_compressThreadShouldContinue && !_socketServer.IsConnected());
        };
        while (!_sendQueueOKToAdd.wait_for(lock, std::chrono::milliseconds(100), canProceed)) {}
        if (!_sendThreadShouldContinue
            || (int)_readyToSendQueue.size() >= _maxQueueLengthAllowed)
            return false;
        _readyToSendQueue.push(std::make_tuple(len, buffer));
    }
    _sendQueueItemAdded.notify_one();
    return true;
}

// Called from send thread. Pops one compressed packet and sends it over the socket.
bool ThreadedSocketServer::ProcessNextDecompressedItem() {
    int len = 0;
    const uint8_t* buffer = nullptr;

    {
        std::unique_lock<std::mutex> lock(_sendQueueMutex);
        bool hasItem = _sendQueueItemAdded.wait_for(lock, std::chrono::milliseconds(500),
            [this]{ return !_readyToSendQueue.empty() || !_sendThreadShouldContinue; });

        if (!hasItem)
            return true; // timeout

        if (_readyToSendQueue.empty())
            return false; // shutting down

        auto& item = _readyToSendQueue.front();
        len    = std::get<0>(item);
        buffer = std::get<1>(item);
        _readyToSendQueue.pop();
    }
    _sendQueueOKToAdd.notify_one();

    bool result = SendBuffer(buffer, len);
    delete[] reinterpret_cast<const Bytef*>(buffer);
    return result;
}

// Called from send thread.
bool ThreadedSocketServer::SendBuffer(const uint8_t* buffer, int len) {
    bool shouldRetry, sent = false;

    do {
        if (!_socketServer.WaitForClientConnect(_servName.c_str()))
            return true;

        if (_socketServer.Send(buffer, len, shouldRetry))
            sent = true;

        if (shouldRetry)
            std::this_thread::sleep_for(std::chrono::microseconds(500));

    // While draining at shutdown the stop flag is already down; keep retrying as long
    // as the client is still there to receive.
    } while (shouldRetry && (_sendThreadShouldContinue || _socketServer.IsConnected()));

    return sent;
}

bool ThreadedSocketServer::WaitForCompressQueueWrite() {
    return ProcessNextCompressedItem();
}

bool ThreadedSocketServer::WaitForSendQueueWrite() {
    return ProcessNextDecompressedItem();
}

// Both loops run until their stop flag is down AND their queue is empty (the
// ProcessNext* calls return false only then, or on error), so Shutdown() drains them.
void ThreadedSocketServer::CompressThreadMain() {
    while (WaitForCompressQueueWrite()) {}
}

void ThreadedSocketServer::SendThreadMain() {
    if (!_socketServer.WaitForClientConnect(_servName.c_str()))
        return;

    while (WaitForSendQueueWrite()) {}

    _socketServer.Shutdown();
}
