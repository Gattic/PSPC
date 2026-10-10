// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc_net: the host side of PSPC's networking, shared by the network HLE libraries (sceNetInet,
// sceNetAdhoc*, sceHttp, ...) and by pspc's built-in ad hoc server. Nothing here knows about guest
// memory or guest threads.
//
//  - Winsock start-up.
//  - Non-blocking sockets that carry a readiness event (WSAEventSelect on every network event).
//    Sockets are never put in blocking mode: a guest call that blocks waits for the event.
//  - A poller thread that tells whoever waits when one of a set of socket events is signalled
//    (one-shot watches). The guest side turns that into a wake-up of a sleeping guest thread.
//  - PSP <-> host conversions: errno values (newlib numbering), struct sockaddr_in, socket options.
//  - The host's own network configuration (IP, netmask, gateway and DNS of the adapter that holds
//    the default route), which PSPC reports as the virtual access point's DHCP lease.
//
// Readiness events are edge-like (Winsock records an event once until the call that re-enables it,
// e.g. recv for FD_READ), so waiters always re-check the real state with CheckReady after a wake-up
// and also wake up periodically; see WaitHostEvents in the runtime (hle/modules/net_host.cpp).
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <functional>
#include <string>
#include <vector>

#include "common/common.h"

namespace hostnet {

// WSAStartup, once per process. Returns false if Winsock is unusable.
bool Startup();

// ---------------------------------------------------------------- PSP constants

// Errno values of the PSP's BSD socket layer (newlib numbering), as sceNetInetGetErrno returns
// them.
enum PspErrno : int {
	PSP_EINTR = 4,
	PSP_EBADF = 9,
	PSP_EAGAIN = 11,
	PSP_ENOMEM = 12,
	PSP_EACCES = 13,
	PSP_EFAULT = 14,
	PSP_EINVAL = 22,
	PSP_EMFILE = 24,
	PSP_ENOSPC = 28,
	PSP_EPIPE = 32,
	PSP_EOPNOTSUPP = 95,
	PSP_EPFNOSUPPORT = 96,
	PSP_ECONNRESET = 104,
	PSP_ENOBUFS = 105,
	PSP_EAFNOSUPPORT = 106,
	PSP_EPROTOTYPE = 107,
	PSP_ENOTSOCK = 108,
	PSP_ENOPROTOOPT = 109,
	PSP_ESHUTDOWN = 110,
	PSP_ECONNREFUSED = 111,
	PSP_EADDRINUSE = 112,
	PSP_ECONNABORTED = 113,
	PSP_ENETUNREACH = 114,
	PSP_ENETDOWN = 115,
	PSP_ETIMEDOUT = 116,
	PSP_EHOSTDOWN = 117,
	PSP_EHOSTUNREACH = 118,
	PSP_EINPROGRESS = 119,
	PSP_EALREADY = 120,
	PSP_EDESTADDRREQ = 121,
	PSP_EMSGSIZE = 122,
	PSP_EPROTONOSUPPORT = 123,
	PSP_ESOCKTNOSUPPORT = 124,
	PSP_EADDRNOTAVAIL = 125,
	PSP_ENETRESET = 126,
	PSP_EISCONN = 127,
	PSP_ENOTCONN = 128,
};

// Winsock error (WSAGetLastError) -> PSP errno. connect's WSAEWOULDBLOCK is EINPROGRESS; pass
// forConnect for that call.
int PspErrnoFromWsa(int wsaError, bool forConnect = false);
// "EAGAIN", "ECONNREFUSED"... for logs.
const char *PspErrnoName(int pspErrno);

// The PSP's struct sockaddr_in: u8 sin_len, u8 sin_family (AF_INET = 2), u16 port and u32 address
// in network byte order, 8 bytes of padding.
#pragma pack(push, 1)
struct PspSockaddrIn {
	u8 len;
	u8 family;
	u16 port;  // network byte order
	u32 addr;  // network byte order
	u8 zero[8];
};
#pragma pack(pop)
static_assert(sizeof(PspSockaddrIn) == 16, "PspSockaddrIn");

constexpr int PSP_AF_INET = 2;
sockaddr_in ToHost(const PspSockaddrIn &a);
PspSockaddrIn ToPsp(const sockaddr_in &a);

// Socket option levels and names. The PSP's stack is BSD-derived and so is Winsock's numbering for
// SOL_SOCKET (0xFFFF) options, the IP options and TCP_NODELAY; TCP_MAXSEG and a few BSD-only
// options differ or don't exist on Windows. Returns false for an option Windows can't express.
constexpr int PSP_SOL_SOCKET = 0xFFFF;
constexpr int PSP_IPPROTO_IP = 0, PSP_IPPROTO_TCP = 6, PSP_IPPROTO_UDP = 17;
enum PspSockopt : int {
	PSP_SO_DEBUG = 0x0001,
	PSP_SO_ACCEPTCONN = 0x0002,
	PSP_SO_REUSEADDR = 0x0004,
	PSP_SO_KEEPALIVE = 0x0008,
	PSP_SO_DONTROUTE = 0x0010,
	PSP_SO_BROADCAST = 0x0020,
	PSP_SO_USELOOPBACK = 0x0040,
	PSP_SO_LINGER = 0x0080,
	PSP_SO_OOBINLINE = 0x0100,
	PSP_SO_REUSEPORT = 0x0200,
	PSP_SO_TIMESTAMP = 0x0400,
	PSP_SO_ONESBCAST = 0x0800,
	PSP_SO_SNDBUF = 0x1001,
	PSP_SO_RCVBUF = 0x1002,
	PSP_SO_SNDLOWAT = 0x1003,
	PSP_SO_RCVLOWAT = 0x1004,
	PSP_SO_SNDTIMEO = 0x1005,
	PSP_SO_RCVTIMEO = 0x1006,
	PSP_SO_ERROR = 0x1007,
	PSP_SO_TYPE = 0x1008,
	PSP_SO_NBIO = 0x1009,
	PSP_SO_BIO = 0x100A,
	PSP_SO_NOSIGPIPE = 0x1022,
	PSP_TCP_NODELAY = 0x01,
	PSP_TCP_MAXSEG = 0x02,
	PSP_IP_OPTIONS = 1,
	PSP_IP_HDRINCL = 2,
	PSP_IP_TOS = 3,
	PSP_IP_TTL = 4,
	PSP_IP_MULTICAST_IF = 9,
	PSP_IP_MULTICAST_TTL = 10,
	PSP_IP_MULTICAST_LOOP = 11,
	PSP_IP_ADD_MEMBERSHIP = 12,
	PSP_IP_DROP_MEMBERSHIP = 13,
};
bool MapSockopt(int pspLevel, int pspName, int *hostLevel, int *hostName);

// recv/send flags (MSG_*). The PSP-only MSG_DONTWAIT (0x80) and MSG_WAITALL are handled by the
// caller (sockets are always non-blocking on the host); the rest are passed on.
constexpr int PSP_MSG_OOB = 0x1, PSP_MSG_PEEK = 0x2, PSP_MSG_DONTROUTE = 0x4, PSP_MSG_EOR = 0x8,
              PSP_MSG_TRUNC = 0x10, PSP_MSG_CTRUNC = 0x20, PSP_MSG_WAITALL = 0x40, PSP_MSG_DONTWAIT = 0x80;
int MapMsgFlags(int pspFlags);

// ---------------------------------------------------------------- sockets

// A host socket and its readiness event.
struct Socket {
	SOCKET s = INVALID_SOCKET;
	WSAEVENT ev = WSA_INVALID_EVENT;
	bool Valid() const { return s != INVALID_SOCKET; }
};

// Creates a non-blocking socket with a readiness event. UDP sockets don't report ICMP "port
// unreachable" as WSAECONNRESET on later receives (the BSD behaviour). On failure returns an invalid
// Socket and sets *wsaError.
Socket OpenSocket(int af, int type, int protocol, int *wsaError);
// Gives a socket that accept() returned its own readiness event (an accepted socket inherits the
// listening socket's WSAEventSelect, event object included).
bool AdoptSocket(SOCKET s, Socket *out, int *wsaError);
// Closes the socket (abortive with RST when `reset`) and releases its event (see ReleaseEvent).
void CloseSocket(Socket &sock, bool reset = false);

struct Readiness {
	bool read = false;    // data, end of stream, or a connection to accept
	bool write = false;   // room to send, or a connect that completed
	bool except = false;  // a connect that failed, or out-of-band data
};
// The socket's state right now (select with a zero timeout). False if select failed (bad socket).
bool CheckReady(SOCKET s, Readiness *r);
// The pending error of a socket (SO_ERROR), as a Winsock error code; reading it clears it.
int PendingError(SOCKET s);
// Bytes that a recv would return now (FIONREAD).
u32 BytesAvailable(SOCKET s);

// ---------------------------------------------------------------- poller

using WatchId = u64;
// Calls `fired` once, on the poller thread, as soon as any of the events is signalled. The events
// that fired are reset. Watching nothing never fires. Thread-safe.
WatchId Watch(const WSAEVENT *events, size_t count, std::function<void()> fired);
// Cancels a watch that hasn't fired (no-op otherwise). `fired` may still be running, or about to,
// on the poller thread when this returns.
void Unwatch(WatchId id);
// Closes an event once the poller no longer waits on it, and cancels the watches that use it.
void ReleaseEvent(WSAEVENT ev);
// Stops the poller thread (tests; the runtime lets it end with the process).
void StopPoller();

// ---------------------------------------------------------------- the host's configuration

struct HostConfig {
	bool valid = false;      // an adapter with an IPv4 address holds the default route
	u32 ip = 0;              // all in network byte order
	u32 netmask = 0;
	u32 gateway = 0;
	u32 dns[2] = {0, 0};
	std::string adapter;     // friendly name, for logs
};
// The adapter that the route to the internet goes through (no traffic is sent to find it). Falls
// back to loopback (127.0.0.1/8, no gateway or DNS) when the host has no network.
HostConfig QueryHostConfig();

// Dotted-quad text of an address in network byte order.
std::string Ipv4ToString(u32 addrNbo);
// Parses a strict dotted quad ("192.168.0.1") into network byte order.
bool ParseIpv4(const char *s, u32 *addrNbo);
// Whether an address (network byte order) is in 127.0.0.0/8.
inline bool IsLoopback(u32 addrNbo) { return (addrNbo & 0xFF) == 127; }

// ---------------------------------------------------------------- name resolution

// getaddrinfo for IPv4 (blocking: call it on a worker thread). Returns 0 with the first address
// (network byte order), or the getaddrinfo error (WSAHOST_NOT_FOUND, WSATRY_AGAIN...).
int ResolveIpv4(const std::string &host, u32 *addrNbo);
// getnameinfo for an IPv4 address (blocking). Returns 0 and the name, or the error.
int ReverseLookup(u32 addrNbo, std::string *name);

}  // namespace hostnet
