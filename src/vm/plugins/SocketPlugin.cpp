/*
 * SocketPlugin.cpp - TCP/UDP socket primitives for Pharo VM
 *
 * Non-blocking POSIX sockets with a background I/O monitor thread that
 * signals Pharo semaphores when sockets become readable/writable or
 * when connections complete.
 */

#include "winsock_compat.h"  // FIRST: winsock2 before any windows.h; net headers + BSD<->winsock shims
#include "SocketPlugin.h"
#include "../DebugSettings.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// =====================================================================
// Private socket structure (pointed to by SQSocket.privateSocketPtr)
// =====================================================================

struct PrivateSocket {
    SOCKET fd;           // socket descriptor (int on POSIX, SOCKET on winsock)
    int connSema;        // connection semaphore index
    int readSema;        // read semaphore index
    int writeSema;       // write semaphore index
    int sockState;       // one of SOCK_* constants
    int sockError;       // SOCK_LAST_ERROR after socket error
    bool writeSignaled;  // true after write-ready signaled; reset on SOCK_EAGAIN
    bool eofDetected;    // true after I/O thread sees MSG_PEEK return 0 (FIN)
    bool listening;      // true once listen() called: WAITING_FOR_CONNECTION here
                         // means a pending *incoming* connection (readable), not a
                         // connecting client socket (writable).
};

// =====================================================================
// Global state
// =====================================================================

static VirtualMachine* vm = nullptr;
static int gSessionID = 1;

// Gated tracing for the server accept/listen/data path: set SOCK_DEBUG=1
// (via the sanctioned DebugSettings env reader, since getenv is poisoned here).
#define SOCKDBG(...) do { if (pharo::g_debug.socketDebug) { \
    fprintf(stderr, "[SOCKDBG] " __VA_ARGS__); fflush(stderr); } } while (0)

// Active socket tracking for I/O monitor thread
static std::mutex gSocketMutex;
static std::vector<PrivateSocket*> gActiveSockets;
static std::vector<PrivateSocket*> gDeleteQueue; // deferred deletion

// I/O monitor thread
static std::thread gIOThread;
static std::atomic<bool> gIORunning{false};

// Self-wake channel for the select() loop.  On POSIX a classic self-pipe; on
// Windows select() can watch only sockets (not pipes), so we use a connected
// loopback UDP socket pair instead.
#ifdef _WIN32
static SOCKET gWakeSend = INVALID_SOCKET;   // sendto() side
static SOCKET gWakeRecv = INVALID_SOCKET;   // FD_SET / recvfrom() side
#else
static int gWakePipe[2] = {-1, -1};
#endif

// =====================================================================
// Helper: set socket to non-blocking
// =====================================================================

static bool setNonBlocking(SOCKET fd) {
    return sockSetNonBlocking(fd) == 0;
}

// =====================================================================
// Helpers: the I/O-thread self-wake channel
// =====================================================================

// The readable end of the wake channel, to add to the select() read set.
static SOCKET gWakeReadFd() {
#ifdef _WIN32
    return gWakeRecv;
#else
    return gWakePipe[0];
#endif
}

// Drain any pending wake bytes (the channel is non-blocking).
static void drainWake() {
    char buf[64];
#ifdef _WIN32
    while (recvfrom(gWakeRecv, buf, sizeof(buf), 0, nullptr, nullptr) > 0) {}
#else
    while (read(gWakePipe[0], buf, sizeof(buf)) > 0) {}
#endif
}

// Create the wake channel with both ends non-blocking.  Returns false on failure.
static bool initWakePair() {
#ifdef _WIN32
    gWakeRecv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    gWakeSend = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gWakeRecv == INVALID_SOCKET || gWakeSend == INVALID_SOCKET) return false;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(gWakeRecv, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
    int alen = (int)sizeof(addr);
    if (getsockname(gWakeRecv, (sockaddr*)&addr, &alen) != 0) return false;
    if (connect(gWakeSend, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
    sockSetNonBlocking(gWakeRecv);
    sockSetNonBlocking(gWakeSend);
    return true;
#else
    if (pipe(gWakePipe) < 0) return false;
    setNonBlocking(gWakePipe[0]);
    setNonBlocking(gWakePipe[1]);
    return true;
#endif
}

static void closeWakePair() {
#ifdef _WIN32
    if (gWakeRecv != INVALID_SOCKET) { sockClose(gWakeRecv); gWakeRecv = INVALID_SOCKET; }
    if (gWakeSend != INVALID_SOCKET) { sockClose(gWakeSend); gWakeSend = INVALID_SOCKET; }
#else
    if (gWakePipe[0] >= 0) { sockClose(gWakePipe[0]); gWakePipe[0] = -1; }
    if (gWakePipe[1] >= 0) { sockClose(gWakePipe[1]); gWakePipe[1] = -1; }
#endif
}

// =====================================================================
// Helper: wake the I/O monitor thread
// =====================================================================

static void wakeIOThread() {
    char c = 'w';
#ifdef _WIN32
    if (gWakeSend != INVALID_SOCKET) (void)send(gWakeSend, &c, 1, 0);
#else
    if (gWakePipe[1] >= 0) (void)write(gWakePipe[1], &c, 1);
#endif
}

// =====================================================================
// Helper: register/unregister socket for I/O monitoring
// =====================================================================

static void registerSocket(PrivateSocket* ps) {
    std::lock_guard<std::mutex> lock(gSocketMutex);
    gActiveSockets.push_back(ps);
    wakeIOThread();
}

[[maybe_unused]] static void unregisterSocket(PrivateSocket* ps) {
    std::lock_guard<std::mutex> lock(gSocketMutex);
    auto it = std::find(gActiveSockets.begin(), gActiveSockets.end(), ps);
    if (it != gActiveSockets.end()) {
        gActiveSockets.erase(it);
    }
    wakeIOThread();
}

// =====================================================================
// I/O monitor thread: select() loop with semaphore signaling
// =====================================================================

static void ioMonitorLoop() {
    while (gIORunning.load()) {
        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        SOCKET maxfd = gWakeReadFd();
        FD_SET(gWakeReadFd(), &readfds);

        // Snapshot active sockets and drain deletion queue under lock
        std::vector<PrivateSocket*> snapshot;
        {
            std::lock_guard<std::mutex> lock(gSocketMutex);
            // Free sockets that were destroyed since last iteration
            for (auto* ps : gDeleteQueue) delete ps;
            gDeleteQueue.clear();
            snapshot = gActiveSockets;
        }

        for (auto* ps : snapshot) {
            if (ps->fd == INVALID_SOCKET) continue;
            if (ps->listening) {
                // Listening server socket: watch for readable = a pending incoming
                // connection, but ONLY while it's still WAITING.  Once we promote it
                // to CONNECTED (below) it stays readable until the image accepts, so
                // continuing to select() it would spin — stop watching until accept
                // resets it to WAITING.
                if (ps->sockState != SOCK_CONNECTED) {
                    FD_SET(ps->fd, &readfds);
                    if (ps->fd > maxfd) maxfd = ps->fd;
                }
            } else if (ps->sockState == SOCK_WAITING_FOR_CONNECTION) {
                // Connecting client socket: connect completes when WRITABLE.
                FD_SET(ps->fd, &writefds);
                if (ps->fd > maxfd) maxfd = ps->fd;
            } else if (ps->sockState == SOCK_CONNECTED) {
                // Always monitor for readable data — even after EOF, Pharo's
                // SSL layer may have buffered data that needs draining
                FD_SET(ps->fd, &readfds);
                if (ps->fd > maxfd) maxfd = ps->fd;
                // Only monitor writability if EOF hasn't been seen
                if (!ps->eofDetected) {
                    FD_SET(ps->fd, &writefds);
                }
            } else if (ps->sockState == SOCK_THIS_END_CLOSED) {
                // We've closed our end; watch for readable so we can detect the
                // peer's FIN and finish the disconnection.  Without this, the
                // image's waitForDisconnectionFor: (which loops while status is
                // ThisEndClosed, waiting on readSemaphore) blocks for its full
                // multi-second timeout whenever both ends are in-image — both
                // sides closing at once, neither seeing the other's FIN.
                FD_SET(ps->fd, &readfds);
                if (ps->fd > maxfd) maxfd = ps->fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int ready = select((int)(maxfd + 1), &readfds, &writefds, nullptr, &tv);
        if (ready <= 0) continue;

        // Drain wake channel
        if (FD_ISSET(gWakeReadFd(), &readfds)) {
            drainWake();
        }

        // Process socket events
        for (auto* ps : snapshot) {
            if (ps->fd == INVALID_SOCKET) continue;

            if (ps->listening) {
                // A pending incoming connection: report the listening socket as
                // CONNECTED.  The image's waitForConnectionFor: loops while the
                // status is WaitingForConnection and returns (→ accept) on
                // Connected; the accept primitive resets it to WaitingForConnection.
                if (FD_ISSET(ps->fd, &readfds)) {
                    ps->sockState = SOCK_CONNECTED;
                    if (ps->connSema > 0 && vm) {
                        SOCKDBG("io: listen fd=%d pending -> CONNECTED, signal connSema=%d\n", ps->fd, ps->connSema);
                        vm->signalSemaphoreWithIndex(ps->connSema);
                    }
                }
            } else if (ps->sockState == SOCK_WAITING_FOR_CONNECTION && FD_ISSET(ps->fd, &writefds)) {
                // Connect completed — check for errors
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(ps->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) {
                    ps->sockState = SOCK_CONNECTED;
                } else {
                    ps->sockError = err;
                    ps->sockState = SOCK_UNCONNECTED;
#ifdef DEBUG
                    fprintf(stderr, "[SOCK] Connection failed: err=%d (%s) fd=%d\n",
                            err, strerror(err), ps->fd);
#endif
                }
                SOCKDBG("io: client connect fd=%d complete err=%d -> state=%d\n", ps->fd, err, ps->sockState);
                if (ps->connSema > 0 && vm) {
                    vm->signalSemaphoreWithIndex(ps->connSema);
                }
                if (ps->writeSema > 0 && vm) {
                    vm->signalSemaphoreWithIndex(ps->writeSema);
                }
            }

            if (ps->sockState == SOCK_CONNECTED && !ps->listening) {
                if (FD_ISSET(ps->fd, &readfds)) {
                    if (!ps->eofDetected) {
                        // First time seeing readability — peek to detect EOF
                        char peek;
                        int n = (int)recv(ps->fd, &peek, 1, MSG_PEEK);
                        if (n == 0) {
                            // EOF (FIN received) — DON'T change sockState here.
                            // The recv() primitive will set SOCK_OTHER_END_CLOSED
                            // when Pharo actually reads 0 bytes. This prevents a
                            // race where SSL-buffered data is lost because Pharo
                            // sees isConnected=false before draining the SSL layer.
                            ps->eofDetected = true;
                            if (ps->connSema > 0 && vm) {
                                vm->signalSemaphoreWithIndex(ps->connSema);
                            }
                        } else if (n < 0 && SOCK_LAST_ERROR != SOCK_EAGAIN && SOCK_LAST_ERROR != SOCK_EWOULDBLOCK) {
                            // Actual socket error — set state immediately
                            ps->sockError = SOCK_LAST_ERROR;
                            ps->sockState = SOCK_OTHER_END_CLOSED;
                            if (ps->connSema > 0 && vm) {
                                vm->signalSemaphoreWithIndex(ps->connSema);
                            }
                        }
                    }
                    // Always signal readSema — after EOF, Pharo's SSL layer
                    // still has buffered data that needs draining via recv()
                    if (ps->readSema > 0 && vm) {
                        vm->signalSemaphoreWithIndex(ps->readSema);
                    }
                }
                if (!ps->eofDetected && FD_ISSET(ps->fd, &writefds) && !ps->writeSignaled) {
                    if (ps->writeSema > 0 && vm) {
                        vm->signalSemaphoreWithIndex(ps->writeSema);
                    }
                    ps->writeSignaled = true;
                }
            }

            if (ps->sockState == SOCK_THIS_END_CLOSED && FD_ISSET(ps->fd, &readfds)) {
                // We closed our end and the socket is now readable (peer FIN /
                // EOF after our shutdown): the disconnection is complete.  Move to
                // UNCONNECTED and wake the closer — waitForDisconnectionFor: loops
                // while status is ThisEndClosed and waits on readSemaphore.
                ps->sockState = SOCK_UNCONNECTED;
                if (ps->readSema > 0 && vm) vm->signalSemaphoreWithIndex(ps->readSema);
                if (ps->connSema > 0 && vm) vm->signalSemaphoreWithIndex(ps->connSema);
                SOCKDBG("io: this-end-closed fd=%d peer FIN -> UNCONNECTED\n", ps->fd);
            }
        }
    }
}

// =====================================================================
// Init / Shutdown
// =====================================================================

void socketPluginInit() {
    if (gIORunning.load()) return;

    // Create the self-wake channel for the select() loop.
    if (!initWakePair()) {
        fprintf(stderr, "[SOCK] socketPluginInit: wake-channel init failed err=%d\n",
                SOCK_LAST_ERROR);
        return;
    }

    gIORunning.store(true);
    gIOThread = std::thread(ioMonitorLoop);
    // Detach so std::thread destructor won't call std::terminate() if
    // the process exits (via exit()) before socketPluginShutdown runs.
    gIOThread.detach();

#ifdef DEBUG
    fprintf(stderr, "[SOCK] socketPluginInit OK (I/O thread started)\n");
#endif
}

void socketPluginShutdown() {
    if (!gIORunning.load()) return;

    gIORunning.store(false);
    wakeIOThread();
    // Thread is detached, so just give it a moment to notice the flag
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    closeWakePair();

    // Clean up any remaining sockets
    {
        std::lock_guard<std::mutex> lock(gSocketMutex);
        for (auto* ps : gActiveSockets) {
            if (ps->fd != INVALID_SOCKET) sockClose(ps->fd);
            delete ps;
        }
        gActiveSockets.clear();
        for (auto* ps : gDeleteQueue) delete ps;
        gDeleteQueue.clear();
    }

    // Bump session ID so stale socket handles from previous image are rejected
    gSessionID++;

    // Clear the VM pointer — will be set again by setInterpreter on next launch
    vm = nullptr;
}

// =====================================================================
// Helper: extract SQSocket pointer from a Pharo ByteArray oop
// Returns nullptr and calls primitiveFail on error.
// =====================================================================

static SQSocket* socketFromOop(sqInt socketOop) {
    if (!vm->isBytes(socketOop) || vm->byteSizeOf(socketOop) != sizeof(SQSocket)) {
        vm->primitiveFailFor(PrimErrBadArgument);
        return nullptr;
    }
    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    if (s->sessionID != gSessionID) {
        vm->primitiveFailFor(PrimErrBadArgument);
        return nullptr;
    }
    return s;
}

static PrivateSocket* privateSocketFrom(sqInt socketOop) {
    SQSocket* s = socketFromOop(socketOop);
    if (!s) return nullptr;
    return (PrivateSocket*)s->privateSocketPtr;
}

// =====================================================================
// PRIMITIVES
// =====================================================================

// primitiveSocketCreate3Semaphores
// Stack: receiver, netType, socketType, recvBufSize, sendBufSize,
//        semaIndex, readSemaIndex, writeSemaIndex
// Returns: socketHandle (ByteArray of sizeof(SQSocket))
extern "C" sqInt sp_primitiveSocketCreate3Semaphores(void) {
    sqInt writeSemaIndex = vm->stackIntegerValue(0);
    sqInt readSemaIndex  = vm->stackIntegerValue(1);
    sqInt semaIndex      = vm->stackIntegerValue(2);
    // sendBufSize (3) and recvBufSize (4) are ignored — OS manages buffers
    sqInt socketType     = vm->stackIntegerValue(5);
    // netType (6) ignored — always IPv4
    if (vm->failed()) return vm->primitiveFail();

    // Create the OS socket
    int domain = AF_INET;
    int type = (socketType == UDP_SOCKET_TYPE) ? SOCK_DGRAM : SOCK_STREAM;
    SOCKET fd = socket(domain, type, 0);
    if (fd == INVALID_SOCKET) return vm->primitiveFail();

    if (!setNonBlocking(fd)) {
        sockClose(fd);
        return vm->primitiveFail();
    }

    // Create private socket struct
    PrivateSocket* ps = new PrivateSocket();
    ps->fd = fd;
    ps->connSema = (int)semaIndex;
    ps->readSema = (int)readSemaIndex;
    ps->writeSema = (int)writeSemaIndex;
    // UDP sockets are connectionless — mark as Connected so isConnected returns true
    // (needed for waitForDataFor: which checks isConnected in its polling loop)
    ps->sockState = (socketType == UDP_SOCKET_TYPE) ? SOCK_CONNECTED : SOCK_UNCONNECTED;
    ps->sockError = 0;
    ps->writeSignaled = false;
    ps->eofDetected = false;
    ps->listening = false;

    // Allocate ByteArray for SQSocket handle
    sqInt classBA = vm->classByteArray();
    sqInt socketOop = vm->instantiateClassindexableSize(classBA, sizeof(SQSocket));
    if (vm->failed()) {
        sockClose(fd);
        delete ps;
        return vm->primitiveFail();
    }

    // Pin the ByteArray so its address doesn't change during GC
    vm->pushRemappableOop(socketOop);

    // Fill in the SQSocket struct
    socketOop = vm->popRemappableOop();
    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    if (!s) {
        sockClose(fd);
        delete ps;
        return vm->primitiveFail();
    }
    s->sessionID = gSessionID;
    s->socketType = (int)socketType;
    s->privateSocketPtr = ps;

    // Register for I/O monitoring
    registerSocket(ps);

    // Replace all 8 stack items (7 args + receiver) with the result
    vm->popthenPush(8, socketOop);
    return 0;
}

// primitiveSocketDestroy
// Stack: receiver, socketHandle
extern "C" sqInt sp_primitiveSocketDestroy(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    // Close fd first so I/O thread skips this socket (fd < 0 check)
    if (ps->fd != INVALID_SOCKET) {
        sockClose(ps->fd);
        ps->fd = INVALID_SOCKET;
    }

    // Clear the handle so it can't be reused
    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    s->sessionID = 0;
    s->privateSocketPtr = nullptr;

    // Remove from active list and defer deletion to I/O thread
    // (avoids use-after-free if I/O thread has a snapshot including this socket)
    {
        std::lock_guard<std::mutex> lock(gSocketMutex);
        auto it = std::find(gActiveSockets.begin(), gActiveSockets.end(), ps);
        if (it != gActiveSockets.end()) {
            gActiveSockets.erase(it);
        }
        gDeleteQueue.push_back(ps);
    }
    wakeIOThread();

    vm->pop(1); // pop socketHandle, leave receiver
    return 0;
}

// primitiveSocketConnectToPort
// Stack: receiver, socketHandle, address (integer or ByteArray), port
extern "C" sqInt sp_primitiveSocketConnectToPort(void) {
    sqInt port    = vm->stackIntegerValue(0);
    sqInt addrOop = vm->stackValue(1);
    sqInt socketOop = vm->stackValue(2);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();
    if (ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    struct sockaddr_storage ss;
    socklen_t ssLen;
    memset(&ss, 0, sizeof(ss));

    // Build IP address string for getaddrinfo (handles NAT64 synthesis)
    char addrStr[INET6_ADDRSTRLEN];
    addrStr[0] = '\0';

    if (vm->isIntegerObject(addrOop)) {
        // SmallInteger: IPv4 address in host byte order
        uint32_t hostAddr = (uint32_t)vm->integerValueOf(addrOop);
        uint32_t netAddr = htonl(hostAddr);
        inet_ntop(AF_INET, &netAddr, addrStr, sizeof(addrStr));
    } else if (vm->isBytes(addrOop)) {
        sqInt addrSize = vm->byteSizeOf(addrOop);
        uint8_t* bytes = (uint8_t*)vm->firstIndexableField(addrOop);
        if (addrSize == 4) {
            inet_ntop(AF_INET, bytes, addrStr, sizeof(addrStr));
        } else if (addrSize == 16) {
            inet_ntop(AF_INET6, bytes, addrStr, sizeof(addrStr));
        } else {
            return vm->primitiveFail();
        }
    } else {
        return vm->primitiveFail();
    }

    // Use getaddrinfo to resolve the numeric address — on IPv6-only networks
    // with NAT64, this synthesizes the proper IPv6 address from an IPv4 literal.
    // This is Apple's recommended approach for NAT64 compatibility.
    // NOTE: Do NOT use AI_NUMERICHOST — Apple docs explicitly say it prevents
    // IPv6 address synthesis on NAT64 networks.
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%ld", (long)port);
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;  // port is numeric, but let address resolve for NAT64
    struct addrinfo* aiResult = nullptr;

    int aiErr = getaddrinfo(addrStr, portStr, &hints, &aiResult);
    if (aiErr != 0 || !aiResult) {
#ifdef DEBUG
        fprintf(stderr, "[SOCK] getaddrinfo('%s', '%s') failed: %s\n",
                addrStr, portStr, gai_strerror(aiErr));
#endif
        return vm->primitiveFail();
    }

    // Use the first result — on NAT64 networks this is the synthesized IPv6
    struct addrinfo* chosen = aiResult;
    memcpy(&ss, chosen->ai_addr, chosen->ai_addrlen);
    ssLen = (socklen_t)chosen->ai_addrlen;

    // If address family changed (IPv4→IPv6 via NAT64), re-create the socket
    if (chosen->ai_family == AF_INET6) {
        SOCKET oldFd = ps->fd;
        SOCKET newFd = socket(AF_INET6, SOCK_STREAM, 0);
        if (newFd == INVALID_SOCKET) {
            ps->sockError = SOCK_LAST_ERROR;
            freeaddrinfo(aiResult);
            return vm->primitiveFail();
        }
        if (!setNonBlocking(newFd)) {
            sockClose(newFd);
            freeaddrinfo(aiResult);
            return vm->primitiveFail();
        }
        sockClose(oldFd);
        ps->fd = newFd;
    }

    freeaddrinfo(aiResult);

    int result = connect(ps->fd, (struct sockaddr*)&ss, ssLen);
    if (result == 0) {
        // Immediate connection (unlikely for TCP but possible on localhost)
        ps->sockState = SOCK_CONNECTED;
        if (ps->connSema > 0) vm->signalSemaphoreWithIndex(ps->connSema);
    } else if (SOCK_LAST_ERROR == SOCK_EINPROGRESS) {
        ps->sockState = SOCK_WAITING_FOR_CONNECTION;
        wakeIOThread(); // ensure monitor notices
    } else {
        ps->sockError = SOCK_LAST_ERROR;
        return vm->primitiveFail();
    }

    vm->pop(3); // pop port, address, socketHandle; leave receiver
    return 0;
}

// primitiveSocketConnectionStatus
// Stack: receiver, socketHandle
// Returns: SmallInteger (status code)
extern "C" sqInt sp_primitiveSocketConnectionStatus(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) {
        // Invalid/destroyed socket — return Invalid status instead of failing
        vm->popthenPush(2, vm->integerObjectOf(SOCK_INVALID));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ps->sockState));
    return 0;
}

// primitiveSocketCloseConnection
// Stack: receiver, socketHandle
extern "C" sqInt sp_primitiveSocketCloseConnection(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    if (ps->fd != INVALID_SOCKET) {
        shutdown(ps->fd, SHUT_RDWR);
        ps->sockState = SOCK_THIS_END_CLOSED;
    }

    vm->pop(1); // leave receiver
    return 0;
}

// primitiveSocketAbortConnection
// Stack: receiver, socketHandle
extern "C" sqInt sp_primitiveSocketAbortConnection(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    if (ps->fd != INVALID_SOCKET) {
        // Set SO_LINGER to 0 for immediate RST
        struct linger lin = {1, 0};
        setsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
        sockClose(ps->fd);
        ps->fd = INVALID_SOCKET;
        ps->sockState = SOCK_THIS_END_CLOSED;
    }

    vm->pop(1); // leave receiver
    return 0;
}

// primitiveSocketSendDataBufCount
// Stack: receiver, socketHandle, data, startIndex (1-based), count
// Returns: SmallInteger (bytes actually sent)
extern "C" sqInt sp_primitiveSocketSendDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt dataOop    = vm->stackValue(2);
    sqInt socketOop  = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    // startIndex is 1-based
    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    ssize_t sent = send(ps->fd, buf + offset, (size_t)count, 0);
    if (sent < 0) {
        if (SOCK_LAST_ERROR == SOCK_EAGAIN || SOCK_LAST_ERROR == SOCK_EWOULDBLOCK) {
            sent = 0; // Nothing sent yet, try again later
            ps->writeSignaled = false; // Re-arm write notification
        } else {
            ps->sockError = SOCK_LAST_ERROR;
            SOCKDBG("send: fd=%d count=%ld -> ERR SOCK_LAST_ERROR=%d\n", ps->fd, (long)count, SOCK_LAST_ERROR);
            return vm->primitiveFail();
        }
    }

    SOCKDBG("send: fd=%d count=%ld -> sent=%zd\n", ps->fd, (long)count, sent);
    vm->popthenPush(5, vm->integerObjectOf((sqInt)sent));
    return 0;
}

// primitiveSocketSendDone
// Stack: receiver, socketHandle
// Returns: Boolean
extern "C" sqInt sp_primitiveSocketSendDone(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    // Non-blocking sockets: send is always "done" since we don't buffer
    // The image retries if not all bytes were sent.
    vm->popthenPush(2, vm->trueObject());
    return 0;
}

// primitiveSocketReceiveDataAvailable
// Stack: receiver, socketHandle
// Returns: Boolean
extern "C" sqInt sp_primitiveSocketReceiveDataAvailable(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) {
        vm->popthenPush(2, vm->falseObject());
        return 0;
    }

    // Use select() with zero timeout to check readability
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ps->fd, &readfds);
    struct timeval tv = {0, 0};
    int ready = select((int)(ps->fd + 1), &readfds, nullptr, nullptr, &tv);

    vm->popthenPush(2, (ready > 0) ? vm->trueObject() : vm->falseObject());
    return 0;
}

// primitiveSocketReceiveDataBufCount
// Stack: receiver, socketHandle, data, startIndex (1-based), count
// Returns: SmallInteger (bytes actually received)
extern "C" sqInt sp_primitiveSocketReceiveDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt dataOop    = vm->stackValue(2);
    sqInt socketOop  = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    ssize_t received = recv(ps->fd, buf + offset, (size_t)count, 0);
    if (received < 0) {
        int e = SOCK_LAST_ERROR;
        if (e == SOCK_EAGAIN || e == SOCK_EWOULDBLOCK) {
            received = 0; // No data available yet
        } else if (e == SOCK_ECONNRESET || e == SOCK_ECONNABORTED) {
            // Peer reset/aborted the connection.  On Windows closesocket() with
            // unread inbound data sends an RST (where a graceful close would FIN),
            // so a normal "other end closed" surfaces here as ECONNRESET.  Treat it
            // as a clean EOF (0 bytes + OtherEndClosed) instead of failing the
            // primitive — otherwise the image raises ConnectionClosed mid-read.
            received = 0;
            ps->sockState = SOCK_OTHER_END_CLOSED;
        } else {
            ps->sockError = e;
#ifdef DEBUG
            fprintf(stderr, "[SOCK] recv fd=%d error=%d (%s)\n",
                    ps->fd, e, strerror(e));
#endif
            return vm->primitiveFail();
        }
    } else if (received == 0) {
        // Connection closed by remote end — this is the definitive EOF.
        // The I/O thread may have already set eofDetected, but this is
        // where we actually transition the state (after all data is read).
        ps->sockState = SOCK_OTHER_END_CLOSED;
    }

    SOCKDBG("recv: fd=%d count=%ld -> received=%zd SOCK_LAST_ERROR=%d state=%d\n",
            ps->fd, (long)count, received, (received < 0 ? SOCK_LAST_ERROR : 0), ps->sockState);
    vm->popthenPush(5, vm->integerObjectOf((sqInt)received));
    return 0;
}

// primitiveSocketError
// Stack: receiver, socketHandle
// Returns: SmallInteger (error code)
extern "C" sqInt sp_primitiveSocketError(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ps->sockError));
    return 0;
}

// primitiveSocketLocalPort
// Stack: receiver, socketHandle
// Returns: SmallInteger (port number)
extern "C" sqInt sp_primitiveSocketLocalPort(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getsockname(ps->fd, (struct sockaddr*)&sa, &len) < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ntohs(sa.sin_port)));
    return 0;
}

// primitiveSocketLocalAddress
// Stack: receiver, socketHandle
// Returns: a 4-byte ByteArray IPv4 address in network byte order.  Pharo's
// SocketAddress model is byte-array based (e.g. ZnNetworkingUtils>>ipAddressToString:
// iterates the bytes with #do:separatedBy:); returning a SmallInteger here, as the
// old code did, makes the server path DNU on #do:separatedBy:.
extern "C" sqInt sp_primitiveSocketLocalAddress(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    uint32_t netAddr = 0;  // 0.0.0.0 when unbound/unknown
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (ps && ps->fd != INVALID_SOCKET && getsockname(ps->fd, (struct sockaddr*)&sa, &len) == 0) {
        netAddr = sa.sin_addr.s_addr;  // already network byte order
    }
    sqInt addrOop = vm->instantiateClassindexableSize(vm->classByteArray(), 4);
    if (vm->failed()) return vm->primitiveFail();
    memcpy(vm->firstIndexableField(addrOop), &netAddr, 4);
    vm->popthenPush(2, addrOop);
    return 0;
}

// primitiveSocketRemoteAddress
// Stack: receiver, socketHandle
// Returns: a 4-byte ByteArray IPv4 address in network byte order (see
// primitiveSocketLocalAddress for why this is a ByteArray, not a SmallInteger).
extern "C" sqInt sp_primitiveSocketRemoteAddress(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    uint32_t netAddr = 0;  // 0.0.0.0 when not connected
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (ps && ps->fd != INVALID_SOCKET && getpeername(ps->fd, (struct sockaddr*)&sa, &len) == 0) {
        netAddr = sa.sin_addr.s_addr;  // already network byte order
    }
    sqInt addrOop = vm->instantiateClassindexableSize(vm->classByteArray(), 4);
    if (vm->failed()) return vm->primitiveFail();
    memcpy(vm->firstIndexableField(addrOop), &netAddr, 4);
    vm->popthenPush(2, addrOop);
    return 0;
}

// primitiveSocketRemotePort
// Stack: receiver, socketHandle
// Returns: SmallInteger (port number)
extern "C" sqInt sp_primitiveSocketRemotePort(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getpeername(ps->fd, (struct sockaddr*)&sa, &len) < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ntohs(sa.sin_port)));
    return 0;
}

// Helper: match option name (not null-terminated) against a C string
static bool optNameIs(const char* buf, sqInt len, const char* expected) {
    sqInt elen = (sqInt)strlen(expected);
    return len == elen && memcmp(buf, expected, (size_t)elen) == 0;
}

// Helper: parse value string as integer
static int parseOptValue(const char* buf, sqInt len) {
    if (len <= 0) return 0;
    char tmp[32];
    sqInt cpLen = len < 31 ? len : 31;
    memcpy(tmp, buf, (size_t)cpLen);
    tmp[cpLen] = '\0';
    return (int)strtol(tmp, nullptr, 10);
}

// primitiveSocketGetOptions
// Stack: receiver, socketHandle, optionName (ByteString)
// Returns: Array of {errorCode. returnedValue}
extern "C" sqInt sp_primitiveSocketGetOptions(void) {
    sqInt nameOop   = vm->stackValue(0);
    sqInt socketOop = vm->stackValue(1);

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    if (!vm->isBytes(nameOop)) return vm->primitiveFail();
    char* optName = (char*)vm->firstIndexableField(nameOop);
    sqInt nameLen = vm->byteSizeOf(nameOop);

    int errCode = 0;
    int retVal = 0;

    if (optNameIs(optName, nameLen, "TCP_NODELAY")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, IPPROTO_TCP, TCP_NODELAY, &retVal, &vlen) < 0)
            errCode = SOCK_LAST_ERROR;
    } else if (optNameIs(optName, nameLen, "SO_LINGER")) {
        struct linger lin;
        socklen_t vlen = sizeof(lin);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, &vlen) < 0)
            errCode = SOCK_LAST_ERROR;
        else
            retVal = lin.l_onoff ? lin.l_linger : 0;
    } else if (optNameIs(optName, nameLen, "SO_KEEPALIVE")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_KEEPALIVE, &retVal, &vlen) < 0)
            errCode = SOCK_LAST_ERROR;
    } else if (optNameIs(optName, nameLen, "SO_SNDBUF")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_SNDBUF, &retVal, &vlen) < 0)
            errCode = SOCK_LAST_ERROR;
    } else if (optNameIs(optName, nameLen, "SO_RCVBUF")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_RCVBUF, &retVal, &vlen) < 0)
            errCode = SOCK_LAST_ERROR;
    } else if (optNameIs(optName, nameLen, "SO_BROADCAST")) {
        // Socket>>broadcastMisconfiguredForSendingTo: reads this to decide
        // whether a failed send to a broadcast address should raise
        // NoBroadcastAllowed — it must reflect the real option state.
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_BROADCAST, &retVal, &vlen) < 0)
            errCode = SOCK_LAST_ERROR;
    } else {
        // Unknown option — return 0 with no error (like the standard VM)
    }

    // Return a 2-element Array: {errCode. retVal}
    sqInt resultArray = vm->instantiateClassindexableSize(vm->classArray(), 2);
    if (vm->failed()) return vm->primitiveFail();

    vm->storePointerofObjectwithValue(0, resultArray, vm->integerObjectOf(errCode));
    vm->storePointerofObjectwithValue(1, resultArray, vm->integerObjectOf(retVal));

    vm->popthenPush(3, resultArray);
    return 0;
}

// primitiveSocketSetOptions
// Stack: receiver, socketHandle, optionName, optionValue
// Returns: Array of {errorCode. returnedValue}
extern "C" sqInt sp_primitiveSocketSetOptions(void) {
    sqInt valueOop  = vm->stackValue(0);
    sqInt nameOop   = vm->stackValue(1);
    sqInt socketOop = vm->stackValue(2);

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    if (!vm->isBytes(nameOop)) return vm->primitiveFail();
    if (!vm->isBytes(valueOop)) return vm->primitiveFail();

    char* optName = (char*)vm->firstIndexableField(nameOop);
    sqInt nameLen = vm->byteSizeOf(nameOop);

    char* optValue = (char*)vm->firstIndexableField(valueOop);
    sqInt valueLen = vm->byteSizeOf(valueOop);

    int errCode = 0;
    int retVal = 0;

    if (optNameIs(optName, nameLen, "TCP_NODELAY")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0)
            errCode = SOCK_LAST_ERROR;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_LINGER")) {
        int val = parseOptValue(optValue, valueLen);
        struct linger lin;
        lin.l_onoff = (val > 0) ? 1 : 0;
        lin.l_linger = val;
        if (setsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin)) < 0)
            errCode = SOCK_LAST_ERROR;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_KEEPALIVE")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0)
            errCode = SOCK_LAST_ERROR;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_SNDBUF")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
            errCode = SOCK_LAST_ERROR;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_RCVBUF")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) < 0)
            errCode = SOCK_LAST_ERROR;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_BROADCAST")) {
        // Required for UDP broadcast sends; without it Windows sendto() to a
        // broadcast address fails WSAEACCES (UDPSocketTest>>testUDPBroadcastError).
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, SOL_SOCKET, SO_BROADCAST, &val, sizeof(val)) < 0)
            errCode = SOCK_LAST_ERROR;
        retVal = val;
    }
    // Unknown options are silently ignored (return {0. 0})

    // Return a 2-element Array: {errCode. retVal} (same as getOptions)
    sqInt resultArray = vm->instantiateClassindexableSize(vm->classArray(), 2);
    if (vm->failed()) return vm->primitiveFail();

    vm->storePointerofObjectwithValue(0, resultArray, vm->integerObjectOf(errCode));
    vm->storePointerofObjectwithValue(1, resultArray, vm->integerObjectOf(retVal));

    vm->popthenPush(4, resultArray);
    return 0;
}

// primitiveSocketBindToPort  (Pharo's split bind/listen server API)
// Stack: receiver, socketHandle, address, port
// Binds the socket to a local address+port WITHOUT listening — the image then
// calls primitiveSocketListenWithBacklog separately.  We only ever implemented
// the combined listenOnPortBacklog before, so this primitive was missing and
// Socket>>bindTo:port: fell through to `SocketError signal:` with SOCK_LAST_ERROR 0 (the
// "SocketError: Undefined error: 0" / "Success" seen in ZnServer/Teapot tests).
extern "C" sqInt sp_primitiveSocketBindToPort(void) {
    sqInt port      = vm->stackIntegerValue(0);
    sqInt addrOop   = vm->stackValue(1);
    sqInt socketOop = vm->stackValue(2);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    // Allow address reuse (mirrors listenOnPortBacklog; avoids TIME_WAIT binds
    // failing when a test re-binds the same port).
    int reuse = 1;
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = INADDR_ANY;   // default: all interfaces (SocketAddress zero)

    // Address handling mirrors primitiveSocketConnectToPort: a 4-byte ByteArray
    // is the raw network-order IPv4 address; an empty address means "any"; a
    // SmallInteger is a host-order IPv4 address.
    if (vm->isBytes(addrOop)) {
        sqInt n = vm->byteSizeOf(addrOop);
        if (n == 4) {
            memcpy(&sa.sin_addr.s_addr, vm->firstIndexableField(addrOop), 4);
        } else if (n != 0) {
            return vm->primitiveFail();   // IPv6/other not handled on this bind path
        }
    } else if (vm->isIntegerObject(addrOop)) {
        sa.sin_addr.s_addr = htonl((uint32_t)vm->integerValueOf(addrOop));
    }

    if (bind(ps->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ps->sockError = SOCK_LAST_ERROR;
        return vm->primitiveFail();
    }

    // bind() does not change connection status — leave it UNCONNECTED so the
    // image's listenWithBacklog: precondition (status == Unconnected) holds.
    vm->pop(3); // pop port, address, socketHandle; leave receiver
    return 0;
}

// primitiveSocketListenWithBacklog  (listen on an already-bound socket)
// Stack: receiver, socketHandle, backlogSize
extern "C" sqInt sp_primitiveSocketListenWithBacklog(void) {
    sqInt backlog   = vm->stackIntegerValue(0);
    sqInt socketOop = vm->stackValue(1);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    if (listen(ps->fd, (int)backlog) < 0) {
        ps->sockError = SOCK_LAST_ERROR;
        return vm->primitiveFail();
    }

    ps->listening = true;
    ps->sockState = SOCK_WAITING_FOR_CONNECTION;
    wakeIOThread();

    vm->pop(2); // pop backlog, socketHandle; leave receiver
    return 0;
}

// Helper: is this a datagram socket? listen() on SOCK_DGRAM fails with
// EOPNOTSUPP — stock Cog's sqSocketListenOnPortBacklogSizeInterface binds
// UDP sockets WITHOUT calling listen(); mirror that in every listen prim.
static bool isDatagramSocket(PrivateSocket* ps) {
    int soType = 0;
    socklen_t tl = sizeof(soType);
    if (getsockopt(ps->fd, SOL_SOCKET, SO_TYPE, &soType, &tl) < 0) return false;
    return soType == SOCK_DGRAM;
}

// primitiveSocketListenWithOrWithoutBacklog
// The image's UDP `Socket>>setPort:` calls this with 2 args; the legacy
// TCP single-connection listen also uses the 2-arg form. 3-arg = backlog.
//   2-arg stack: receiver, socketHandle, port
//   3-arg stack: receiver, socketHandle, port, backlogSize
// Missing before 2026-07-02: UDP setPort: failed with sockError 0
// ("SocketError: The operation completed successfully") and no UDP server
// could ever bind — UDPSocketEchoTest>>testEcho.
extern "C" sqInt sp_primitiveSocketListenWithOrWithoutBacklog(void) {
    sqInt argCount = vm->methodArgumentCount();
    sqInt backlog = 1;
    sqInt port, socketOop;
    if (argCount == 3) {
        backlog   = vm->stackIntegerValue(0);
        port      = vm->stackIntegerValue(1);
        socketOop = vm->stackValue(2);
    } else if (argCount == 2) {
        port      = vm->stackIntegerValue(0);
        socketOop = vm->stackValue(1);
    } else {
        return vm->primitiveFail();
    }
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    int reuse = 1;
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(ps->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ps->sockError = SOCK_LAST_ERROR;
        return vm->primitiveFail();
    }

    if (!isDatagramSocket(ps)) {
        if (listen(ps->fd, (int)backlog) < 0) {
            ps->sockError = SOCK_LAST_ERROR;
            return vm->primitiveFail();
        }
        ps->listening = true;
        ps->sockState = SOCK_WAITING_FOR_CONNECTION;
    }
    // UDP: bound and ready; stays SOCK_CONNECTED (set at creation) so
    // isConnected/waitForData behave like stock.
    wakeIOThread();

    vm->pop(argCount);  // pop args, leave receiver
    return 0;
}

// primitiveSocketListenOnPortBacklog
// Stack: receiver, socketHandle, port, backlogSize
extern "C" sqInt sp_primitiveSocketListenOnPortBacklog(void) {
    sqInt backlog    = vm->stackIntegerValue(0);
    sqInt port       = vm->stackIntegerValue(1);
    sqInt socketOop  = vm->stackValue(2);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    // Allow address reuse
    int reuse = 1;
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(ps->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ps->sockError = SOCK_LAST_ERROR;
        return vm->primitiveFail();
    }

    if (!isDatagramSocket(ps)) {   // listen() on UDP is EOPNOTSUPP; bind is all it needs
        if (listen(ps->fd, (int)backlog) < 0) {
            ps->sockError = SOCK_LAST_ERROR;
            return vm->primitiveFail();
        }
        ps->listening = true;
        ps->sockState = SOCK_WAITING_FOR_CONNECTION;
    }
    wakeIOThread();

    vm->pop(3); // pop args, leave receiver
    return 0;
}

// primitiveSocketListenOnPortBacklogInterface
// Stack: receiver, socketHandle, port, backlogSize, interfaceAddress
extern "C" sqInt sp_primitiveSocketListenOnPortBacklogInterface(void) {
    sqInt interfaceAddr = vm->stackIntegerValue(0);
    sqInt backlog       = vm->stackIntegerValue(1);
    sqInt port          = vm->stackIntegerValue(2);
    sqInt socketOop     = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    int reuse = 1;
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl((uint32_t)interfaceAddr);

    if (bind(ps->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ps->sockError = SOCK_LAST_ERROR;
        return vm->primitiveFail();
    }

    if (!isDatagramSocket(ps)) {   // listen() on UDP is EOPNOTSUPP; bind is all it needs
        if (listen(ps->fd, (int)backlog) < 0) {
            ps->sockError = SOCK_LAST_ERROR;
            return vm->primitiveFail();
        }
        ps->listening = true;
        ps->sockState = SOCK_WAITING_FOR_CONNECTION;
    }
    wakeIOThread();

    vm->pop(4); // pop args, leave receiver
    return 0;
}

// primitiveSocketAccept3Semaphores
// Stack: receiver, serverSocketHandle, recvBufSize, sendBufSize,
//        semaIndex, readSemaIndex, writeSemaIndex
// Returns: new socketHandle (ByteArray)
extern "C" sqInt sp_primitiveSocketAccept3Semaphores(void) {
    sqInt writeSemaIndex = vm->stackIntegerValue(0);
    sqInt readSemaIndex  = vm->stackIntegerValue(1);
    sqInt semaIndex      = vm->stackIntegerValue(2);
    // sendBufSize (3), recvBufSize (4) ignored
    sqInt serverOop      = vm->stackValue(5);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* serverPs = privateSocketFrom(serverOop);
    if (!serverPs || serverPs->fd == INVALID_SOCKET) return vm->primitiveFail();

    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    SOCKET clientFd = accept(serverPs->fd, (struct sockaddr*)&clientAddr, &addrLen);
    SOCKDBG("accept: serverFd=%d -> clientFd=%d SOCK_LAST_ERROR=%d\n", (int)serverPs->fd, (int)clientFd, SOCK_LAST_ERROR);
    if (clientFd == INVALID_SOCKET) {
        if (SOCK_LAST_ERROR == SOCK_EAGAIN || SOCK_LAST_ERROR == SOCK_EWOULDBLOCK) {
            // No pending connection — create an unconnected socket handle
            // The image will retry
        }
        return vm->primitiveFail();
    }

    setNonBlocking(clientFd);

    // The listening socket returns to waiting; the IO thread re-promotes it to
    // CONNECTED when another incoming connection is pending.
    serverPs->sockState = SOCK_WAITING_FOR_CONNECTION;
    SOCKDBG("accept: reset listen fd=%d -> WAITING\n", serverPs->fd);

    PrivateSocket* ps = new PrivateSocket();
    ps->fd = clientFd;
    ps->connSema = (int)semaIndex;
    ps->readSema = (int)readSemaIndex;
    ps->writeSema = (int)writeSemaIndex;
    ps->sockState = SOCK_CONNECTED;
    ps->sockError = 0;
    ps->writeSignaled = false;

    sqInt socketOop = vm->instantiateClassindexableSize(vm->classByteArray(), sizeof(SQSocket));
    if (vm->failed()) {
        sockClose(clientFd);
        delete ps;
        return vm->primitiveFail();
    }

    vm->pushRemappableOop(socketOop);
    socketOop = vm->popRemappableOop();

    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    s->sessionID = gSessionID;
    s->socketType = TCP_SOCKET_TYPE;
    s->privateSocketPtr = ps;

    registerSocket(ps);

    vm->popthenPush(7, socketOop);
    return 0;
}

// primitiveHasSocketAccess
// Stack: receiver
// Returns: true (we always allow socket access)
extern "C" sqInt sp_primitiveHasSocketAccess(void) {
    vm->popthenPush(1, vm->trueObject());
    return 0;
}

// =====================================================================
// UDP primitives: sendto / recvfrom
// =====================================================================

// primitiveSocketSendUDPDataBufCount
// Pharo method: primSocket: socketID sendUDPData: data toHost: hostAddr port: port startIndex: start count: count
// Stack [0]=count [1]=startIndex [2]=port [3]=hostAddr [4]=data [5]=socketID [6]=receiver
// Returns: SmallInteger (bytes sent)
extern "C" sqInt sp_primitiveSocketSendUDPDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt port       = vm->stackIntegerValue(2);
    sqInt addrOop    = vm->stackValue(3);
    sqInt dataOop    = vm->stackValue(4);
    sqInt socketOop  = vm->stackValue(5);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    // Build destination address
    uint32_t hostAddr = 0;
    if (vm->isIntegerObject(addrOop)) {
        hostAddr = (uint32_t)vm->integerValueOf(addrOop);
    } else if (vm->isBytes(addrOop) && vm->byteSizeOf(addrOop) >= 4) {
        uint8_t* abytes = (uint8_t*)vm->firstIndexableField(addrOop);
        hostAddr = ((uint32_t)abytes[0] << 24) | ((uint32_t)abytes[1] << 16) |
                   ((uint32_t)abytes[2] << 8)  | (uint32_t)abytes[3];
    } else {
        return vm->primitiveFail();
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);
    dest.sin_addr.s_addr = htonl(hostAddr);

    ssize_t sent = sendto(ps->fd, buf + offset, (size_t)count, 0,
                          (struct sockaddr*)&dest, sizeof(dest));
    if (sent < 0) {
        if (SOCK_LAST_ERROR == SOCK_EAGAIN || SOCK_LAST_ERROR == SOCK_EWOULDBLOCK) {
            sent = 0;
        } else {
            ps->sockError = SOCK_LAST_ERROR;
            return vm->primitiveFail();
        }
    }

    vm->popthenPush(7, vm->integerObjectOf((sqInt)sent));
    return 0;
}

// primitiveSocketReceiveUDPDataBufCount
// Pharo method: primSocket: socketID receiveUDPDataInto: buf startingAt: start count: count
// Stack [0]=count [1]=startIndex [2]=data [3]=socketID [4]=receiver
// Returns: Array {bytesReceived, senderAddress (ByteArray 4), senderPort, moreFlag}
extern "C" sqInt sp_primitiveSocketReceiveUDPDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt dataOop    = vm->stackValue(2);
    sqInt socketOop  = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd == INVALID_SOCKET) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    memset(&from, 0, sizeof(from));

    ssize_t received = recvfrom(ps->fd, buf + offset, (size_t)count, 0,
                                (struct sockaddr*)&from, &fromLen);
    bool moreData = false;
    if (received < 0) {
        if (SOCK_LAST_ERROR == SOCK_EAGAIN || SOCK_LAST_ERROR == SOCK_EWOULDBLOCK) {
            received = 0;
        } else {
            ps->sockError = SOCK_LAST_ERROR;
            return vm->primitiveFail();
        }
    } else if (received > 0) {
        // Check if more data from same datagram is available
        char peekBuf;
        int more = (int)recvfrom(ps->fd, &peekBuf, 1, MSG_PEEK | MSG_DONTWAIT, nullptr, nullptr);
        moreData = (more > 0);
    }

    // Build result Array: {bytesReceived, senderAddress (ByteArray 4), senderPort, moreFlag}
    // Allocate ByteArray for sender address first, protect it, then allocate Array
    sqInt addrBA = vm->instantiateClassindexableSize(vm->classByteArray(), 4);
    if (vm->failed()) return vm->primitiveFail();
    uint8_t* addrBuf = (uint8_t*)vm->firstIndexableField(addrBA);
    // Copy sender address in network byte order (4 raw bytes)
    uint32_t rawAddr = from.sin_addr.s_addr; // already network byte order
    memcpy(addrBuf, &rawAddr, 4);

    vm->pushRemappableOop(addrBA);
    sqInt resultArray = vm->instantiateClassindexableSize(vm->classArray(), 4);
    addrBA = vm->popRemappableOop();
    if (vm->failed()) return vm->primitiveFail();

    // Fill the 4-element Array
    vm->storePointerofObjectwithValue(0, resultArray, vm->integerObjectOf((sqInt)received));
    vm->storePointerofObjectwithValue(1, resultArray, addrBA);
    vm->storePointerofObjectwithValue(2, resultArray, vm->integerObjectOf(ntohs(from.sin_port)));
    vm->storePointerofObjectwithValue(3, resultArray, moreData ? vm->trueObject() : vm->falseObject());

    vm->popthenPush(5, resultArray); // pop receiver + 4 args, push result
    return 0;
}

// =====================================================================
// Plugin initialization (called from InterpreterProxy.cpp)
// =====================================================================

extern "C" sqInt SocketPlugin_setInterpreter(VirtualMachine* anInterpreter) {
#ifdef DEBUG
    fprintf(stderr, "[SOCK] SocketPlugin_setInterpreter called (proxy=%p)\n", (void*)anInterpreter);
#endif
    vm = anInterpreter;
    socketPluginInit();
    return 0;
}
