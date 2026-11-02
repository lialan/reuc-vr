#pragma once
// The Winsock names netplay uses, on BSD sockets off Windows, so the network code is one source on every host.
// uc_socklen: the address-length type of getsockopt/recvfrom; UC_SEND_FLAGS and uc_socket_no_sigpipe: a send to a closed TCP peer returns an
// error instead of raising SIGPIPE (which would end the process) on POSIX.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using uc_socklen = int;
constexpr int UC_SEND_FLAGS = 0;
inline void uc_socket_no_sigpipe(SOCKET) {}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using u_long = unsigned long;
using BOOL = int;
#ifndef TRUE
#define TRUE 1
#endif
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
constexpr int SD_SEND = SHUT_WR;
constexpr int WSAEWOULDBLOCK = EWOULDBLOCK, WSAEINPROGRESS = EINPROGRESS, WSAEADDRINUSE = EADDRINUSE, WSAEACCES = EACCES,
              WSAECONNREFUSED = ECONNREFUSED, WSAECONNRESET = ECONNRESET, WSAEMSGSIZE = EMSGSIZE, WSAENOBUFS = ENOBUFS;
inline int WSAGetLastError() { return errno; }
inline int closesocket(SOCKET s) { return ::close(s); }
inline int ioctlsocket(SOCKET s, unsigned long command, u_long* value) { int v = int(*value); return ::ioctl(s, command, &v); }
inline int InetPtonA(int family, const char* text, void* address) { return ::inet_pton(family, text, address); }
using uc_socklen = socklen_t;
#ifdef MSG_NOSIGNAL
constexpr int UC_SEND_FLAGS = MSG_NOSIGNAL;   // Linux, Android
#else
constexpr int UC_SEND_FLAGS = 0;
#endif
inline void uc_socket_no_sigpipe(SOCKET s) {
#ifdef SO_NOSIGPIPE   // macOS has no MSG_NOSIGNAL: the option on the socket instead
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)s;
#endif
}
#endif
