// SPDX-License-Identifier: GPL-2.0-or-later
//
// Network libraries, offline: sceNet (pspnet core), sceNet_lib, sceNetInet, sceNetApctl,
// sceNetResolver, sceWlanDrv / sceWlanDrv_lib, sceNetUpnp, sceNetIfhandle. The other network
// libraries live in net_parse.cpp, net_http.cpp, net_adhoc.cpp and net_np.cpp.
//
// PSPC has no network: the WLAN switch reads off, so the libraries start up as usual but connecting
// to an access point fails with WLAN_SWITCH_OFF, names don't resolve (no DNS server) and sockets
// never reach anyone.
//
// Function set, argument checks and structures ported from PPSSPP (Core/HLE/sceNet.cpp,
// sceNet_lib.cpp, sceNetInet.cpp, sceNetResolver.cpp, NetInetConstants.h), Copyright (c) 2012-
// PPSSPP Project, GPL-2.0-or-later. sceNetEtherNtostr/Strton follow pspnet.prx as measured on
// hardware (pspautotests net/primary/ether).
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/misc_libs.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/thread.h"

namespace net {
// Fixed MAC: a unicast, globally administered address (games such as Gran Turismo check the two
// low bits of the first byte).
const u8 kLocalMac[6] = {0x00, 0x16, 0xFE, 0x50, 0x53, 0x43};
}  // namespace net

namespace {

constexpr const char *LOG = "NET";

// pspnet_wlan errors.
constexpr u32 ERROR_NET_WLAN_POWER_OFF = 0x80410D0C;
constexpr u32 ERROR_NET_WLAN_INVALID_ARG = 0x80410D13;

// ================================================================ sceNet (pspnet core)

struct NetMallocStat {
	s32 pool;
	s32 maximum;
	s32 free;
};

bool g_netInited = false;
u32 g_netPoolAddr = 0, g_netThread1Addr = 0, g_netThread2Addr = 0;
NetMallocStat g_netMallocStat{};
u32 g_netDropRate = 0, g_netDropDuration = 0;

u32 AllocUser(u32 size, bool fromTop, const char *tag) { return kernel::g_userMem.Alloc(size, fromTop, tag); }
void FreeUser(u32 &addr) {
	if (addr) kernel::g_userMem.Free(addr);
	addr = 0;
}

void ApctlTerm();

void NetTerm() {
	net::AdhocShutdown();
	ApctlTerm();
	FreeUser(g_netPoolAddr);
	FreeUser(g_netThread1Addr);
	FreeUser(g_netThread2Addr);
	g_netInited = false;
}

int sceNetInit(u32 poolSize, u32 calloutPri, u32 calloutStack, u32 netinitPri, u32 netinitStack) {
	// Like PPSSPP: a second init starts over rather than leaking the first pool and threads.
	if (g_netInited) NetTerm();
	if (poolSize == 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE;
	if (calloutPri < 0x08 || calloutPri > 0x77) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	if (netinitPri < 0x08 || netinitPri > 0x77) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;

	// The library takes its pool and the stacks of its two threads (4 KB each whatever the
	// arguments say, past firmware 1.50) from user memory.
	g_netThread1Addr = AllocUser(4096, true, "SceNetCallout stack");
	g_netThread2Addr = AllocUser(4096, true, "SceNetNetintr stack");
	g_netPoolAddr = AllocUser(poolSize, false, "SceNet pool");
	if (!g_netThread1Addr || !g_netThread2Addr || !g_netPoolAddr) {
		FreeUser(g_netPoolAddr);
		FreeUser(g_netThread1Addr);
		FreeUser(g_netThread2Addr);
		return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	}
	// Vantage Master Portable checks these against what it is about to send.
	g_netMallocStat.pool = (s32)poolSize - 0x20;
	g_netMallocStat.maximum = 0x4050;
	g_netMallocStat.free = g_netMallocStat.pool;
	g_netInited = true;
	LOGI(LOG, "sceNetInit(pool=%u, calloutPri=%u, calloutStack=%u, netintrPri=%u, netintrStack=%u)", poolSize,
	     calloutPri, calloutStack, netinitPri, netinitStack);
	return 0;
}

int sceNetTerm() {
	NetTerm();
	return 0;
}

int sceNetFreeThreadinfo(int) { return 0; }
int sceNetThreadAbort(int) { return 0; }

int sceNetGetMallocStat(u32 statAddr) {
	if (!mem::IsValidRange(statAddr, sizeof(NetMallocStat))) return 0;
	mem::WriteBytes(statAddr, &g_netMallocStat, sizeof(g_netMallocStat));
	return 0;
}

int sceNetGetLocalEtherAddr(u32 addrAddr) {
	if (!mem::IsValidRange(addrAddr, 6)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	mem::WriteBytes(addrAddr, net::kLocalMac, 6);
	return 0;
}

// Formats a MAC as "xx:xx:xx:xx:xx:xx" and returns the length. With a null argument pspnet.prx
// returns whatever $v0 held - the high half of an address inside the library (the `lui` of its data
// pointer), 0x09D40000 where the firmware loads it. Report that.
int sceNetEtherNtostr(u32 macAddr, u32 nameAddr) {
	if (!macAddr || !nameAddr || !mem::IsValidRange(macAddr, 6) || !mem::IsValidRange(nameAddr, 18)) return 0x09D40000;
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	char buf[18];
	int len = snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	mem::WriteBytes(nameAddr, buf, (u32)len + 1);
	return len;
}

// pspnet.prx's digit conversion: letters count from 10 whatever their case and whether or not they
// are hex digits; anything else is 9999999.
int EtherDigit(u8 c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'z') return c - 'a' + 10;
	if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
	return 9999999;
}

// Parses six "HH?" groups: two characters make a byte (16 * first + second), one separator of any
// kind is skipped, and the string's end isn't looked for - a short string reads on past its NUL,
// exactly as the firmware does. Returns the last group's full value.
int sceNetEtherStrton(u32 nameAddr, u32 macAddr) {
	if (!nameAddr || !mem::IsValidAddress(nameAddr)) return 0;
	int value = 0;
	u8 mac[6];
	u32 p = nameAddr;
	for (int i = 0; i < 6; i++) {
		int hi = EtherDigit(mem::IsValidAddress(p) ? GRead8(p) : 0);
		int lo = EtherDigit(mem::IsValidAddress(p + 1) ? GRead8(p + 1) : 0);
		value = hi * 16 + lo;
		mac[i] = (u8)value;
		p += 3;
	}
	if (mem::IsValidRange(macAddr, 6)) mem::WriteBytes(macAddr, mac, 6);
	return value;
}

// ---------------------------------------------------------------- sceNetIfhandle

int sceNetGetDropRate(u32 dropRateAddr, u32 dropDurationAddr) {
	if (mem::IsValidRange(dropRateAddr, 4)) GWrite32(dropRateAddr, g_netDropRate);
	if (mem::IsValidRange(dropDurationAddr, 4)) GWrite32(dropDurationAddr, g_netDropDuration);
	return 0;
}
int sceNetSetDropRate(u32 dropRate, u32 dropDuration) {
	g_netDropRate = dropRate;
	g_netDropDuration = dropDuration;
	return 0;
}

// ---------------------------------------------------------------- sceNetUpnp
// No router to talk to; the library itself comes up fine.

int sceNetUpnpInit(int, int) { return 0; }
int sceNetUpnpStart() { return 0; }
int sceNetUpnpStop() { return 0; }
int sceNetUpnpTerm() { return 0; }
int sceNetUpnpGetNatInfo() { return 0; }

// ---------------------------------------------------------------- sceNet_lib
// Internal pspnet exports (string helpers used by the other network modules).

u32 sceNetStrtoul(u32 strAddr, u32 endAddrPtr, int base) {
	const char *str = mem::GetCString(strAddr);
	if (!str) return 0;
	char *end = nullptr;
	u32 res = (u32)strtoul(str, &end, base);
	if (mem::IsValidRange(endAddrPtr, 4)) GWrite32(endAddrPtr, strAddr + (u32)(end - str));
	return res;
}
u32 sceNetMemmove(u32 dst, u32 src, u32 count) {
	if (count && mem::IsValidRange(dst, count) && mem::IsValidRange(src, count)) memmove(GPtr(dst), GPtr(src), count);
	return dst;
}
u32 sceNetStrcpy(u32 dst, u32 srcAddr) {
	const char *src = mem::GetCString(srcAddr);
	if (src && mem::IsValidRange(dst, (u32)strlen(src) + 1)) mem::WriteBytes(dst, src, (u32)strlen(src) + 1);
	return dst;
}
u32 sceNetStrncpy(u32 dst, u32 srcAddr, u32 count) {
	if (!count || !mem::IsValidRange(dst, count)) return dst;
	std::string src = mem::ReadString(srcAddr, count);
	mem::Memset(dst, 0, count);
	mem::WriteBytes(dst, src.data(), (u32)std::min<size_t>(src.size(), count));
	return dst;
}
int sceNetStrcmp(const char *a, const char *b) { return a && b ? strcmp(a, b) : 0; }
int sceNetStrncmp(const char *a, const char *b, u32 n) { return a && b ? strncmp(a, b, n) : 0; }
int sceNetStrcasecmp(const char *a, const char *b) { return a && b ? _stricmp(a, b) : 0; }
u32 sceNetStrlen(const char *s) { return s ? (u32)strlen(s) : 0; }
u32 sceNetStrchr(u32 strAddr, int ch) {
	const char *s = mem::GetCString(strAddr);
	if (!s) return 0;
	const char *r = strchr(s, ch);
	return r ? strAddr + (u32)(r - s) : 0;
}
u32 sceNetRand() {
	static u32 state = 0x12345678;
	state = state * 1103515245u + 12345u;
	return state >> 1;
}
int sceNetMemcmp(u32 a, u32 b, u32 n) {
	if (!n || !mem::IsValidRange(a, n) || !mem::IsValidRange(b, n)) return 0;
	return memcmp(GPtr(a), GPtr(b), n);
}

// ================================================================ sceWlanDrv
// The WLAN switch is off.

u32 sceWlanGetSwitchState() { return 0; }
u32 sceWlanDevIsPowerOn() { return 0; }
int sceWlanGetEtherAddr(u32 addrAddr) {
	if (!mem::IsValidRange(addrAddr, 6)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	mem::WriteBytes(addrAddr, net::kLocalMac, 6);
	return 0;
}
int sceWlanDevGetMacAddr(u32 addrAddr) { return sceWlanGetEtherAddr(addrAddr); }
int sceWlanDevIsGameMode() { return 0; }
int sceWlanDevGetStateGPIO() { return 0; }
int sceWlanDevAttach() { return (int)ERROR_NET_WLAN_POWER_OFF; }
int sceWlanDevDetach() { return 0; }
int sceWlanOk() { return 0; }

// ================================================================ sceNetApctl

enum ApctlState : u32 {
	APCTL_STATE_DISCONNECTED = 0,
};

constexpr u32 APCTL_PROFILENAME_MAXLEN = 64;
constexpr u32 APCTL_SSID_MAXLEN = 32;
constexpr u32 APCTL_IPADDR_MAXLEN = 16;
constexpr u32 APCTL_URL_MAXLEN = 128;
constexpr int MAX_APCTL_HANDLERS = 32;

bool g_apctlInited = false;
u32 g_apctlState = APCTL_STATE_DISCONNECTED;
std::map<int, std::pair<u32, u32>> g_apctlHandlers;  // id -> (entry, arg)

void ApctlTerm() {
	g_apctlHandlers.clear();
	g_apctlInited = false;
	g_apctlState = APCTL_STATE_DISCONNECTED;
}

int sceNetApctlInit(int stackSize, int initPriority) {
	if (g_apctlInited) return (int)SCE_NET_APCTL_ERROR_ALREADY_INITIALIZED;
	g_apctlState = APCTL_STATE_DISCONNECTED;
	g_apctlInited = true;
	LOGI(LOG, "sceNetApctlInit(%d, %d)", stackSize, initPriority);
	return 0;
}

int sceNetApctlTerm() {
	ApctlTerm();
	return 0;
}

int sceNetApctlConnect(int confId) {
	LOGI(LOG, "sceNetApctlConnect(%d): WLAN switch is off", confId);
	return (int)SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF;
}
int sceNetApctlConnectWithMac(int confId, u32) { return sceNetApctlConnect(confId); }

int sceNetApctlDisconnect() {
	g_apctlState = APCTL_STATE_DISCONNECTED;
	return 0;
}

int sceNetApctlGetState(u32 stateAddr) {
	if (!mem::IsValidRange(stateAddr, 4)) return (int)ERROR_NET_WLAN_INVALID_ARG;
	GWrite32(stateAddr, g_apctlState);
	return 0;
}

int sceNetApctlAddHandler(u32 handler, u32 arg) {
	if (!mem::IsValidAddress(handler)) return (int)ERROR_NET_WLAN_INVALID_ARG;
	if ((int)g_apctlHandlers.size() >= MAX_APCTL_HANDLERS) return (int)SCE_NET_ADHOCCTL_ERROR_TOO_MANY_HANDLERS;
	int id = 0;
	while (g_apctlHandlers.count(id)) id++;
	g_apctlHandlers[id] = {handler, arg};
	return id;
}

int sceNetApctlDelHandler(u32 id) {
	auto it = g_apctlHandlers.find((int)id);
	if (it == g_apctlHandlers.end()) return (int)SCE_NET_APCTL_ERROR_INVALID_ID;
	g_apctlHandlers.erase(it);
	return 0;
}

// Not associated with any access point: the info is what an idle interface reports (dotted
// addresses of 0.0.0.0 - Driver 76 wants dots rather than an empty buffer).
int sceNetApctlGetInfo(int code, u32 infoAddr) {
	auto writeStr = [&](const char *s, u32 size) {
		if (!mem::IsValidRange(infoAddr, size)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		mem::Memset(infoAddr, 0, size);
		mem::WriteBytes(infoAddr, s, (u32)std::min<size_t>(strlen(s), size - 1));
		return 0;
	};
	auto writeInt = [&](u32 v, u32 size) {
		if (!mem::IsValidRange(infoAddr, size)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		if (size == 1) GWrite8(infoAddr, (u8)v);
		else if (size == 2) GWrite16(infoAddr, (u16)v);
		else GWrite32(infoAddr, v);
		return 0;
	};
	switch (code) {
	case 0: return writeStr("", APCTL_PROFILENAME_MAXLEN);   // PROFILE_NAME
	case 1: return writeStr("", 6);                          // BSSID (zero)
	case 2: return writeStr("", APCTL_SSID_MAXLEN);          // SSID
	case 3: return writeInt(0, 4);                           // SSID_LENGTH
	case 4: return writeInt(0, 4);                           // SECURITY_TYPE (none)
	case 5: return writeInt(0, 1);                           // STRENGTH
	case 6: return writeInt(0, 1);                           // CHANNEL
	case 7: return writeInt(0, 1);                           // POWER_SAVE
	case 8:                                                  // IP
	case 9:                                                  // SUBNETMASK
	case 10:                                                 // GATEWAY
	case 11:                                                 // PRIMDNS
	case 12: return writeStr("0.0.0.0", APCTL_IPADDR_MAXLEN);  // SECDNS
	case 13: return writeInt(0, 4);                          // USE_PROXY
	case 14: return writeStr("", APCTL_URL_MAXLEN);          // PROXY_URL
	case 15: return writeInt(0, 2);                          // PROXY_PORT
	case 16: return writeInt(0, 4);                          // 8021_EAP_TYPE
	case 17: return writeInt(0, 4);                          // START_BROWSER
	case 18: return writeInt(0, 4);                          // WIFISP
	default: return (int)SCE_NET_APCTL_ERROR_INVALID_CODE;
	}
}

// Scanning needs the radio.
int sceNetApctlScanUser() { return (int)SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF; }

// The scan found nothing: an empty list.
int sceNetApctlGetBSSDescIDListUser(u32 sizeAddr, u32 bufAddr) {
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)ERROR_NET_WLAN_INVALID_ARG;
	GWrite32(sizeAddr, 0);
	return 0;
}
int sceNetApctlGetBSSDescIDList2(u32 sizeAddr, u32 bufAddr, u32, u32) { return sceNetApctlGetBSSDescIDListUser(sizeAddr, bufAddr); }
int sceNetApctlGetBSSDescEntryUser(int entryId, int infoId, u32 resultAddr) {
	if (!mem::IsValidAddress(resultAddr)) return (int)ERROR_NET_WLAN_INVALID_ARG;
	return (int)SCE_NET_APCTL_ERROR_INVALID_ID;
}
int sceNetApctlAddInternalHandler(u32 handler, u32 arg) { return sceNetApctlAddHandler(handler, arg); }
int sceNetApctlDelInternalHandler(u32 id) { return sceNetApctlDelHandler(id); }
int sceNetApctl_lib2_69745F0A(int) { return 0; }

// ================================================================ sceNetInet
// Sockets can be created and configured, but there is no interface to send through: connecting
// and sending fail with ENETUNREACH, and nothing ever arrives.

constexpr int INET_MIN_SOCKET = 1;
constexpr int INET_MAX_SOCKETS = 64;
constexpr int INET_AF_INET = 2;
constexpr int INET_SOCK_STREAM = 1, INET_SOCK_DGRAM = 2, INET_SOCK_RAW = 3;
constexpr int INET_SOL_SOCKET = 0xFFFF;
constexpr int INET_SO_ERROR = 0x1007, INET_SO_TYPE = 0x1008, INET_SO_NBIO = 0x1009, INET_SO_RCVTIMEO = 0x1006,
              INET_SO_SNDBUF = 0x1001, INET_SO_RCVBUF = 0x1002;
constexpr u32 INET_MSG_DONTWAIT = 0x80;
constexpr s32 INET_WAKE_ABORTED = 1;

struct InetSocket {
	bool used = false;
	int domain = 0, type = 0, protocol = 0;
	bool nonblocking = false;
	bool bound = false, listening = false, shutdown = false;
	u32 rcvTimeoutUs = 0;
	std::map<u64, u32> options;  // (level << 32 | name) -> value, for getsockopt
	u8 localAddr[16] = {};       // the sockaddr given to bind
};

bool g_inetInited = false;
InetSocket g_inetSockets[INET_MAX_SOCKETS];
std::map<SceUID, int> g_inetErrno;  // per thread

int SetErrno(int err) {
	g_inetErrno[sched::CurrentId()] = err;
	return -1;
}

InetSocket *GetSocket(int s) {
	if (s < INET_MIN_SOCKET || s >= INET_MAX_SOCKETS || !g_inetSockets[s].used) return nullptr;
	return &g_inetSockets[s];
}

// Ends the blocking waits on socket s (sceNetInetSocketAbort / close).
void AbortSocketWaits(int s) {
	std::vector<kernel::PspThread *> waiting;
	kernel::ForEach<kernel::PspThread>([&](kernel::PspThread *t) {
		if (t->IsWaitingFor(kernel::WAITTYPE_NET, (SceUID)s)) waiting.push_back(t);
	});
	for (auto *t : waiting) sched::Wake(t, INET_WAKE_ABORTED);
}

// A receive / accept that nothing will ever satisfy: fails at once when non-blocking, otherwise
// sleeps for the receive timeout (or until aborted, or for ever).
int WaitForNothing(int s, InetSocket *sock, u32 flags) {
	if (sock->nonblocking || (flags & INET_MSG_DONTWAIT)) return SetErrno(net::INET_EAGAIN);
	s32 r;
	if (sock->rcvTimeoutUs) r = sched::WaitUntil(kernel::WAITTYPE_NET, (SceUID)s, sched::NowUs() + sock->rcvTimeoutUs, false);
	else r = sched::Wait(kernel::WAITTYPE_NET, (SceUID)s, 0, false);
	if (r == INET_WAKE_ABORTED || r < 0) return SetErrno(net::INET_EINTR);
	return SetErrno(net::INET_EAGAIN);
}

void InetTerm() {
	for (int s = INET_MIN_SOCKET; s < INET_MAX_SOCKETS; s++) {
		if (g_inetSockets[s].used) {
			g_inetSockets[s] = InetSocket{};
			AbortSocketWaits(s);
		}
	}
	g_inetInited = false;
}

int sceNetInetInit() {
	if (g_inetInited) return (int)SCE_NET_INET_ERROR_ALREADY_INITIALIZED;
	g_inetErrno.clear();
	g_inetInited = true;
	return 0;
}
int sceNetInetTerm() {
	InetTerm();
	return 0;
}
int sceNetInetGetErrno() {
	auto it = g_inetErrno.find(sched::CurrentId());
	return it == g_inetErrno.end() ? 0 : it->second;
}
int sceNetInetGetPspError() {
	int e = sceNetInetGetErrno();
	return e ? (int)(0x80010000u | (u32)e) : 0;
}

int sceNetInetSocket(int domain, int type, int protocol) {
	if (domain != INET_AF_INET) return SetErrno(net::INET_EAFNOSUPPORT);
	int baseType = type & 0xF;
	if (baseType != INET_SOCK_STREAM && baseType != INET_SOCK_DGRAM && baseType != INET_SOCK_RAW) {
		return SetErrno(net::INET_EPROTOTYPE);
	}
	for (int s = INET_MIN_SOCKET; s < INET_MAX_SOCKETS; s++) {
		if (!g_inetSockets[s].used) {
			g_inetSockets[s] = InetSocket{};
			g_inetSockets[s].used = true;
			g_inetSockets[s].domain = domain;
			g_inetSockets[s].type = baseType;
			g_inetSockets[s].protocol = protocol;
			g_inetSockets[s].nonblocking = (type & 0x20000000) != 0;
			LOGD(LOG, "sceNetInetSocket(%d, %d, %d) -> %d", domain, type, protocol, s);
			return s;
		}
	}
	return SetErrno(net::INET_ENOBUFS);
}

int sceNetInetClose(int s) {
	if (!GetSocket(s)) return SetErrno(net::INET_EBADF);
	g_inetSockets[s] = InetSocket{};
	AbortSocketWaits(s);
	return 0;
}
int sceNetInetCloseWithRST(int s) { return sceNetInetClose(s); }

int sceNetInetSocketAbort(int s) {
	if (!GetSocket(s)) return SetErrno(net::INET_EBADF);
	AbortSocketWaits(s);
	return 0;
}

int sceNetInetShutdown(int s, int how) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (sock->type == INET_SOCK_STREAM) return SetErrno(net::INET_ENOTCONN);
	sock->shutdown = true;
	return 0;
}

int sceNetInetBind(int s, u32 addrPtr, int addrLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!mem::IsValidRange(addrPtr, 8) || addrLen < 8) return SetErrno(net::INET_EINVAL);
	if (sock->bound) return SetErrno(net::INET_EINVAL);
	mem::ReadBytes(sock->localAddr, addrPtr, (u32)std::min(addrLen, 16));
	sock->bound = true;
	return 0;
}

int sceNetInetListen(int s, int backlog) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (sock->type != INET_SOCK_STREAM) return SetErrno(net::INET_EOPNOTSUPP);
	sock->listening = true;
	return 0;
}

int sceNetInetAccept(int s, u32 addrPtr, u32 addrLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->listening) return SetErrno(net::INET_EINVAL);
	return WaitForNothing(s, sock, 0);
}

int sceNetInetConnect(int s, u32 addrPtr, int addrLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!mem::IsValidRange(addrPtr, 8) || addrLen < 8) return SetErrno(net::INET_EINVAL);
	LOGI(LOG, "sceNetInetConnect(%d): no network", s);
	return SetErrno(net::INET_ENETUNREACH);
}

int sceNetInetSend(int s, u32 buf, u32 len, u32 flags) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	return SetErrno(sock->type == INET_SOCK_STREAM ? net::INET_ENOTCONN : net::INET_EDESTADDRREQ);
}
int sceNetInetSendto(int s, u32 buf, u32 len, u32 flags, u32 toPtr, u32 toLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (sock->type == INET_SOCK_STREAM) return SetErrno(net::INET_ENOTCONN);
	return SetErrno(net::INET_ENETUNREACH);
}
int sceNetInetSendmsg(int s, u32 msg, u32 flags) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	return SetErrno(net::INET_ENETUNREACH);
}

int sceNetInetRecv(int s, u32 buf, u32 len, u32 flags) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (sock->type == INET_SOCK_STREAM) return SetErrno(net::INET_ENOTCONN);
	return WaitForNothing(s, sock, flags);
}
int sceNetInetRecvfrom(int s, u32 buf, u32 len, u32 flags, u32 fromPtr, u32 fromLenPtr) {
	return sceNetInetRecv(s, buf, len, flags);
}
int sceNetInetRecvmsg(int s, u32 msg, u32 flags) { return sceNetInetRecv(s, 0, 0, flags); }

int sceNetInetSetsockopt(int s, int level, int name, u32 valPtr, int valLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	u32 val = mem::IsValidRange(valPtr, 4) && valLen >= 4 ? GRead32(valPtr) : (mem::IsValidRange(valPtr, 1) && valLen >= 1 ? GRead8(valPtr) : 0);
	if (level == INET_SOL_SOCKET) {
		if (name == INET_SO_NBIO) sock->nonblocking = val != 0;
		if (name == INET_SO_RCVTIMEO) sock->rcvTimeoutUs = val;
	}
	sock->options[((u64)(u32)level << 32) | (u32)name] = val;
	return 0;
}

int sceNetInetGetsockopt(int s, int level, int name, u32 valPtr, u32 valLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!mem::IsValidRange(valPtr, 4) || !mem::IsValidRange(valLenPtr, 4)) return SetErrno(net::INET_EFAULT);
	u32 val = 0;
	auto it = sock->options.find(((u64)(u32)level << 32) | (u32)name);
	if (it != sock->options.end()) val = it->second;
	if (level == INET_SOL_SOCKET) {
		if (name == INET_SO_TYPE) val = (u32)sock->type;
		else if (name == INET_SO_ERROR) val = 0;
		else if (name == INET_SO_NBIO) val = sock->nonblocking ? 1 : 0;
		else if (it == sock->options.end() && name == INET_SO_SNDBUF) val = 16384;
		else if (it == sock->options.end() && name == INET_SO_RCVBUF) val = sock->type == INET_SOCK_DGRAM ? 41600 : 16384;
	}
	GWrite32(valPtr, val);
	GWrite32(valLenPtr, 4);
	return 0;
}

int sceNetInetGetsockname(int s, u32 namePtr, u32 nameLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!mem::IsValidRange(namePtr, 16) || !mem::IsValidRange(nameLenPtr, 4)) return SetErrno(net::INET_EFAULT);
	u8 addr[16] = {16, INET_AF_INET};
	if (sock->bound) {
		memcpy(addr, sock->localAddr, sizeof(addr));
		addr[0] = 16;
		addr[1] = INET_AF_INET;
		memset(addr + 4, 0, 4);  // no interface address
	}
	mem::WriteBytes(namePtr, addr, 16);
	GWrite32(nameLenPtr, 16);
	return 0;
}
int sceNetInetGetpeername(int s, u32, u32) {
	if (!GetSocket(s)) return SetErrno(net::INET_EBADF);
	return SetErrno(net::INET_ENOTCONN);
}

// Nothing is ever readable; writable only matters for connected sockets, and none are.
int sceNetInetSelect(int nfds, u32 readfds, u32 writefds, u32 exceptfds, u32 timeoutPtr) {
	for (u32 set : {readfds, writefds, exceptfds}) {
		if (set && mem::IsValidRange(set, 32)) mem::Memset(set, 0, 32);
	}
	if (timeoutPtr && mem::IsValidRange(timeoutPtr, 8)) {
		u64 us = (u64)GRead32(timeoutPtr) * 1000000 + GRead32(timeoutPtr + 4);
		if (us) sched::WaitUntil(kernel::WAITTYPE_NET, 0, sched::NowUs() + us, false);
	} else if (!timeoutPtr) {
		sched::Wait(kernel::WAITTYPE_NET, 0, 0, false);
	}
	return 0;
}
int sceNetInetPoll(u32 fdsPtr, u32 nfds, int timeoutMs) {
	for (u32 i = 0; i < nfds && i < 256; i++) {
		u32 e = fdsPtr + i * 8;
		if (!mem::IsValidRange(e, 8)) break;
		GWrite16(e + 6, 0);  // revents
	}
	if (timeoutMs > 0) sched::WaitUntil(kernel::WAITTYPE_NET, 0, sched::NowUs() + (u64)timeoutMs * 1000, false);
	else if (timeoutMs < 0) sched::Wait(kernel::WAITTYPE_NET, 0, 0, false);
	return 0;
}

// Address conversion (pure functions; these work offline).
bool ParseIPv4(const char *s, u32 *out) {
	if (!s) return false;
	u32 parts[4];
	int n = 0;
	const char *p = s;
	while (n < 4) {
		if (*p < '0' || *p > '9') return false;
		u32 v = 0;
		int digits = 0;
		while (*p >= '0' && *p <= '9') {
			v = v * 10 + (u32)(*p++ - '0');
			if (++digits > 3 || v > 255) return false;
		}
		parts[n++] = v;
		if (n < 4) {
			if (*p != '.') return false;
			p++;
		}
	}
	if (*p) return false;
	*out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);  // network byte order
	return true;
}

u32 sceNetInetInetAddr(const char *s) {
	u32 a;
	return ParseIPv4(s, &a) ? a : 0xFFFFFFFF;
}
int sceNetInetInetAton(const char *s, u32 inAddrPtr) {
	u32 a;
	if (!ParseIPv4(s, &a)) return 0;
	if (mem::IsValidRange(inAddrPtr, 4)) GWrite32(inAddrPtr, a);
	return 1;
}
int sceNetInetInetPton(int af, const char *s, u32 dstPtr) {
	if (af != INET_AF_INET) return SetErrno(net::INET_EAFNOSUPPORT);
	u32 a;
	if (!ParseIPv4(s, &a)) return 0;
	if (mem::IsValidRange(dstPtr, 4)) GWrite32(dstPtr, a);
	return 1;
}
u32 sceNetInetInetNtop(int af, u32 srcPtr, u32 dstPtr, u32 size) {
	if (af != INET_AF_INET) {
		SetErrno(net::INET_EAFNOSUPPORT);
		return 0;
	}
	if (!mem::IsValidRange(srcPtr, 4)) return 0;
	u32 a = GRead32(srcPtr);
	char buf[16];
	int len = snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
	if (size < (u32)len + 1 || !mem::IsValidRange(dstPtr, (u32)len + 1)) {
		SetErrno(net::INET_ENOSPC);
		return 0;
	}
	mem::WriteBytes(dstPtr, buf, (u32)len + 1);
	return dstPtr;
}

// ================================================================ sceNetResolver
// Created fine; with no DNS server only dotted addresses resolve.

struct Resolver {
	u32 bufferAddr = 0, bufferLen = 0;
	bool running = false;
};
std::map<int, Resolver> g_resolvers;
int g_nextResolverId = 1;
// Starts out initialized (Outrun 2006 creates resolvers without calling sceNetResolverInit).
bool g_resolverInited = true;

int sceNetResolverInit() {
	g_resolverInited = true;
	return 0;
}
int sceNetResolverTerm() {
	g_resolvers.clear();
	g_resolverInited = false;
	return 0;
}
int sceNetResolverCreate(u32 idPtr, u32 bufferPtr, int bufferLen) {
	if (!mem::IsValidRange(idPtr, 4)) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	if (mem::IsValidRange(bufferPtr, 4) && bufferLen < 1) return (int)SCE_NET_RESOLVER_ERROR_INVALID_BUFLEN;
	if (g_resolvers.size() >= 32) return (int)SCE_NET_RESOLVER_ERROR_ID_MAX;
	int id = g_nextResolverId++;
	g_resolvers[id] = Resolver{bufferPtr, (u32)bufferLen, false};
	GWrite32(idPtr, (u32)id);
	return 0;
}
int sceNetResolverDelete(int id) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	if (!g_resolvers.erase(id)) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	return 0;
}
int sceNetResolverStop(int id) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	auto it = g_resolvers.find(id);
	if (it == g_resolvers.end()) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	if (!it->second.running) return (int)SCE_NET_RESOLVER_ERROR_ALREADY_STOPPED;
	it->second.running = false;
	return 0;
}
int sceNetResolverStartNtoA(int id, u32 hostnamePtr, u32 inAddrPtr, int timeout, int retry) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	if (!g_resolvers.count(id)) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	if (!mem::IsValidRange(inAddrPtr, 4)) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	const char *host = mem::GetCString(hostnamePtr);
	if (!host) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	u32 a;
	if (ParseIPv4(host, &a)) {
		GWrite32(inAddrPtr, a);
		return 0;
	}
	LOGI(LOG, "sceNetResolverStartNtoA(%s): no DNS server", host);
	return (int)net::SCE_NET_RESOLVER_NO_DNS_SERVER;
}
int sceNetResolverStartNtoAAsync(int id, u32 hostnamePtr, u32 inAddrPtr, int timeout, int retry) {
	return sceNetResolverStartNtoA(id, hostnamePtr, inAddrPtr, timeout, retry);
}
int sceNetResolverStartAtoN(int id, u32 inAddr, u32 hostnamePtr, int hostnameLen, int timeout, int retry) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	if (!g_resolvers.count(id)) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	if (!mem::IsValidRange(hostnamePtr, 1) || hostnameLen < 1) return (int)SCE_NET_RESOLVER_ERROR_INVALID_BUFLEN;
	return (int)net::SCE_NET_RESOLVER_NO_DNS_SERVER;
}
int sceNetResolverStartAtoNAsync(int id, u32 inAddr, u32 hostnamePtr, int hostnameLen, int timeout, int retry) {
	return sceNetResolverStartAtoN(id, inAddr, hostnamePtr, hostnameLen, timeout, retry);
}
// The async requests above complete at once: nothing is ever pending.
int sceNetResolverPollAsync(int id, u32 resultPtr) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	if (!g_resolvers.count(id)) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	return 0;
}
int sceNetResolverWaitAsync(int id, u32 resultPtr) { return sceNetResolverPollAsync(id, resultPtr); }

}  // namespace

namespace net {
bool NetInited() { return g_netInited; }
}  // namespace net

void Register_NetLibraries() {
	static const hle::HleFunction sceNet[] = {
		HLEFN(0x39AF39A6, sceNetInit),
		HLEFN(0x281928A9, sceNetTerm),
		HLEFN(0x89360950, sceNetEtherNtostr),
		HLEFN(0xD27961C9, sceNetEtherStrton),
		HLEFN(0x0BF0A3AE, sceNetGetLocalEtherAddr),
		HLEFN(0x50647530, sceNetFreeThreadinfo),
		HLEFN(0xCC393E48, sceNetGetMallocStat),
		HLEFN(0xAD6844C6, sceNetThreadAbort),
	};
	hle::RegisterModule("sceNet", sceNet);

	static const hle::HleFunction sceNetLib[] = {
		HLEFN(0x1858883D, sceNetRand),
		HLEFN(0x2A73ADDC, sceNetStrtoul),
		HLEFN(0x4753D878, sceNetMemmove),
		HLEFN(0x80C9F02A, sceNetStrcpy),
		HLEFN(0x94DCA9F0, sceNetStrncmp),
		HLEFN(0x9CFBC7E3, sceNetStrcasecmp),
		HLEFN(0xA0F16ABD, sceNetStrcmp),
		HLEFN(0xB5CE388A, sceNetStrncpy),
		HLEFN(0xBCBE14CF, sceNetStrchr),
		HLESTUB(0xCF705E46, "sceNetSprintf"),
		HLEFN(0xD8722983, sceNetStrlen),
		HLEFN(0xE0A81C7C, sceNetMemcmp),
	};
	hle::RegisterModule("sceNet_lib", sceNetLib);

	static const hle::HleFunction sceNetIfhandle[] = {
		HLEFN(0xC80181A2, sceNetGetDropRate),
		HLEFN(0xFD8585E1, sceNetSetDropRate),
	};
	hle::RegisterModule("sceNetIfhandle", sceNetIfhandle);

	static const hle::HleFunction sceNetUpnp[] = {
		HLEFN(0x27045362, sceNetUpnpGetNatInfo),
		HLEFN(0x3432B2E5, sceNetUpnpStart),
		HLEFN(0x3E32ED9E, sceNetUpnpStop),
		HLEFN(0x540491EF, sceNetUpnpTerm),
		HLEFN(0xE24220B5, sceNetUpnpInit),
	};
	hle::RegisterModule("sceNetUpnp", sceNetUpnp);

	static const hle::HleFunction sceWlanDrv[] = {
		HLEFN(0xD7763699, sceWlanGetSwitchState),
		HLEFN(0x0C622081, sceWlanGetEtherAddr),
		HLEFN(0x93440B11, sceWlanDevIsPowerOn),
	};
	hle::RegisterModule("sceWlanDrv", sceWlanDrv);

	static const hle::HleFunction sceWlanDrvLib[] = {
		HLEFN(0x05FE320C, sceWlanDevGetStateGPIO),
		HLEFN(0x19E51F54, sceWlanDevGetMacAddr),
		HLEFN(0x482CAE9A, sceWlanDevAttach),
		HLEFN(0xC9A8CAB7, sceWlanDevDetach),
		HLEFN(0x5E7C8D94, sceWlanDevIsGameMode),
		HLEFN(0x7FF54BD2, sceWlanOk),  // sceWlanDevSetGPIO
		HLEFN(0x9658C9F7, sceWlanOk),  // sceWlanGPRegisterCallback
		HLEFN(0x4C7F62E0, sceWlanOk),  // sceWlanGPUnRegisterCallback
		HLEFN(0x749B813A, sceWlanOk),  // sceWlanSetHostDiscover
		HLEFN(0xFE8A0B46, sceWlanOk),  // sceWlanSetWakeUp
		HLESTUB(0x5ED4049A, "sceWlanGPPrevEstablishActive"),
		HLESTUB(0xA447103A, "sceWlanGPRecv"),
		HLESTUB(0xB4D7CB74, "sceWlanGPSend"),
	};
	hle::RegisterModule("sceWlanDrv_lib", sceWlanDrvLib);

	static const hle::HleFunction sceNetApctl[] = {
		HLEFN(0xCFB957C6, sceNetApctlConnect),
		HLEFN(0x24FE91A1, sceNetApctlDisconnect),
		HLEFN(0x5DEAC81B, sceNetApctlGetState),
		HLEFN(0x8ABADD51, sceNetApctlAddHandler),
		HLEFN(0xE2F91F9B, sceNetApctlInit),
		HLEFN(0x5963991B, sceNetApctlDelHandler),
		HLEFN(0xB3EDD0EC, sceNetApctlTerm),
		HLEFN(0x2BEFDF23, sceNetApctlGetInfo),
		HLEFN(0xA3E77E13, sceNetApctlScanUser),  // sceNetApctlScanSSID2
		HLEFN(0xE9B2E5E6, sceNetApctlScanUser),
		HLEFN(0xF25A5006, sceNetApctlGetBSSDescIDList2),
		HLEFN(0x2935C45B, sceNetApctlGetBSSDescEntryUser),  // sceNetApctlGetBSSDescEntry2
		HLEFN(0x04776994, sceNetApctlGetBSSDescEntryUser),
		HLEFN(0x6BDDCB8C, sceNetApctlGetBSSDescIDListUser),
		HLEFN(0x7CFAB990, sceNetApctlAddInternalHandler),
		HLEFN(0xE11BAFAB, sceNetApctlDelInternalHandler),
		HLEFN(0xA7BB73DF, sceNetApctlAddHandler),  // sceNetApctlAddInternal03Handler
		HLEFN(0x6F5D2981, sceNetApctlDelHandler),  // sceNetApctlDelInternal03Handler
		HLEFN(0x69745F0A, sceNetApctl_lib2_69745F0A),
		HLEFN(0x4C19731F, sceNetApctlGetInfo),  // sceNetApctl_lib2_4C19731F
		HLEFN(0xB3CF6849, sceNetApctlScanUser),  // sceNetApctlScan
		HLEFN(0x0C7FFA5C, sceNetApctlGetBSSDescIDListUser),  // sceNetApctlGetBSSDescIDList
		HLEFN(0x96BEB231, sceNetApctlGetBSSDescEntryUser),  // sceNetApctlGetBSSDescEntry
		HLEFN(0xC20A144C, sceNetApctlConnectWithMac),  // sceNetApctl_lib2_C20A144C
	};
	hle::RegisterModule("sceNetApctl", sceNetApctl);

	static const hle::HleFunction sceNetInet[] = {
		HLEFN(0x17943399, sceNetInetInit),
		HLEFN(0x4CFE4E56, sceNetInetShutdown),
		HLEFN(0xA9ED66B9, sceNetInetTerm),
		HLEFN(0x8B7B220F, sceNetInetSocket),
		HLEFN(0x2FE71FE7, sceNetInetSetsockopt),
		HLEFN(0x4A114C7C, sceNetInetGetsockopt),
		HLEFN(0x410B34AA, sceNetInetConnect),
		HLEFN(0x805502DD, sceNetInetCloseWithRST),
		HLEFN(0xD10A1A7A, sceNetInetListen),
		HLEFN(0xDB094E1B, sceNetInetAccept),
		HLEFN(0xFAABB1DD, sceNetInetPoll),
		HLEFN(0x5BE8D595, sceNetInetSelect),
		HLEFN(0x8D7284EA, sceNetInetClose),
		HLEFN(0xCDA85C99, sceNetInetRecv),
		HLEFN(0xC91142E4, sceNetInetRecvfrom),
		HLEFN(0xEECE61D2, sceNetInetRecvmsg),
		HLEFN(0x7AA671BC, sceNetInetSend),
		HLEFN(0x05038FC7, sceNetInetSendto),
		HLEFN(0x774E36F4, sceNetInetSendmsg),
		HLEFN(0xFBABE411, sceNetInetGetErrno),
		HLEFN(0x1A33F9AE, sceNetInetBind),
		HLEFN(0xB75D5B0A, sceNetInetInetAddr),
		HLEFN(0x1BDF5D13, sceNetInetInetAton),
		HLEFN(0xD0792666, sceNetInetInetNtop),
		HLEFN(0xE30B8C19, sceNetInetInetPton),
		HLEFN(0x8CA3A97E, sceNetInetGetPspError),
		HLEFN(0xE247B6D6, sceNetInetGetpeername),
		HLEFN(0x162E6FD5, sceNetInetGetsockname),
		HLEFN(0x80A21ABD, sceNetInetSocketAbort),
		HLESTUB(0x39B0C7D3, "sceNetInetGetUdpcbstat"),
		HLESTUB(0xB3888AD4, "sceNetInetGetTcpcbstat"),
		HLESTUB(0x2D5868C0, "sceNetInetDelArp"),
		HLESTUB(0xCCC18C45, "sceNetInetAddArp"),
	};
	hle::RegisterModule("sceNetInet", sceNetInet);

	static const hle::HleFunction sceNetResolver[] = {
		HLEFN(0x224C5F44, sceNetResolverStartNtoA),
		HLEFN(0x244172AF, sceNetResolverCreate),
		HLEFN(0x94523E09, sceNetResolverDelete),
		HLEFN(0xF3370E61, sceNetResolverInit),
		HLEFN(0x808F6063, sceNetResolverStop),
		HLEFN(0x6138194A, sceNetResolverTerm),
		HLEFN(0x629E2FB7, sceNetResolverStartAtoN),
		HLEFN(0x14C17EF9, sceNetResolverStartNtoAAsync),
		HLEFN(0xAAC09184, sceNetResolverStartAtoNAsync),
		HLEFN(0x12748EB9, sceNetResolverWaitAsync),
		HLEFN(0x4EE99358, sceNetResolverPollAsync),
	};
	hle::RegisterModule("sceNetResolver", sceNetResolver);

	net::RegisterParseLibraries();
	net::RegisterHttpLibraries();
	net::RegisterAdhocLibraries();
	net::RegisterNpLibraries();
}
