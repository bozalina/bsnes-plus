#ifndef BUFFERED_SOCKET_H
#define BUFFERED_SOCKET_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <tuple>

// ---------------------------------------------------------------------------
// Platform socket abstraction
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET socket_t;
   static const socket_t INVALID_SOCK_VAL = INVALID_SOCKET;
#  define SOCK_CLOSE(s)     closesocket(s)
#  define SOCK_SHUTDOWN(s)  ::shutdown(s, SD_SEND)
#  define SOCK_LAST_ERROR() WSAGetLastError()
#  define SOCK_WOULDBLOCK   WSAEWOULDBLOCK
#  define SOCK_NOBUFS       WSAENOBUFS
#  define SOCK_CONNRESET    WSAECONNRESET
#  define SOCK_ERROR_VAL    SOCKET_ERROR
   static inline void sock_platform_init()    { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
   static inline void sock_platform_cleanup() { WSACleanup(); }
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <unistd.h>
   typedef int socket_t;
   static const socket_t INVALID_SOCK_VAL = -1;
#  define SOCK_CLOSE(s)     ::close(s)
#  define SOCK_SHUTDOWN(s)  ::shutdown(s, SHUT_WR)
#  define SOCK_LAST_ERROR() errno
#  define SOCK_WOULDBLOCK   EWOULDBLOCK
#  define SOCK_NOBUFS       ENOBUFS
#  define SOCK_CONNRESET    ECONNRESET
#  define SOCK_ERROR_VAL    (-1)
   static inline void sock_platform_init()    {}
   static inline void sock_platform_cleanup() {}
#endif
// ---------------------------------------------------------------------------

const int one_megabyte = 0x400 * 0x400;

class ByteBuffer {
public:
    // perf: instead of sending each ~8 byte message immediately, batch them up and send a full batch at once
    const static int _maxSize = 1 * one_megabyte;

    uint8_t _buffer[_maxSize];
    int _bufferLenUsed = 0;

    bool CanAdd(int len);
    bool Append(const uint8_t* buf, int len);
    void Clear();

    ByteBuffer(const ByteBuffer& from);
    ByteBuffer();
};

struct addrinfo;

class SocketServer {
public:
    bool WaitForClientConnect(const char* servName);
    void Shutdown();

    bool Send(const uint8_t *buf, int len, bool &shouldRetryOut);
    bool ClientHadConnected() const;

protected:
    socket_t _clientSocket = INVALID_SOCK_VAL;

    bool DoOpen(const char *servname, socket_t &listenSocket, struct addrinfo *&result);
    void EnableAsyncIO() const;
    void ReportError(const char *errorMsg, bool printLastError = true);
    void Die(const char *errorMsg, bool printLastError = true);
    bool BlockAndWaitForClientConnect(socket_t listenSocket, bool enableAsyncIO = false);
};

class ThreadedSocketServer {
public:
    bool Init(const char* servName);
    void Shutdown();

    bool Push(ByteBuffer& byteBuffer);

    void CompressThreadMain();
    void SendThreadMain();

    ThreadedSocketServer();
    ~ThreadedSocketServer();

protected:
    std::string _servName;
    static const int _maxQueueLengthAllowed = (100 * one_megabyte) / ByteBuffer::_maxSize;

    std::mutex              _compressQueueMutex;
    std::condition_variable _compressQueueItemAdded;
    std::condition_variable _compressQueueOKToAdd;
    std::atomic<bool>       _compressThreadShouldContinue{false};
    std::queue<ByteBuffer>  _readyToCompressQueue;

    std::mutex                                    _sendQueueMutex;
    std::condition_variable                       _sendQueueItemAdded;
    std::condition_variable                       _sendQueueOKToAdd;
    std::atomic<bool>                             _sendThreadShouldContinue{false};
    std::queue<std::tuple<int, const uint8_t*>>   _readyToSendQueue;

    std::thread _compressThread;
    std::thread _sendThread;

    SocketServer _socketServer;

    bool PushDecompressedData(const uint8_t *buffer, int len);
    bool ProcessNextCompressedItem();
    bool ProcessNextDecompressedItem();
    bool WaitForCompressQueueWrite();
    bool WaitForSendQueueWrite();
    bool SendBuffer(const uint8_t* packetBuffer, int len);
};

// Use this from the main thread
class BufferedServer {
public:
    bool Init(const char* servName);
    void Shutdown();

    bool Push(const uint8_t* buf, int len);
    bool FlushWorkingBuffer();

    inline bool IsInitialized() { return _initialized; }

protected:
    ByteBuffer _workingBuffer;
    bool _initialized = false;

    ThreadedSocketServer _thread;
};

#endif // BUFFERED_SOCKET_H
