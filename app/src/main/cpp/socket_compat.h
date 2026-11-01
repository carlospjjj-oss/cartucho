// Cartucho - BSD sockets on POSIX and Windows (Winsock), the subset the
// network code uses. Sockets are kept in an int (valid Winsock handles fit).
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace cartucho {

inline void SockInit() {
    static bool done = false;
    if (!done) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
}
inline int SockClose(int fd) { return closesocket(static_cast<SOCKET>(fd)); }
inline int SockShutdown(int fd) { return shutdown(static_cast<SOCKET>(fd), SD_BOTH); }
inline void SockNonBlocking(int fd, bool on) {
    u_long v = on ? 1 : 0;
    ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &v);
}
inline void SockRecvTimeout(int fd, int ms) {
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}
inline int SockPoll(pollfd* p, unsigned n, int ms) { return WSAPoll(p, n, ms); }
inline int SockError() { return WSAGetLastError(); }
inline bool SockInProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline bool SockRetry(int e) { return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT || e == WSAEINTR; }
inline long long SockRecv(int fd, void* buf, size_t n) {
    return recv(static_cast<SOCKET>(fd), static_cast<char*>(buf), static_cast<int>(n), 0);
}
inline long long SockSend(int fd, const void* buf, size_t n) {
    return send(static_cast<SOCKET>(fd), static_cast<const char*>(buf), static_cast<int>(n), 0);
}
template <typename T>
inline int SockOpt(int fd, int level, int name, const T& v) {
    return setsockopt(static_cast<SOCKET>(fd), level, name, reinterpret_cast<const char*>(&v), sizeof(v));
}
inline int SockOpen(int family, int type, int proto) {
    SockInit();
    const SOCKET s = socket(family, type, proto);
    return s == INVALID_SOCKET ? -1 : static_cast<int>(s);
}
inline int SockAccept(int fd) {
    const SOCKET s = accept(static_cast<SOCKET>(fd), nullptr, nullptr);
    return s == INVALID_SOCKET ? -1 : static_cast<int>(s);
}

}  // namespace cartucho

#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace cartucho {

inline void SockInit() {}
inline int SockClose(int fd) { return close(fd); }
inline int SockShutdown(int fd) { return shutdown(fd, SHUT_RDWR); }
inline void SockNonBlocking(int fd, bool on) {
    const int f = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}
inline void SockRecvTimeout(int fd, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
inline int SockPoll(pollfd* p, unsigned n, int ms) { return poll(p, n, ms); }
inline int SockError() { return errno; }
inline bool SockInProgress(int e) { return e == EINPROGRESS; }
inline bool SockRetry(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
inline long long SockRecv(int fd, void* buf, size_t n) { return recv(fd, buf, n, 0); }
inline long long SockSend(int fd, const void* buf, size_t n) {
#ifdef MSG_NOSIGNAL
    return send(fd, buf, n, MSG_NOSIGNAL);
#else
    return send(fd, buf, n, 0);
#endif
}
template <typename T>
inline int SockOpt(int fd, int level, int name, const T& v) { return setsockopt(fd, level, name, &v, sizeof(v)); }
inline int SockOpen(int family, int type, int proto) { return socket(family, type, proto); }
inline int SockAccept(int fd) { return accept(fd, nullptr, nullptr); }

}  // namespace cartucho
#endif
