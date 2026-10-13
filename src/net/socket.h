// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/common.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <cerrno>
#include <unistd.h>
#endif

namespace hostnet {
#ifdef _WIN32
using SocketHandle = SOCKET;
using PollFd = WSAPOLLFD;
inline constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using PollFd = pollfd;
inline constexpr SocketHandle InvalidSocket = -1;
#endif
inline constexpr int SocketError = -1;
using EventHandle = void *;
inline constexpr EventHandle InvalidEvent = nullptr;
#ifdef _WIN32
#define PSPC_SOCKET_ERROR(name) inline constexpr int Error_##name = WSA##name
#else
#define PSPC_SOCKET_ERROR(name) inline constexpr int Error_##name = name
#endif
PSPC_SOCKET_ERROR(EINTR); PSPC_SOCKET_ERROR(EBADF); PSPC_SOCKET_ERROR(EACCES);
PSPC_SOCKET_ERROR(EFAULT); PSPC_SOCKET_ERROR(EINVAL); PSPC_SOCKET_ERROR(EMFILE);
PSPC_SOCKET_ERROR(EWOULDBLOCK); PSPC_SOCKET_ERROR(EINPROGRESS); PSPC_SOCKET_ERROR(EALREADY);
PSPC_SOCKET_ERROR(ENOTSOCK); PSPC_SOCKET_ERROR(EDESTADDRREQ); PSPC_SOCKET_ERROR(EMSGSIZE);
PSPC_SOCKET_ERROR(EPROTOTYPE); PSPC_SOCKET_ERROR(ENOPROTOOPT); PSPC_SOCKET_ERROR(EPROTONOSUPPORT);
PSPC_SOCKET_ERROR(ESOCKTNOSUPPORT); PSPC_SOCKET_ERROR(EOPNOTSUPP); PSPC_SOCKET_ERROR(EPFNOSUPPORT);
PSPC_SOCKET_ERROR(EAFNOSUPPORT); PSPC_SOCKET_ERROR(EADDRINUSE); PSPC_SOCKET_ERROR(EADDRNOTAVAIL);
PSPC_SOCKET_ERROR(ENETDOWN); PSPC_SOCKET_ERROR(ENETUNREACH); PSPC_SOCKET_ERROR(ENETRESET);
PSPC_SOCKET_ERROR(ECONNABORTED); PSPC_SOCKET_ERROR(ECONNRESET); PSPC_SOCKET_ERROR(ENOBUFS);
PSPC_SOCKET_ERROR(EISCONN); PSPC_SOCKET_ERROR(ENOTCONN); PSPC_SOCKET_ERROR(ESHUTDOWN);
PSPC_SOCKET_ERROR(ETIMEDOUT); PSPC_SOCKET_ERROR(ECONNREFUSED); PSPC_SOCKET_ERROR(EHOSTDOWN);
PSPC_SOCKET_ERROR(EHOSTUNREACH);
#undef PSPC_SOCKET_ERROR
#ifdef _WIN32
inline constexpr int Error_NOT_ENOUGH_MEMORY = WSA_NOT_ENOUGH_MEMORY;
inline constexpr int Error_NOTINITIALISED = WSANOTINITIALISED;
inline constexpr int Error_HOST_NOT_FOUND = WSAHOST_NOT_FOUND;
inline constexpr int Error_TRY_AGAIN = WSATRY_AGAIN;
inline constexpr int Error_NO_RECOVERY = WSANO_RECOVERY;
inline constexpr int Error_NO_DATA = WSANO_DATA;
inline constexpr int ShutRead = SD_RECEIVE, ShutWrite = SD_SEND, ShutBoth = SD_BOTH;
#else
inline constexpr int Error_NOT_ENOUGH_MEMORY = ENOMEM;
inline constexpr int Error_NOTINITIALISED = ENETDOWN;
inline constexpr int Error_HOST_NOT_FOUND = EAI_NONAME;
inline constexpr int Error_TRY_AGAIN = EAI_AGAIN;
inline constexpr int Error_NO_RECOVERY = EAI_FAIL;
inline constexpr int Error_NO_DATA = EAI_NODATA;
inline constexpr int ShutRead = SHUT_RD, ShutWrite = SHUT_WR, ShutBoth = SHUT_RDWR;
#endif
int LastSocketError();
int CloseSocketHandle(SocketHandle s);
int SocketIoctl(SocketHandle s, long command, unsigned long *value);
int Poll(PollFd *fds, size_t count, int timeoutMs);
SocketHandle Accept(SocketHandle s, sockaddr *address, int *length);
int GetSockName(SocketHandle s, sockaddr *address, int *length);
int GetPeerName(SocketHandle s, sockaddr *address, int *length);
int GetSockOpt(SocketHandle s, int level, int name, char *value, int *length);
// Datagram truncation copies the prefix, then returns SocketError with Error_EMSGSIZE on every host.
int RecvFrom(SocketHandle s, char *data, int size, int flags, sockaddr *from, int *length);
int Send(SocketHandle s, const char *data, int size, int flags);
int SendTo(SocketHandle s, const char *data, int size, int flags, const sockaddr *to, int length);

EventHandle NewEvent(bool manual = false);
void SignalEvent(EventHandle event);
void ResetEvent(EventHandle event);
void CloseEvent(EventHandle event);
// Returns index of a ready event, -1 on timeout, -2 on error. Auto-reset events are consumed.
int WaitEvents(const EventHandle *events, size_t count, int timeoutMs);
EventHandle SocketEvent(SocketHandle socket);
}
