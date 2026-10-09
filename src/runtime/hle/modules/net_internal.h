// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared pieces of the network libraries (net*.cpp). PSPC runs PSP software offline: the WLAN
// switch reads "off", so the libraries initialize normally but anything that needs an access
// point, a DNS server or ad hoc peers fails the way the firmware does without a network.
#pragma once

#include "common/common.h"

namespace net {

// Registration of the libraries implemented in the other net_*.cpp files (called by
// Register_NetLibraries in net.cpp).
void RegisterParseLibraries();  // net_parse.cpp: sceParseUri, sceParseHttp
void RegisterHttpLibraries();   // net_http.cpp: sceHttp, sceSsl, sceHttpStorage
void RegisterAdhocLibraries();  // net_adhoc.cpp: sceNetAdhoc, sceNetAdhocctl, sceNetAdhocMatching, sceNetAdhocDiscover
void RegisterNpLibraries();     // net_np.cpp: sceNp, sceNpAuth, sceNpService

// The console's (fixed) wireless MAC address.
extern const u8 kLocalMac[6];
// Player nickname reported by the network libraries.
constexpr const char *kNickname = "PSPC";

// State owned by net.cpp.
bool NetInited();
// Called by sceNetTerm: the ad hoc libraries shut down with the core library.
void AdhocShutdown();

// Errno values of the PSP's BSD socket layer (newlib numbering), as sceNetInetGetErrno returns them.
enum InetErrno : int {
	INET_EINTR = 4,
	INET_EBADF = 9,
	INET_EAGAIN = 11,
	INET_EACCES = 13,
	INET_EFAULT = 14,
	INET_EINVAL = 22,
	INET_ENOSPC = 28,
	INET_EPIPE = 32,
	INET_EOPNOTSUPP = 95,
	INET_EPFNOSUPPORT = 96,
	INET_ECONNRESET = 104,
	INET_ENOBUFS = 105,
	INET_EAFNOSUPPORT = 106,
	INET_EPROTOTYPE = 107,
	INET_ENOTSOCK = 108,
	INET_ENOPROTOOPT = 109,
	INET_ESHUTDOWN = 110,
	INET_ECONNREFUSED = 111,
	INET_EADDRINUSE = 112,
	INET_ECONNABORTED = 113,
	INET_ENETUNREACH = 114,
	INET_ENETDOWN = 115,
	INET_ETIMEDOUT = 116,
	INET_EHOSTDOWN = 117,
	INET_EHOSTUNREACH = 118,
	INET_EINPROGRESS = 119,
	INET_EALREADY = 120,
	INET_EDESTADDRREQ = 121,
	INET_EMSGSIZE = 122,
	INET_EPROTONOSUPPORT = 123,
	INET_ESOCKTNOSUPPORT = 124,
	INET_EADDRNOTAVAIL = 125,
	INET_EISCONN = 127,
	INET_ENOTCONN = 128,
};

// Error codes of the core / WLAN layers used by more than one file.
constexpr u32 SCE_NET_RESOLVER_NO_DNS_SERVER = 0x80410402;

}  // namespace net
