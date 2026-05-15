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
    // TODO: blocks until a client connects. A better approach would pause emulation
    // (but not the UI), then unpause when a client connects.
    _clientSocket = accept(listenSocket, nullptr, nullptr);

    if (_clientSocket == INVALID_SOCK_VAL) {
        ReportError("accept() failed");
        return false;
    }

    if (enableAsyncIO)
        EnableAsyncIO();

    return true;
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
    _compressThreadShouldContinue = true;
    _sendThreadShouldContinue     = true;

    _compressThread = std::thread([this]{ CompressThreadMain(); });
    _sendThread     = std::thread([this]{ SendThreadMain(); });

    return true;
}

void ThreadedSocketServer::Shutdown() {
    _compressThreadShouldContinue = false;
    _sendThreadShouldContinue     = false;

    // Wake any threads blocked on condition variables so they can observe the
    // stop flags and exit cleanly.
    _compressQueueItemAdded.notify_all();
    _compressQueueOKToAdd.notify_all();
    _sendQueueItemAdded.notify_all();
    _sendQueueOKToAdd.notify_all();

    if (_compressThread.joinable()) _compressThread.join();
    if (_sendThread.joinable())     _sendThread.join();
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
    return PushDecompressedData(reinterpret_cast<const uint8_t*>(packetBuffer), len);
}

// Called from compress thread. Queues a compressed packet for the send thread.
bool ThreadedSocketServer::PushDecompressedData(const uint8_t *buffer, int len) {
    {
        std::unique_lock<std::mutex> lock(_sendQueueMutex);
        _sendQueueOKToAdd.wait(lock, [this]{
            return (int)_readyToSendQueue.size() < _maxQueueLengthAllowed
                || !_sendThreadShouldContinue;
        });
        if (!_sendThreadShouldContinue)
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

    } while (shouldRetry && _sendThreadShouldContinue);

    return sent;
}

bool ThreadedSocketServer::WaitForCompressQueueWrite() {
    return ProcessNextCompressedItem();
}

bool ThreadedSocketServer::WaitForSendQueueWrite() {
    return ProcessNextDecompressedItem();
}

void ThreadedSocketServer::CompressThreadMain() {
    while (_compressThreadShouldContinue) {
        if (!WaitForCompressQueueWrite())
            break;
    }
}

void ThreadedSocketServer::SendThreadMain() {
    if (!_socketServer.WaitForClientConnect(_servName.c_str()))
        return;

    while (_sendThreadShouldContinue) {
        if (!WaitForSendQueueWrite())
            break;
    }

    _socketServer.Shutdown();
}
