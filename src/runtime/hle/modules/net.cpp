// SPDX-License-Identifier: GPL-2.0-or-later
//
// Network libraries: sceNet (pspnet core), sceNet_lib, sceNetInet, sceNetApctl, sceNetResolver,
// sceWlanDrv / sceWlanDrv_lib, sceNetUpnp, sceNetIfhandle. The other network libraries live in
// net_parse.cpp, net_http.cpp, net_adhoc.cpp and net_np.cpp; the host glue in net_host.cpp.
//
// With networking on (the default), the WLAN switch reads on and network configuration 1 "PSPC"
// connects to a virtual access point: the host's own network. sceNetApctlConnect goes through the
// usual JOINING -> GETTING_IP -> GOT_IP states (handlers are called on the SceNetApctl thread) and
// GetInfo reports the host's IP, netmask, gateway and DNS. BSD sockets (sceNetInet) are Winsock
// sockets, always non-blocking on the host; a blocking guest call sleeps its guest thread until the
// socket is ready. Names resolve through the host's resolver once the PSP has a DNS server (GOT_IP).
//
// With network=0 everything behaves as on a PSP whose WLAN switch is off: connecting fails with
// WLAN_SWITCH_OFF, names don't resolve and sockets never reach anyone.
//
// Function set, argument checks, structures and the option / errno conversions ported from PPSSPP
// (Core/HLE/sceNet.cpp, sceNet_lib.cpp, sceNetInet.cpp, sceNetResolver.cpp, NetInetConstants.cpp),
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. sceNetEtherNtostr/Strton follow pspnet.prx
// as measured on hardware (pspautotests net/primary/ether).
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
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
// No port mapping is ever set up on the host's router; the library itself comes up fine.

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
int sceNetStrcasecmp(const char *a, const char *b) {
	if (!a || !b) return 0;
	while (*a && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) { ++a; ++b; }
	return std::tolower((unsigned char)*a) - std::tolower((unsigned char)*b);
}
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
// The unnamed sceNet_lib exports, identified from pspnet.prx's code (the XMB's update_plugin and
// libdnas use them): memcpy, memset, strcat, tolower and the ctype table lookup.
u32 sceNetMemcpy(u32 dst, u32 src, u32 n) {
	if (!dst) return 0;
	if (n && mem::IsValidRange(dst, n) && mem::IsValidRange(src, n)) memmove(GPtr(dst), GPtr(src), n);
	return dst;
}
u32 sceNetMemset(u32 dst, int value, u32 n) {
	if (!dst) return 0;
	if (n && mem::IsValidRange(dst, n)) mem::Memset(dst, (u8)value, n);
	return dst;
}
u32 sceNetStrcat(u32 dst, u32 src) {
	const char *d = mem::GetCString(dst), *s = mem::GetCString(src);
	if (!d || !s) return dst;
	u32 dl = (u32)strlen(d), sl = (u32)strlen(s);
	if (dst + dl == src + sl) return dst;  // the same string: pspnet leaves it
	std::string tail(s, sl);
	if (mem::IsValidRange(dst + dl, sl + 1)) mem::WriteBytes(dst + dl, tail.c_str(), sl + 1);
	return dst;
}
// newlib's ctype flags: 1 upper, 2 lower, 4 digit, 8 space, 0x10 punctuation, 0x20 control,
// 0x40 hex digit, 0x80 blank.
int NetCtype(int c) {
	if (c < 0 || c > 127) return 0;
	int f = 0;
	if (c >= 'A' && c <= 'Z') f |= 0x01;
	if (c >= 'a' && c <= 'z') f |= 0x02;
	if (c >= '0' && c <= '9') f |= 0x04;
	if ((c >= 9 && c <= 13) || c == ' ') f |= 0x08;
	if (c > 32 && c < 127 && !(f & 0x07)) f |= 0x10;
	if (c < 32 || c == 127) f |= 0x20;
	if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= 0x40;
	if (c == ' ') f |= 0x80;
	return f;
}
int sceNetCtype(int c) { return NetCtype((int)(s8)c); }
int sceNetTolower(int c) {
	int ch = (int)(s8)c;
	return (NetCtype(ch) & 0x01) ? ch + 0x20 : ch;
}

// ================================================================ sceWlanDrv
// The switch follows pspc.ini's network setting.

u32 sceWlanGetSwitchState() { return net::Enabled() ? 1 : 0; }
u32 sceWlanDevIsPowerOn() { return net::Enabled() ? 1 : 0; }
int sceWlanGetEtherAddr(u32 addrAddr) {
	if (!mem::IsValidRange(addrAddr, 6)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	mem::WriteBytes(addrAddr, net::kLocalMac, 6);
	return 0;
}
int sceWlanDevGetMacAddr(u32 addrAddr) { return sceWlanGetEtherAddr(addrAddr); }
int sceWlanDevIsGameMode() { return 0; }
int sceWlanDevGetStateGPIO() { return 0; }
int sceWlanDevAttach() { return net::Enabled() ? 0 : (int)ERROR_NET_WLAN_POWER_OFF; }
int sceWlanDevDetach() { return 0; }
int sceWlanOk() { return 0; }

// ================================================================ sceNetApctl
// The virtual access point. State changes happen at scheduled times (an association takes a moment
// on hardware); each one is reported to the handlers on the SceNetApctl thread, which exists while
// the library is initialized. The Netconf dialog connects without the library: then nobody is told.

enum ApctlEvent : u32 {
	APCTL_EVENT_CONNECT_REQUEST = 0,
	APCTL_EVENT_SCAN_REQUEST = 1,
	APCTL_EVENT_SCAN_COMPLETE = 2,
	APCTL_EVENT_ESTABLISHED = 3,
	APCTL_EVENT_GET_IP = 4,
	APCTL_EVENT_DISCONNECT_REQUEST = 5,
	APCTL_EVENT_ERROR = 6,
	APCTL_EVENT_INFO = 7,
	APCTL_EVENT_EAP_AUTH = 8,
	APCTL_EVENT_KEY_EXCHANGE = 9,
	APCTL_EVENT_RECONNECT = 10,
	APCTL_EVENT_SCAN_STOP = 11,
};

constexpr u32 APCTL_PROFILENAME_MAXLEN = 64;
constexpr u32 APCTL_SSID_MAXLEN = 32;
constexpr u32 APCTL_IPADDR_MAXLEN = 16;
constexpr u32 APCTL_URL_MAXLEN = 128;
constexpr int MAX_APCTL_HANDLERS = 32;
// Each step of an association (request -> associated -> DHCP lease) takes this long.
constexpr u64 APCTL_STEP_US = 200000;
constexpr const char *APCTL_SSID = "PSPC";
// The virtual access point's BSSID (unicast, globally administered).
constexpr u8 APCTL_BSSID[6] = {0x00, 0x16, 0xFE, 0x41, 0x50, 0x01};
constexpr u8 APCTL_CHANNEL = 6;
constexpr u8 APCTL_STRENGTH = 100;

struct ApctlHandler {
	u32 entry, arg;
};

bool g_apctlInited = false;
int g_apctlState = net::APCTL_STATE_DISCONNECTED;
int g_apctlConfId = 1;
std::map<int, ApctlHandler> g_apctlHandlers;
net::ApctlLease g_apctlLease;
u64 g_apctlStepEvent = 0;
net::HandlerThread g_apctlThread;

void ApctlCancelStep() {
	if (g_apctlStepEvent) sched::CancelEvent(g_apctlStepEvent);
	g_apctlStepEvent = 0;
}

// Moves to `state` and tells the handlers (oldState, newState, event, error, arg).
void ApctlChange(int state, u32 event, u32 error = 0) {
	int old = g_apctlState;
	g_apctlState = state;
	LOGI(LOG, "sceNetApctl: state %d -> %d (event %u)", old, state, event);
	if (!g_apctlThread.Running()) return;
	for (auto &h : g_apctlHandlers) {
		ApctlHandler handler = h.second;
		g_apctlThread.Post([handler, old, state, event, error] {
			net::HandlerThread::CallHandler(handler.entry, {(u32)old, (u32)state, event, error, handler.arg});
		});
	}
}

void ApctlScheduleStep(std::function<void()> fn) {
	ApctlCancelStep();
	g_apctlStepEvent = sched::ScheduleEvent(sched::NowUs() + APCTL_STEP_US, [fn] {
		g_apctlStepEvent = 0;
		fn();
	});
}

void ApctlGotIp() {
	hostnet::HostConfig cfg = hostnet::QueryHostConfig();
	g_apctlLease.ip = cfg.ip;
	g_apctlLease.netmask = cfg.netmask;
	g_apctlLease.gateway = cfg.gateway;
	g_apctlLease.dns1 = cfg.dns[0];
	g_apctlLease.dns2 = cfg.dns[1];
	// A revival DNS server answers for the game's servers (net_infra.cpp).
	if (const u32 revival = net::InfraDnsServer()) g_apctlLease.dns1 = revival;
	LOGI(LOG, "sceNetApctl: connected to \"%s\" through %s: IP %s, netmask %s, gateway %s, DNS %s", APCTL_SSID,
	     cfg.adapter.c_str(), hostnet::Ipv4ToString(cfg.ip).c_str(), hostnet::Ipv4ToString(cfg.netmask).c_str(),
	     hostnet::Ipv4ToString(cfg.gateway).c_str(), hostnet::Ipv4ToString(cfg.dns[0]).c_str());
	ApctlChange(net::APCTL_STATE_GOT_IP, APCTL_EVENT_GET_IP);
}

void ApctlTerm() {
	ApctlCancelStep();
	g_apctlThread.Stop();
	g_apctlHandlers.clear();
	g_apctlInited = false;
	g_apctlState = net::APCTL_STATE_DISCONNECTED;
	g_apctlLease = net::ApctlLease{};
}

int sceNetApctlInit(int stackSize, int initPriority) {
	if (g_apctlInited) return (int)SCE_NET_APCTL_ERROR_ALREADY_INITIALIZED;
	g_apctlState = net::APCTL_STATE_DISCONNECTED;
	g_apctlLease = net::ApctlLease{};
	if (net::Enabled()) g_apctlThread.Start("SceNetApctl", initPriority, stackSize);
	g_apctlInited = true;
	LOGI(LOG, "sceNetApctlInit(%d, %d)", stackSize, initPriority);
	return 0;
}

int sceNetApctlTerm() {
	ApctlTerm();
	return 0;
}

int sceNetApctlConnect(int confId) {
	if (!net::Enabled()) {
		LOGI(LOG, "sceNetApctlConnect(%d): WLAN switch is off", confId);
		return (int)SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF;
	}
	if (g_apctlState != net::APCTL_STATE_DISCONNECTED) return (int)SCE_NET_APCTL_ERROR_NOT_DISCONNECTED;
	g_apctlConfId = confId <= 0 ? 1 : confId;
	LOGI(LOG, "sceNetApctlConnect(%d)", confId);
	ApctlChange(net::APCTL_STATE_JOINING, APCTL_EVENT_CONNECT_REQUEST);
	ApctlScheduleStep([] {
		ApctlChange(net::APCTL_STATE_GETTING_IP, APCTL_EVENT_ESTABLISHED);
		ApctlScheduleStep([] { ApctlGotIp(); });
	});
	return 0;
}
int sceNetApctlConnectWithMac(int confId, u32) { return sceNetApctlConnect(confId); }

int sceNetApctlDisconnect() {
	ApctlCancelStep();
	g_apctlLease = net::ApctlLease{};
	// Handlers hear about it even when nothing was connected (Phantasy Star Portable 2 waits for it).
	ApctlChange(net::APCTL_STATE_DISCONNECTED, APCTL_EVENT_DISCONNECT_REQUEST);
	return 0;
}

int sceNetApctlGetState(u32 stateAddr) {
	if (!mem::IsValidRange(stateAddr, 4)) return (int)ERROR_NET_WLAN_INVALID_ARG;
	GWrite32(stateAddr, (u32)g_apctlState);
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

std::string ProfileName() {
	char name[32];
	snprintf(name, sizeof(name), "PSPC %d", g_apctlConfId);
	return name;
}

// Before the association the info is what an idle interface reports (dotted addresses of 0.0.0.0 -
// Driver 76 wants dots rather than an empty buffer).
int sceNetApctlGetInfo(int code, u32 infoAddr) {
	auto writeStr = [&](const std::string &s, u32 size) {
		if (!mem::IsValidRange(infoAddr, size)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		mem::Memset(infoAddr, 0, size);
		mem::WriteBytes(infoAddr, s.data(), (u32)std::min<size_t>(s.size(), size - 1));
		return 0;
	};
	auto writeInt = [&](u32 v, u32 size) {
		if (!mem::IsValidRange(infoAddr, size)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		if (size == 1) GWrite8(infoAddr, (u8)v);
		else if (size == 2) GWrite16(infoAddr, (u16)v);
		else GWrite32(infoAddr, v);
		return 0;
	};
	bool associated = g_apctlState >= net::APCTL_STATE_GETTING_IP && g_apctlState <= net::APCTL_STATE_GOT_IP;
	bool joining = g_apctlState != net::APCTL_STATE_DISCONNECTED && g_apctlState != net::APCTL_STATE_SCANNING;
	bool gotIp = g_apctlState == net::APCTL_STATE_GOT_IP;
	auto ip = [&](u32 a) { return writeStr(gotIp ? hostnet::Ipv4ToString(a) : "0.0.0.0", APCTL_IPADDR_MAXLEN); };
	switch (code) {
	case 0: return writeStr(joining ? ProfileName() : "", APCTL_PROFILENAME_MAXLEN);  // PROFILE_NAME
	case 1:                                                                           // BSSID
		if (!mem::IsValidRange(infoAddr, 6)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		if (associated) mem::WriteBytes(infoAddr, APCTL_BSSID, 6);
		else mem::Memset(infoAddr, 0, 6);
		return 0;
	case 2: return writeStr(joining ? APCTL_SSID : "", APCTL_SSID_MAXLEN);                 // SSID
	case 3: return writeInt(joining ? (u32)strlen(APCTL_SSID) : 0, 4);                     // SSID_LENGTH
	case 4: return writeInt(0, 4);                                                         // SECURITY_TYPE (none)
	case 5: return writeInt(associated ? APCTL_STRENGTH : 0, 1);                           // STRENGTH
	case 6: return writeInt(associated ? APCTL_CHANNEL : 0, 1);                            // CHANNEL
	case 7: return writeInt(0, 1);                                                         // POWER_SAVE
	case 8: return ip(g_apctlLease.ip);                                                    // IP
	case 9: return ip(g_apctlLease.netmask);                                               // SUBNETMASK
	case 10: return ip(g_apctlLease.gateway);                                              // GATEWAY
	case 11: return ip(g_apctlLease.dns1);                                                 // PRIMDNS
	case 12: return ip(g_apctlLease.dns2);                                                 // SECDNS
	case 13: return writeInt(0, 4);                                                        // USE_PROXY
	case 14: return writeStr("", APCTL_URL_MAXLEN);                                        // PROXY_URL
	case 15: return writeInt(0, 2);                                                        // PROXY_PORT
	case 16: return writeInt(0, 4);                                                        // 8021_EAP_TYPE
	case 17: return writeInt(0, 4);                                                        // START_BROWSER
	case 18: return writeInt(0, 4);                                                        // WIFISP
	default: return (int)SCE_NET_APCTL_ERROR_INVALID_CODE;
	}
}

// A scan finds the virtual access point.
int sceNetApctlScanUser() {
	if (!net::Enabled()) return (int)SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF;
	if (g_apctlState != net::APCTL_STATE_DISCONNECTED) return (int)SCE_NET_APCTL_ERROR_NOT_DISCONNECTED;
	ApctlChange(net::APCTL_STATE_SCANNING, APCTL_EVENT_SCAN_REQUEST);
	ApctlScheduleStep([] { ApctlChange(net::APCTL_STATE_DISCONNECTED, APCTL_EVENT_SCAN_COMPLETE); });
	return 0;
}

// The scan result list: 8-byte entries (next pointer, entry ID). One entry while networking is on.
int sceNetApctlGetBSSDescIDListUser(u32 sizeAddr, u32 bufAddr) {
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)ERROR_NET_WLAN_INVALID_ARG;
	constexpr u32 kEntry = 8;
	u32 entries = net::Enabled() ? 1 : 0;
	u32 room = GRead32(sizeAddr);
	GWrite32(sizeAddr, entries * kEntry);
	if (bufAddr && mem::IsValidAddress(bufAddr)) {
		for (u32 i = 0; i < entries && (i + 1) * kEntry <= room; i++) {
			u32 e = bufAddr + i * kEntry;
			if (!mem::IsValidRange(e, kEntry)) break;
			GWrite32(e, i + 1 < entries ? e + kEntry : 0);
			GWrite32(e + 4, i);
		}
	}
	return 0;
}
int sceNetApctlGetBSSDescIDList2(u32 sizeAddr, u32 bufAddr, u32, u32) { return sceNetApctlGetBSSDescIDListUser(sizeAddr, bufAddr); }

int sceNetApctlGetBSSDescEntryUser(int entryId, int infoId, u32 resultAddr) {
	if (!mem::IsValidAddress(resultAddr)) return (int)ERROR_NET_WLAN_INVALID_ARG;
	if (!net::Enabled() || entryId != 0) return (int)SCE_NET_APCTL_ERROR_INVALID_ID;
	switch (infoId) {
	case 0:  // BSSID
		if (!mem::IsValidRange(resultAddr, 6)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		mem::WriteBytes(resultAddr, APCTL_BSSID, 6);
		return 0;
	case 1:  // SSID (32 bytes)
		if (!mem::IsValidRange(resultAddr, APCTL_SSID_MAXLEN)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		mem::Memset(resultAddr, 0, APCTL_SSID_MAXLEN);
		mem::WriteBytes(resultAddr, APCTL_SSID, (u32)strlen(APCTL_SSID));
		return 0;
	case 2:  // SSID length
		if (!mem::IsValidRange(resultAddr, 4)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		GWrite32(resultAddr, (u32)strlen(APCTL_SSID));
		return 0;
	case 3:  // channel
		GWrite8(resultAddr, APCTL_CHANNEL);
		return 0;
	case 4:  // signal strength
		GWrite8(resultAddr, APCTL_STRENGTH);
		return 0;
	case 5:  // security
		if (!mem::IsValidRange(resultAddr, 4)) return (int)ERROR_NET_WLAN_INVALID_ARG;
		GWrite32(resultAddr, 0);
		return 0;
	default: return (int)SCE_NET_APCTL_ERROR_INVALID_CODE;
	}
}
int sceNetApctlAddInternalHandler(u32 handler, u32 arg) { return sceNetApctlAddHandler(handler, arg); }
int sceNetApctlDelInternalHandler(u32 id) { return sceNetApctlDelHandler(id); }
int sceNetApctl_lib2_69745F0A(int) { return 0; }

// ================================================================ sceNetInet

constexpr int INET_MIN_SOCKET = 1;
constexpr int INET_MAX_SOCKETS = 64;
constexpr int INET_AF_INET = 2;
constexpr int INET_SOCK_STREAM = 1, INET_SOCK_DGRAM = 2, INET_SOCK_RAW = 3;
constexpr int INET_IPPROTO_ICMP = 1;
constexpr int INET_SOCK_NONBLOCK = 0x20000000;
constexpr u32 INET_MSG_DONTWAIT = 0x80;
constexpr int INET_SOMAXCONN = 128;
// NetBSD poll(2) events.
constexpr u16 INET_POLLIN = 0x0001, INET_POLLPRI = 0x0002, INET_POLLOUT = 0x0004, INET_POLLERR = 0x0008,
              INET_POLLHUP = 0x0010, INET_POLLNVAL = 0x0020, INET_POLLRDNORM = 0x0040, INET_POLLRDBAND = 0x0080,
              INET_POLLWRBAND = 0x0100;
// The PSP's socket buffer sizes before a game sets them.
constexpr u32 INET_DEFAULT_SNDBUF = 16384, INET_DEFAULT_RCVBUF_STREAM = 16384, INET_DEFAULT_RCVBUF_DGRAM = 41600;

struct InetSocket {
	bool used = false;
	hostnet::Socket host;  // invalid when networking is off: the socket then reaches nobody
	int domain = 0, type = 0, protocol = 0;
	bool nonblocking = false;
	bool bound = false, listening = false, shutdown = false;
	bool connecting = false;  // a non-blocking connect is in progress
	int savedError = 0;       // PSP errno for SO_ERROR (a failed connect that select/poll noticed)
	u32 rcvTimeoutUs = 0, sndTimeoutUs = 0;
	u64 abortKey = 0;
	std::map<u64, u32> options;  // (level << 32 | name) -> value, for getsockopt
	u8 localAddr[16] = {};       // offline: the sockaddr given to bind
	bool wildcard = false;       // bound to 0.0.0.0 on the host for a guest wildcard bind (getsockname shows the AP's address)
	// Datagrams pspc answered itself (DNS for redirected names, net_infra.cpp), received before
	// anything from the host.
	std::deque<std::pair<std::vector<u8>, sockaddr_in>> injected;
	// A raw ICMP socket the host would not open (it needs CAP_NET_RAW on Linux, an administrator on Windows): `host` is an
	// ordinary UDP socket then, and the echo requests sent are answered here (AnswerIcmpEcho).
	bool icmpEcho = false;
};

bool g_inetInited = false;
InetSocket g_inetSockets[INET_MAX_SOCKETS];
std::map<SceUID, int> g_inetErrno;  // per thread
u64 g_nextInetAbortKey = 1;
constexpr s32 INET_WAKE_ABORTED = 1;

int SetErrno(int err) {
	g_inetErrno[sched::CurrentId()] = err;
	return -1;
}

int SetErrnoFromWsa(int wsaError, bool forConnect = false) { return SetErrno(hostnet::PspErrnoFromWsa(wsaError, forConnect)); }

InetSocket *GetSocket(int s) {
	if (s < INET_MIN_SOCKET || s >= INET_MAX_SOCKETS || !g_inetSockets[s].used) return nullptr;
	return &g_inetSockets[s];
}

u64 OptKey(int level, int name) { return ((u64)(u32)level << 32) | (u32)name; }

// --- offline sockets (network=0): they exist, but there is no interface to send through ---

// Ends the blocking waits on an offline socket (sceNetInetSocketAbort / close).
void AbortOfflineWaits(int s) {
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

// --- host sockets ---

void ReleaseSocket(int s, bool reset) {
	InetSocket &sock = g_inetSockets[s];
	if (sock.host.Valid()) {
		net::AbortWaits(sock.abortKey);
		hostnet::CloseSocket(sock.host, reset);
		net::HostSocketClosed();
	} else {
		AbortOfflineWaits(s);
	}
	sock = InetSocket{};
}

void InetTerm() {
	for (int s = INET_MIN_SOCKET; s < INET_MAX_SOCKETS; s++)
		if (g_inetSockets[s].used) ReleaseSocket(s, false);
	g_inetInited = false;
}

u64 Deadline(u32 timeoutUs) { return timeoutUs ? sched::NowUs() + timeoutUs : 0; }

// Sleeps until the socket's event fires, the deadline passes or the socket is aborted.
net::WaitResult WaitSocket(InetSocket *sock, u64 deadline) {
	return net::WaitHostEvents(&sock->host.ev, 1, deadline, sock->abortKey);
}

bool DeadlinePassed(u64 deadline) { return deadline && sched::NowUs() >= deadline; }

// Reads a guest struct sockaddr_in. PSP software sets sin_family (byte 1) to AF_INET; a Linux-style
// u16 family is accepted too.
bool ReadSockaddr(u32 addr, int len, sockaddr_in *out, int *err) {
	if (len < 8 || !mem::IsValidRange(addr, 8)) {
		*err = net::INET_EINVAL;
		return false;
	}
	hostnet::PspSockaddrIn p{};
	mem::ReadBytes(&p, addr, (u32)std::min(len, 16));
	if (p.family != INET_AF_INET && !(p.family == 0 && p.len == INET_AF_INET)) {
		*err = net::INET_EAFNOSUPPORT;
		return false;
	}
	*out = hostnet::ToHost(p);
	return true;
}

// Writes a host address as a guest struct sockaddr_in into (addr, *lenPtr), BSD style: at most
// *lenPtr bytes, and *lenPtr becomes the full size.
void WriteSockaddr(u32 addr, u32 lenPtr, const sockaddr_in &a) {
	if (!addr || !lenPtr || !mem::IsValidRange(lenPtr, 4)) return;
	hostnet::PspSockaddrIn p = hostnet::ToPsp(a);
	u32 room = GRead32(lenPtr);
	u32 n = std::min<u32>(room, sizeof(p));
	if (n && mem::IsValidRange(addr, n)) mem::WriteBytes(addr, &p, n);
	GWrite32(lenPtr, sizeof(p));
}

// The interface a wildcard bind really takes: the virtual access point's address once connected,
// loopback before (a PSP without an access point reaches nobody either).
u32 InterfaceAddress() {
	if (g_apctlState == net::APCTL_STATE_GOT_IP && g_apctlLease.ip) return g_apctlLease.ip;
	return htonl(INADDR_LOOPBACK);
}

// Before the access point gives the PSP an address only loopback is reachable.
bool Reachable(const sockaddr_in &to) {
	return hostnet::IsLoopback(to.sin_addr.s_addr) || g_apctlState == net::APCTL_STATE_GOT_IP;
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

// The Internet checksum (RFC 1071) of `n` bytes.
u16 InetChecksum(const u8 *p, size_t n) {
	u32 sum = 0;
	for (size_t i = 0; i + 1 < n; i += 2) sum += (u32)p[i] << 8 | p[i + 1];
	if (n & 1) sum += (u32)p[n - 1] << 8;
	while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
	return (u16)~sum;
}

// The game pings the servers in its list with ICMP echo requests on a raw socket and shows nothing for a server until the
// reply comes. Where the host won't open that socket the request is answered here, as if the server replied at once (the
// list's Ping column then reads 0): a reply as a raw socket receives it, the IPv4 header and the ICMP message.
int AnswerIcmpEcho(InetSocket *sock, u32 buf, u32 len, const sockaddr_in &to) {
	if (len > 65000 || (len && !mem::IsValidRange(buf, len))) return SetErrno(net::INET_EFAULT);
	if (len < 8) return SetErrno(net::INET_EINVAL);
	std::vector<u8> out(20 + len);
	mem::ReadBytes(out.data() + 20, buf, len);
	constexpr u8 ECHO_REQUEST = 8;
	if (out[20] != ECHO_REQUEST) return (int)len;       // anything else goes nowhere
	out[20] = 0;                                        // echo reply: the request's identifier, sequence number and data
	out[22] = out[23] = 0;
	const u16 icmp = InetChecksum(out.data() + 20, len);
	out[22] = (u8)(icmp >> 8), out[23] = (u8)icmp;
	const u32 total = (u32)out.size(), src = ntohl(to.sin_addr.s_addr), dst = 0x7F000001;
	const u8 header[20] = {0x45, 0, (u8)(total >> 8), (u8)total, 0, 0, 0, 0, 64, 1, 0, 0,
	                       (u8)(src >> 24), (u8)(src >> 16), (u8)(src >> 8), (u8)src,
	                       (u8)(dst >> 24), (u8)(dst >> 16), (u8)(dst >> 8), (u8)dst};
	std::copy(header, header + 20, out.begin());
	const u16 ip = InetChecksum(out.data(), 20);
	out[10] = (u8)(ip >> 8), out[11] = (u8)ip;
	sock->injected.push_back({std::move(out), to});
	return (int)len;
}

int sceNetInetSocket(int domain, int type, int protocol) {
	if (domain != INET_AF_INET) return SetErrno(net::INET_EAFNOSUPPORT);
	int baseType = type & 0xF;
	if (baseType != INET_SOCK_STREAM && baseType != INET_SOCK_DGRAM && baseType != INET_SOCK_RAW) {
		return SetErrno(net::INET_EPROTOTYPE);
	}
	int s = INET_MIN_SOCKET;
	while (s < INET_MAX_SOCKETS && g_inetSockets[s].used) s++;
	if (s == INET_MAX_SOCKETS) return SetErrno(net::INET_ENOBUFS);
	InetSocket sock;
	sock.used = true;
	sock.domain = domain;
	sock.type = baseType;
	sock.protocol = protocol;
	sock.nonblocking = (type & INET_SOCK_NONBLOCK) != 0;
	if (net::Enabled()) {
		int hostType = baseType == INET_SOCK_STREAM ? SOCK_STREAM : baseType == INET_SOCK_DGRAM ? SOCK_DGRAM : SOCK_RAW;
		int err = 0;
		sock.host = hostnet::OpenSocket(AF_INET, hostType, protocol, &err);
		if (!sock.host.Valid() && baseType == INET_SOCK_RAW && protocol == INET_IPPROTO_ICMP) {
			LOGI(LOG, "sceNetInetSocket: the host won't open a raw ICMP socket (%d): echo requests are answered by pspc", err);
			sock.host = hostnet::OpenSocket(AF_INET, SOCK_DGRAM, 0, &err);
			sock.icmpEcho = sock.host.Valid();
		}
		if (!sock.host.Valid()) {
			LOGW(LOG, "sceNetInetSocket(%d, %d, %d): host socket failed (%d)", domain, type, protocol, err);
			return SetErrnoFromWsa(err);
		}
		sock.abortKey = (2ull << 40) | g_nextInetAbortKey++;
		net::HostSocketOpened();
	}
	g_inetSockets[s] = sock;
	LOGD(LOG, "sceNetInetSocket(%d, %d, %d) -> %d", domain, type, protocol, s);
	return s;
}

int sceNetInetClose(int s) {
	if (!GetSocket(s)) return SetErrno(net::INET_EBADF);
	LOGD(LOG, "sceNetInetClose(%d)", s);
	ReleaseSocket(s, false);
	return 0;
}
int sceNetInetCloseWithRST(int s) {
	if (!GetSocket(s)) return SetErrno(net::INET_EBADF);
	ReleaseSocket(s, true);
	return 0;
}

int sceNetInetSocketAbort(int s) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (sock->host.Valid()) net::AbortWaits(sock->abortKey);
	else AbortOfflineWaits(s);
	return 0;
}

int sceNetInetShutdown(int s, int how) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) {
		if (sock->type == INET_SOCK_STREAM) return SetErrno(net::INET_ENOTCONN);
		sock->shutdown = true;
		return 0;
	}
	int hostHow = how == 0 ? hostnet::ShutRead : how == 1 ? hostnet::ShutWrite : hostnet::ShutBoth;
	if (how < 0 || how > 2) return SetErrno(net::INET_EINVAL);
	if (shutdown(sock->host.s, hostHow) != 0) return SetErrnoFromWsa(hostnet::LastSocketError());
	sock->shutdown = true;
	return 0;
}

// UDP ports Windows' own services hold on every interface address (SSDP Discovery: 1900, mDNS:
// 5353, LLMNR: 5355). On a PSP the game has them to itself: SCE-RT games bind 1900 for UPnP. They
// bind here sharing the port (SO_REUSEADDR), as Windows UPnP clients do, unless one of this game's
// own sockets has it.
bool SharedSystemPort(u16 port) { return port == 1900 || port == 5353 || port == 5355; }

bool GuestHoldsUdpPort(const InetSocket *self, u16 port) {
	for (int s = INET_MIN_SOCKET; s < INET_MAX_SOCKETS; s++) {
		const InetSocket &o = g_inetSockets[s];
		if (&o == self || !o.used || !o.bound || o.type != INET_SOCK_DGRAM || !o.host.Valid()) continue;
		sockaddr_in got{};
		int len = sizeof(got);
		if (hostnet::GetSockName(o.host.s, (sockaddr *)&got, &len) == 0 && ntohs(got.sin_port) == port) return true;
	}
	return false;
}

int BindHost(InetSocket *sock, sockaddr_in a) {
	// A wildcard bind takes the access point's address (what getsockname reports), but binds the host
	// socket to all interfaces once connected: Windows won't send from a LAN-bound socket to loopback,
	// where a game server on the same PC listens. Before the access point connects: loopback only.
	if (a.sin_addr.s_addr == htonl(INADDR_ANY)) {
		if (InterfaceAddress() == htonl(INADDR_LOOPBACK)) a.sin_addr.s_addr = InterfaceAddress();
		else sock->wildcard = true;
	}
	if (bind(sock->host.s, (const sockaddr *)&a, sizeof(a)) != 0) {
		const int e = hostnet::LastSocketError();
		const u16 port = ntohs(a.sin_port);
		if ((e != hostnet::Error_EADDRINUSE && e != hostnet::Error_EACCES) || sock->type != INET_SOCK_DGRAM || port == 0 ||
		    GuestHoldsUdpPort(sock, port))
			return SetErrnoFromWsa(e);
		if (SharedSystemPort(port)) {
			int on = 1;
			setsockopt(sock->host.s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
			if (bind(sock->host.s, (const sockaddr *)&a, sizeof(a)) != 0) return SetErrnoFromWsa(hostnet::LastSocketError());
			LOGI(LOG, "  UDP port %u is held by a Windows service too: shared", port);
		} else {
			// Another program - or another PSPC instance on this PC - has the port (or Windows reserved
			// it): take a free one. getsockname reports the real port, so a game that tells other players
			// where to reach it (peer-to-peer games) gives them an address that works.
			a.sin_port = 0;
			if (bind(sock->host.s, (const sockaddr *)&a, sizeof(a)) != 0) return SetErrnoFromWsa(hostnet::LastSocketError());
			sockaddr_in got{};
			int len = sizeof(got);
			hostnet::GetSockName(sock->host.s, (sockaddr *)&got, &len);
			LOGI(LOG, "  UDP port %u is in use on this PC: using %u instead", port, ntohs(got.sin_port));
		}
	}
	sock->bound = true;
	if (logging::g_level <= LogLevel::Debug) {
		sockaddr_in got{};
		int len = sizeof(got);
		hostnet::GetSockName(sock->host.s, (sockaddr *)&got, &len);
		LOGD(LOG, "  bound to %s:%u on the host", hostnet::Ipv4ToString(got.sin_addr.s_addr).c_str(), ntohs(got.sin_port));
	}
	return 0;
}

int sceNetInetBind(int s, u32 addrPtr, int addrLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) {
		if (!mem::IsValidRange(addrPtr, 8) || addrLen < 8) return SetErrno(net::INET_EINVAL);
		if (sock->bound) return SetErrno(net::INET_EINVAL);
		mem::ReadBytes(sock->localAddr, addrPtr, (u32)std::min(addrLen, 16));
		sock->bound = true;
		return 0;
	}
	sockaddr_in a;
	int err;
	if (!ReadSockaddr(addrPtr, addrLen, &a, &err)) return SetErrno(err);
	if (sock->bound) return SetErrno(net::INET_EINVAL);
	int r = BindHost(sock, a);
	LOGI(LOG, "sceNetInetBind(%d, %s:%u) -> %d", s, hostnet::Ipv4ToString(a.sin_addr.s_addr).c_str(), ntohs(a.sin_port), r);
	return r;
}

int sceNetInetListen(int s, int backlog) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (sock->type != INET_SOCK_STREAM) return SetErrno(net::INET_EOPNOTSUPP);
	if (!sock->host.Valid()) {
		sock->listening = true;
		return 0;
	}
	if (!sock->bound) {
		// BSD binds an unbound socket to an ephemeral port; Winsock refuses.
		sockaddr_in any{};
		any.sin_family = AF_INET;
		if (BindHost(sock, any) < 0) return -1;
	}
	if (listen(sock->host.s, backlog == INET_SOMAXCONN || backlog <= 0 ? SOMAXCONN : backlog) != 0)
		return SetErrnoFromWsa(hostnet::LastSocketError());
	sock->listening = true;
	LOGI(LOG, "sceNetInetListen(%d, %d)", s, backlog);
	return 0;
}

int sceNetInetAccept(int s, u32 addrPtr, u32 addrLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->listening) return SetErrno(net::INET_EINVAL);
	if (!sock->host.Valid()) return WaitForNothing(s, sock, 0);
	u64 deadline = Deadline(sock->rcvTimeoutUs);
	for (;;) {
		sockaddr_in peer{};
		int peerLen = sizeof(peer);
		hostnet::SocketHandle ns = hostnet::Accept(sock->host.s, (sockaddr *)&peer, &peerLen);
		if (ns != hostnet::InvalidSocket) {
			int id = INET_MIN_SOCKET;
			while (id < INET_MAX_SOCKETS && g_inetSockets[id].used) id++;
			if (id == INET_MAX_SOCKETS) {
				hostnet::CloseSocketHandle(ns);
				return SetErrno(net::INET_ENOBUFS);
			}
			InetSocket n;
			int err = 0;
			if (!hostnet::AdoptSocket(ns, &n.host, &err)) {
				hostnet::CloseSocketHandle(ns);
				return SetErrnoFromWsa(err);
			}
			n.used = true;
			n.domain = sock->domain;
			n.type = sock->type;
			n.protocol = sock->protocol;
			n.bound = true;
			// A connection inherits the listening socket's I/O mode and timeouts (4.4BSD sonewconn).
			n.nonblocking = sock->nonblocking;
			n.rcvTimeoutUs = sock->rcvTimeoutUs;
			n.sndTimeoutUs = sock->sndTimeoutUs;
			n.abortKey = (2ull << 40) | g_nextInetAbortKey++;
			net::HostSocketOpened();
			g_inetSockets[id] = n;
			WriteSockaddr(addrPtr, addrLenPtr, peer);
			LOGI(LOG, "sceNetInetAccept(%d) -> %d from %s:%u", s, id, hostnet::Ipv4ToString(peer.sin_addr.s_addr).c_str(),
			     ntohs(peer.sin_port));
			return id;
		}
		int e = hostnet::LastSocketError();
		if (e != hostnet::Error_EWOULDBLOCK) return SetErrnoFromWsa(e);
		if (sock->nonblocking) return SetErrno(net::INET_EAGAIN);
		if (DeadlinePassed(deadline)) return SetErrno(net::INET_EAGAIN);
		if (WaitSocket(sock, deadline) == net::HOSTWAIT_ABORTED) return SetErrno(net::INET_EINTR);
		sock = GetSocket(s);
		if (!sock || !sock->host.Valid()) return SetErrno(net::INET_EBADF);
	}
}

// Notices the end of a non-blocking connect: true when it is over, with *pspErrno its result.
bool ConnectFinished(InetSocket *sock, int *pspErrno) {
	hostnet::Readiness r;
	if (!hostnet::CheckReady(sock->host.s, &r)) {
		*pspErrno = net::INET_EBADF;
		return true;
	}
	if (r.except) {
		int e = hostnet::PendingError(sock->host.s);
		*pspErrno = hostnet::PspErrnoFromWsa(e ? e : hostnet::Error_ECONNREFUSED);
		return true;
	}
	if (r.write) {
		*pspErrno = 0;
		return true;
	}
	return false;
}

int sceNetInetConnect(int s, u32 addrPtr, int addrLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) {
		if (!mem::IsValidRange(addrPtr, 8) || addrLen < 8) return SetErrno(net::INET_EINVAL);
		LOGI(LOG, "sceNetInetConnect(%d): no network", s);
		return SetErrno(net::INET_ENETUNREACH);
	}
	sockaddr_in a;
	int err;
	if (!ReadSockaddr(addrPtr, addrLen, &a, &err)) return SetErrno(err);
	LOGI(LOG, "sceNetInetConnect(%d, %s:%u)%s", s, hostnet::Ipv4ToString(a.sin_addr.s_addr).c_str(), ntohs(a.sin_port),
	     sock->nonblocking ? " non-blocking" : "");
	if (!Reachable(a)) return SetErrno(net::INET_ENETUNREACH);
	if (connect(sock->host.s, (const sockaddr *)&a, sizeof(a)) == 0) {
		sock->bound = true;
		sock->connecting = false;
		return 0;
	}
	int e = hostnet::LastSocketError();
	if (e == hostnet::Error_EINVAL && sock->connecting) e = hostnet::Error_EALREADY;  // a second connect while one runs
	if (e != hostnet::Error_EWOULDBLOCK) {
		if (e == hostnet::Error_EISCONN) sock->connecting = false;
		return SetErrnoFromWsa(e, true);
	}
	sock->bound = true;
	sock->connecting = true;
	sock->savedError = 0;
	if (sock->nonblocking) return SetErrno(net::INET_EINPROGRESS);
	for (;;) {
		int result;
		if (ConnectFinished(sock, &result)) {
			sock->connecting = false;
			if (result) {
				LOGI(LOG, "sceNetInetConnect(%d): %s", s, hostnet::PspErrnoName(result));
				return SetErrno(result);
			}
			return 0;
		}
		if (WaitSocket(sock, 0) == net::HOSTWAIT_ABORTED) return SetErrno(net::INET_EINTR);
		sock = GetSocket(s);
		if (!sock || !sock->host.Valid()) return SetErrno(net::INET_EBADF);
	}
}

// send / sendto. `to` is null for send.
int SendCommon(int s, u32 buf, u32 len, u32 flags, const sockaddr_in *to) {
	InetSocket *sock = GetSocket(s);
	if (len && !mem::IsValidRange(buf, len)) return SetErrno(net::INET_EFAULT);
	if (to && !Reachable(*to)) return SetErrno(net::INET_ENETUNREACH);
	bool dontWait = sock->nonblocking || (flags & INET_MSG_DONTWAIT);
	int hostFlags = hostnet::MapMsgFlags((int)flags);
	u64 deadline = Deadline(sock->sndTimeoutUs);
	u32 sent = 0;
	for (;;) {
		const char *p = (const char *)GPtr(buf) + sent;
		int n = to ? hostnet::SendTo(sock->host.s, p, (int)(len - sent), hostFlags, (const sockaddr *)to, sizeof(*to))
		           : hostnet::Send(sock->host.s, p, (int)(len - sent), hostFlags);
		if (n >= 0) {
			sent += (u32)n;
			if (sent >= len || sock->type != INET_SOCK_STREAM || dontWait) return (int)sent;
			continue;  // a blocking stream send queues everything
		}
		int e = hostnet::LastSocketError();
		if (e != hostnet::Error_EWOULDBLOCK) {
			if (sent) return (int)sent;
			if (e == hostnet::Error_ESHUTDOWN) return SetErrno(net::INET_EPIPE);
			return SetErrnoFromWsa(e);
		}
		if (dontWait || DeadlinePassed(deadline)) return sent ? (int)sent : SetErrno(net::INET_EAGAIN);
		if (WaitSocket(sock, deadline) == net::HOSTWAIT_ABORTED) return sent ? (int)sent : SetErrno(net::INET_EINTR);
		sock = GetSocket(s);
		if (!sock || !sock->host.Valid()) return sent ? (int)sent : SetErrno(net::INET_EBADF);
	}
}

// recv / recvfrom. Fills the sender's address for recvfrom.
int RecvCommon(int s, u32 buf, u32 len, u32 flags, u32 fromPtr, u32 fromLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (len && !mem::IsValidRange(buf, len)) return SetErrno(net::INET_EFAULT);
	bool dontWait = sock->nonblocking || (flags & INET_MSG_DONTWAIT);
	bool waitAll = (flags & hostnet::PSP_MSG_WAITALL) && sock->type == INET_SOCK_STREAM && !(flags & hostnet::PSP_MSG_PEEK);
	int hostFlags = hostnet::MapMsgFlags((int)flags);
	u64 deadline = Deadline(sock->rcvTimeoutUs);
	u32 got = 0;
	for (;;) {
		// Checked on every pass: a thread already waiting here picks it up at its next recheck.
		if (!sock->injected.empty() && sock->type != INET_SOCK_STREAM) {
			const auto &d = sock->injected.front();
			const u32 n = std::min<u32>(len, (u32)d.first.size());
			if (n) mem::WriteBytes(buf, d.first.data(), n);
			if (fromPtr) WriteSockaddr(fromPtr, fromLenPtr, d.second);
			if (!(flags & hostnet::PSP_MSG_PEEK)) sock->injected.pop_front();
			return (int)n;
		}
		char *p = (char *)GPtr(buf) + got;
		sockaddr_in from{};
		int fromLen = sizeof(from);
		int n = hostnet::RecvFrom(sock->host.s, p, (int)(len - got), hostFlags, (sockaddr *)&from, &fromLen);
		int e = n < 0 ? hostnet::LastSocketError() : 0;
		if (n < 0 && e == hostnet::Error_EMSGSIZE && sock->type != INET_SOCK_STREAM) {
			n = (int)len;  // the datagram was cut to the buffer: BSD returns what fit
			e = 0;
		}
		if (n >= 0) {
			if (sock->type == INET_SOCK_DGRAM && fromLen >= (int)sizeof(sockaddr_in))
				LOGD(LOG, "recv(%d): %d bytes from %s:%u", s, n, hostnet::Ipv4ToString(from.sin_addr.s_addr).c_str(), ntohs(from.sin_port));
			if (fromPtr && fromLen >= (int)sizeof(sockaddr_in) && from.sin_family == AF_INET) WriteSockaddr(fromPtr, fromLenPtr, from);
			got += (u32)n;
			if (n == 0 || !waitAll || got >= len) return (int)got;
			continue;
		}
		if (e != hostnet::Error_EWOULDBLOCK) {
			if (got) return (int)got;
			return SetErrnoFromWsa(e);
		}
		if (got && !waitAll) return (int)got;
		if (dontWait || DeadlinePassed(deadline)) return got ? (int)got : SetErrno(net::INET_EAGAIN);
		if (WaitSocket(sock, deadline) == net::HOSTWAIT_ABORTED) return got ? (int)got : SetErrno(net::INET_EINTR);
		sock = GetSocket(s);
		if (!sock || !sock->host.Valid()) return got ? (int)got : SetErrno(net::INET_EBADF);
	}
}

// The DNS server the access point told the game about (the host's, or a revival server).
bool IsDnsServer(const sockaddr_in &to) {
	return ntohs(to.sin_port) == 53 && to.sin_addr.s_addr != 0 &&
	       (to.sin_addr.s_addr == g_apctlLease.dns1 || to.sin_addr.s_addr == g_apctlLease.dns2);
}

// A DNS query the game sends itself (SCE-RT games resolve their servers this way): answered here
// when the name is redirected to a revival server (net_infra.cpp). Returns the bytes "sent", or -1
// to send the query on to the real server.
int TryAnswerDns(int s, u32 buf, u32 len, const sockaddr_in &to) {
	if (len < 12 || len > 512 || !mem::IsValidRange(buf, len) || !net::InfraHasRedirects()) return -1;
	struct Out {
		std::vector<u8> query, response;
		bool answered = false;
	};
	auto out = std::make_shared<Out>();
	out->query.resize(len);
	mem::ReadBytes(out->query.data(), buf, len);
	const bool done = net::RunBlockingOnHost(
		[out] { out->answered = net::InfraAnswerDnsQuery(out->query.data(), out->query.size(), &out->response); },
		sched::NowUs() + 5000000);
	InetSocket *sock = GetSocket(s);
	if (!done || !out->answered || !sock) return -1;
	sock->injected.push_back({out->response, to});
	return (int)len;
}

int sceNetInetSend(int s, u32 buf, u32 len, u32 flags) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) return SetErrno(sock->type == INET_SOCK_STREAM ? net::INET_ENOTCONN : net::INET_EDESTADDRREQ);
	if (sock->type == INET_SOCK_DGRAM) {
		sockaddr_in peer{};
		int peerLen = sizeof(peer);
		if (hostnet::GetPeerName(sock->host.s, (sockaddr *)&peer, &peerLen) == 0 && IsDnsServer(peer)) {
			const int r = TryAnswerDns(s, buf, len, peer);
			if (r >= 0) return r;
		}
	}
	return SendCommon(s, buf, len, flags, nullptr);
}
int sceNetInetSendto(int s, u32 buf, u32 len, u32 flags, u32 toPtr, u32 toLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) {
		if (sock->type == INET_SOCK_STREAM) return SetErrno(net::INET_ENOTCONN);
		return SetErrno(net::INET_ENETUNREACH);
	}
	if (!toPtr || sock->type == INET_SOCK_STREAM) return SendCommon(s, buf, len, flags, nullptr);
	sockaddr_in to;
	int err;
	if (!ReadSockaddr(toPtr, (int)toLen, &to, &err)) return SetErrno(err);
	LOGD(LOG, "sceNetInetSendto(%d, %u bytes) to %s:%u", s, len, hostnet::Ipv4ToString(to.sin_addr.s_addr).c_str(),
	     ntohs(to.sin_port));
	if (sock->icmpEcho) return Reachable(to) ? AnswerIcmpEcho(sock, buf, len, to) : SetErrno(net::INET_ENETUNREACH);
	if (sock->type == INET_SOCK_DGRAM && IsDnsServer(to)) {
		const int r = TryAnswerDns(s, buf, len, to);
		if (r >= 0) return r;
	}
	return SendCommon(s, buf, len, flags, &to);
}

int sceNetInetRecv(int s, u32 buf, u32 len, u32 flags) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) {
		if (sock->type == INET_SOCK_STREAM) return SetErrno(net::INET_ENOTCONN);
		return WaitForNothing(s, sock, flags);
	}
	return RecvCommon(s, buf, len, flags, 0, 0);
}
int sceNetInetRecvfrom(int s, u32 buf, u32 len, u32 flags, u32 fromPtr, u32 fromLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) return sceNetInetRecv(s, buf, len, flags);
	return RecvCommon(s, buf, len, flags, fromPtr, fromLenPtr);
}

// struct msghdr (NetBSD, 32-bit): msg_name, msg_namelen, msg_iov, msg_iovlen, msg_control,
// msg_controllen, msg_flags. struct iovec: iov_base, iov_len. Control messages aren't passed on.
struct InetMsghdr {
	u32 name, nameLen, iov, iovLen, control, controlLen;
	s32 flags;
};
static_assert(sizeof(InetMsghdr) == 28, "msghdr");

bool ReadIovecs(const InetMsghdr &m, std::vector<std::pair<u32, u32>> *out, u32 *total) {
	*total = 0;
	if (m.iovLen > 1024 || (m.iovLen && !mem::IsValidRange(m.iov, m.iovLen * 8))) return false;
	for (u32 i = 0; i < m.iovLen; i++) {
		u32 base = GRead32(m.iov + i * 8), len = GRead32(m.iov + i * 8 + 4);
		if (len && !mem::IsValidRange(base, len)) return false;
		out->push_back({base, len});
		*total += len;
	}
	return true;
}

// A scratch buffer in guest memory isn't needed: gather into a host buffer, send it in one go.
int sceNetInetSendmsg(int s, u32 msgAddr, u32 flags) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) return SetErrno(net::INET_ENETUNREACH);
	if (!mem::IsValidRange(msgAddr, sizeof(InetMsghdr))) return SetErrno(net::INET_EFAULT);
	InetMsghdr m;
	mem::ReadBytes(&m, msgAddr, sizeof(m));
	std::vector<std::pair<u32, u32>> iov;
	u32 total;
	if (!ReadIovecs(m, &iov, &total)) return SetErrno(net::INET_EFAULT);
	std::vector<char> data;
	data.reserve(total);
	for (auto &v : iov) data.insert(data.end(), (const char *)GPtr(v.first), (const char *)GPtr(v.first) + v.second);
	sockaddr_in to{};
	bool haveTo = false;
	if (m.name && sock->type != INET_SOCK_STREAM) {
		int err;
		if (!ReadSockaddr(m.name, (int)m.nameLen, &to, &err)) return SetErrno(err);
		haveTo = true;
		if (!Reachable(to)) return SetErrno(net::INET_ENETUNREACH);
	}
	bool dontWait = sock->nonblocking || (flags & INET_MSG_DONTWAIT);
	u64 deadline = Deadline(sock->sndTimeoutUs);
	for (;;) {
		int n = haveTo ? hostnet::SendTo(sock->host.s, data.data(), (int)data.size(), hostnet::MapMsgFlags((int)flags), (const sockaddr *)&to, sizeof(to))
		               : hostnet::Send(sock->host.s, data.data(), (int)data.size(), hostnet::MapMsgFlags((int)flags));
		if (n >= 0) return n;
		int e = hostnet::LastSocketError();
		if (e != hostnet::Error_EWOULDBLOCK) return SetErrnoFromWsa(e);
		if (dontWait || DeadlinePassed(deadline)) return SetErrno(net::INET_EAGAIN);
		if (WaitSocket(sock, deadline) == net::HOSTWAIT_ABORTED) return SetErrno(net::INET_EINTR);
		sock = GetSocket(s);
		if (!sock || !sock->host.Valid()) return SetErrno(net::INET_EBADF);
	}
}

int sceNetInetRecvmsg(int s, u32 msgAddr, u32 flags) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) return sceNetInetRecv(s, 0, 0, flags);
	if (!mem::IsValidRange(msgAddr, sizeof(InetMsghdr))) return SetErrno(net::INET_EFAULT);
	InetMsghdr m;
	mem::ReadBytes(&m, msgAddr, sizeof(m));
	std::vector<std::pair<u32, u32>> iov;
	u32 total;
	if (!ReadIovecs(m, &iov, &total)) return SetErrno(net::INET_EFAULT);
	std::vector<char> data(std::max<u32>(total, 1));
	bool dontWait = sock->nonblocking || (flags & INET_MSG_DONTWAIT);
	u64 deadline = Deadline(sock->rcvTimeoutUs);
	for (;;) {
		sockaddr_in from{};
		int fromLen = sizeof(from);
		int n = hostnet::RecvFrom(sock->host.s, data.data(), (int)total, hostnet::MapMsgFlags((int)flags), (sockaddr *)&from, &fromLen);
		bool truncated = false;
		if (n < 0 && hostnet::LastSocketError() == hostnet::Error_EMSGSIZE) {
			n = (int)total;
			truncated = true;
		}
		if (n >= 0) {
			u32 off = 0;
			for (auto &v : iov) {
				u32 c = std::min<u32>(v.second, (u32)n - off);
				if (c) mem::WriteBytes(v.first, data.data() + off, c);
				off += c;
				if (off >= (u32)n) break;
			}
			if (m.name && fromLen >= (int)sizeof(sockaddr_in)) {
				hostnet::PspSockaddrIn p = hostnet::ToPsp(from);
				u32 c = std::min<u32>(m.nameLen, sizeof(p));
				if (c && mem::IsValidRange(m.name, c)) mem::WriteBytes(m.name, &p, c);
				GWrite32(msgAddr + 4, sizeof(p));
			}
			GWrite32(msgAddr + 20, 0);                                         // msg_controllen
			GWrite32(msgAddr + 24, truncated ? (u32)hostnet::PSP_MSG_TRUNC : 0);  // msg_flags
			return n;
		}
		int e = hostnet::LastSocketError();
		if (e != hostnet::Error_EWOULDBLOCK) return SetErrnoFromWsa(e);
		if (dontWait || DeadlinePassed(deadline)) return SetErrno(net::INET_EAGAIN);
		if (WaitSocket(sock, deadline) == net::HOSTWAIT_ABORTED) return SetErrno(net::INET_EINTR);
		sock = GetSocket(s);
		if (!sock || !sock->host.Valid()) return SetErrno(net::INET_EBADF);
	}
}

int sceNetInetSetsockopt(int s, int level, int name, u32 valPtr, int valLen) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	u32 val = mem::IsValidRange(valPtr, 4) && valLen >= 4 ? GRead32(valPtr) : (mem::IsValidRange(valPtr, 1) && valLen >= 1 ? GRead8(valPtr) : 0);
	if (level == hostnet::PSP_SOL_SOCKET) {
		switch (name) {
		case hostnet::PSP_SO_NBIO: sock->nonblocking = val != 0; break;
		case hostnet::PSP_SO_BIO: sock->nonblocking = false; break;
		case hostnet::PSP_SO_RCVTIMEO: sock->rcvTimeoutUs = val; break;
		case hostnet::PSP_SO_SNDTIMEO: sock->sndTimeoutUs = val; break;
		case hostnet::PSP_SO_RCVBUF:
		case hostnet::PSP_SO_SNDBUF:
			// UNO asks for a huge buffer and expects the PSP's refusal.
			if (val > 8 * 1024 * 1024) return SetErrno(net::INET_ENOBUFS);
			break;
		default: break;
		}
	}
	sock->options[OptKey(level, name)] = val;
	if (!sock->host.Valid()) return 0;
	if (level == hostnet::PSP_SOL_SOCKET) {
		switch (name) {
		case hostnet::PSP_SO_NBIO:
		case hostnet::PSP_SO_BIO:
		case hostnet::PSP_SO_RCVTIMEO:
		case hostnet::PSP_SO_SNDTIMEO:
		case hostnet::PSP_SO_NOSIGPIPE:
		case hostnet::PSP_SO_REUSEPORT:
		case hostnet::PSP_SO_ONESBCAST:  // Outrun 2006; nothing to do on the host
			return 0;  // emulated here (host sockets are always non-blocking)
		case hostnet::PSP_SO_LINGER: {
			if (valLen < 8 || !mem::IsValidRange(valPtr, 8)) return SetErrno(net::INET_EINVAL);
			linger l{(u_short)(GRead32(valPtr) != 0), (u_short)std::min<u32>(GRead32(valPtr + 4), 0xFFFF)};
			if (setsockopt(sock->host.s, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof(l)) != 0)
				return SetErrnoFromWsa(hostnet::LastSocketError());
			return 0;
		}
		default: break;
		}
	}
	int hl, hn;
	if (!hostnet::MapSockopt(level, name, &hl, &hn)) {
		LOGD(LOG, "sceNetInetSetsockopt(%d, %x, %x): no host equivalent; kept", s, level, name);
		return 0;
	}
	std::vector<char> raw((size_t)std::clamp(valLen, 0, 64));
	if (!raw.empty()) {
		if (!mem::IsValidRange(valPtr, (u32)raw.size())) return SetErrno(net::INET_EFAULT);
		mem::ReadBytes(raw.data(), valPtr, (u32)raw.size());
	}
	if (setsockopt(sock->host.s, hl, hn, raw.data(), (int)raw.size()) != 0) {
		int e = hostnet::LastSocketError();
		LOGD(LOG, "sceNetInetSetsockopt(%d, %x, %x, %u): host error %d", s, level, name, val, e);
		return SetErrnoFromWsa(e);
	}
	return 0;
}

int sceNetInetGetsockopt(int s, int level, int name, u32 valPtr, u32 valLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!mem::IsValidRange(valPtr, 4) || !mem::IsValidRange(valLenPtr, 4)) return SetErrno(net::INET_EFAULT);
	auto it = sock->options.find(OptKey(level, name));
	bool set = it != sock->options.end();
	u32 val = set ? it->second : 0;
	bool known = true;
	if (level == hostnet::PSP_SOL_SOCKET) {
		switch (name) {
		case hostnet::PSP_SO_TYPE: val = (u32)sock->type; break;
		case hostnet::PSP_SO_NBIO: val = sock->nonblocking ? 1 : 0; break;
		case hostnet::PSP_SO_RCVTIMEO: val = sock->rcvTimeoutUs; break;
		case hostnet::PSP_SO_SNDTIMEO: val = sock->sndTimeoutUs; break;
		case hostnet::PSP_SO_ERROR:
			val = (u32)sock->savedError;
			sock->savedError = 0;
			if (!val && sock->host.Valid()) {
				int e = hostnet::PendingError(sock->host.s);
				val = e ? (u32)hostnet::PspErrnoFromWsa(e) : 0;
			}
			break;
		case hostnet::PSP_SO_SNDBUF:
			if (!set) val = INET_DEFAULT_SNDBUF;
			break;
		case hostnet::PSP_SO_RCVBUF:
			if (!set) val = sock->type == INET_SOCK_DGRAM ? INET_DEFAULT_RCVBUF_DGRAM : INET_DEFAULT_RCVBUF_STREAM;
			break;
		case hostnet::PSP_SO_ACCEPTCONN: val = sock->listening ? 1 : 0; break;
		default: known = false; break;
		}
	} else {
		known = false;
	}
	if (!known && !set && sock->host.Valid()) {
		int hl, hn;
		if (hostnet::MapSockopt(level, name, &hl, &hn)) {
			int hv = 0;
			int hlen = sizeof(hv);
			if (hostnet::GetSockOpt(sock->host.s, hl, hn, (char *)&hv, &hlen) != 0) return SetErrnoFromWsa(hostnet::LastSocketError());
			val = hlen == 1 ? (u32)(u8)hv : (u32)hv;
		}
	}
	GWrite32(valPtr, val);
	GWrite32(valLenPtr, 4);
	return 0;
}

int sceNetInetGetsockname(int s, u32 namePtr, u32 nameLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!mem::IsValidRange(namePtr, 16) || !mem::IsValidRange(nameLenPtr, 4)) return SetErrno(net::INET_EFAULT);
	if (!sock->host.Valid()) {
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
	sockaddr_in a{};
	a.sin_family = AF_INET;
	int len = sizeof(a);
	if (hostnet::GetSockName(sock->host.s, (sockaddr *)&a, &len) != 0) {
		int e = hostnet::LastSocketError();
		if (e != hostnet::Error_EINVAL) return SetErrnoFromWsa(e);
		a = sockaddr_in{};  // not bound yet: BSD reports 0.0.0.0:0
		a.sin_family = AF_INET;
	}
	if (sock->wildcard && a.sin_addr.s_addr == htonl(INADDR_ANY)) a.sin_addr.s_addr = InterfaceAddress();
	WriteSockaddr(namePtr, nameLenPtr, a);
	return 0;
}

int sceNetInetGetpeername(int s, u32 namePtr, u32 nameLenPtr) {
	InetSocket *sock = GetSocket(s);
	if (!sock) return SetErrno(net::INET_EBADF);
	if (!sock->host.Valid()) return SetErrno(net::INET_ENOTCONN);
	if (!mem::IsValidRange(namePtr, 8) || !mem::IsValidRange(nameLenPtr, 4)) return SetErrno(net::INET_EFAULT);
	sockaddr_in a{};
	int len = sizeof(a);
	if (hostnet::GetPeerName(sock->host.s, (sockaddr *)&a, &len) != 0) return SetErrnoFromWsa(hostnet::LastSocketError());
	WriteSockaddr(namePtr, nameLenPtr, a);
	return 0;
}

// The state select / poll report for one socket. BSD makes a socket whose connect failed readable
// and writable (so_error tells why); Winsock only reports it as an exception.
hostnet::Readiness SocketReadiness(InetSocket *sock) {
	hostnet::Readiness r;
	if (!sock->host.Valid() || !hostnet::CheckReady(sock->host.s, &r)) return hostnet::Readiness{};
	if (!sock->injected.empty()) r.read = true;
	if (sock->connecting && (r.write || r.except)) {
		sock->connecting = false;
		if (r.except) {
			int e = hostnet::PendingError(sock->host.s);
			sock->savedError = hostnet::PspErrnoFromWsa(e ? e : hostnet::Error_ECONNREFUSED);
			r.read = r.write = true;
			r.except = false;
		}
	}
	return r;
}

bool FdIsSet(u32 set, int fd) { return set && (GRead8(set + (u32)fd / 8) >> (fd % 8)) & 1; }
void FdSet(u32 set, int fd) { GWrite8(set + (u32)fd / 8, (u8)(GRead8(set + (u32)fd / 8) | (1 << (fd % 8)))); }

int sceNetInetSelect(int nfds, u32 readfds, u32 writefds, u32 exceptfds, u32 timeoutPtr) {
	if (!net::Enabled()) {
		// Offline: nothing is ever readable; writable only matters for connected sockets, and none are.
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
	nfds = std::clamp(nfds, 0, 256);
	for (u32 set : {readfds, writefds, exceptfds}) {
		if (set && !mem::IsValidRange(set, (u32)(nfds + 7) / 8)) return SetErrno(net::INET_EFAULT);
	}
	u64 deadline = 0;
	bool poll = false;
	if (timeoutPtr) {
		if (!mem::IsValidRange(timeoutPtr, 8)) return SetErrno(net::INET_EFAULT);
		s32 sec = (s32)GRead32(timeoutPtr), usec = (s32)GRead32(timeoutPtr + 4);
		if (sec < 0 || usec < 0) return SetErrno(net::INET_EINVAL);
		u64 us = (u64)sec * 1000000 + (u64)usec;
		if (us == 0) poll = true;
		else deadline = sched::NowUs() + us;
	}
	// The sockets asked about.
	struct Want {
		int fd;
		bool r, w, x;
	};
	std::vector<Want> wants;
	for (int fd = 0; fd < nfds; fd++) {
		Want w{fd, FdIsSet(readfds, fd), FdIsSet(writefds, fd), FdIsSet(exceptfds, fd)};
		if (!w.r && !w.w && !w.x) continue;
		if (!GetSocket(fd)) return SetErrno(net::INET_EBADF);
		wants.push_back(w);
	}
	for (;;) {
		std::vector<Want> ready;
		std::vector<hostnet::EventHandle> events;
		for (const Want &w : wants) {
			InetSocket *sock = GetSocket(w.fd);
			if (!sock) return SetErrno(net::INET_EBADF);
			hostnet::Readiness r = SocketReadiness(sock);
			Want got{w.fd, w.r && r.read, w.w && r.write, w.x && r.except};
			if (got.r || got.w || got.x) ready.push_back(got);
			if (sock->host.Valid()) events.push_back(sock->host.ev);
		}
		if (!ready.empty() || poll || DeadlinePassed(deadline)) {
			for (u32 set : {readfds, writefds, exceptfds})
				if (set) mem::Memset(set, 0, (u32)(nfds + 7) / 8);
			int count = 0;
			for (const Want &w : ready) {
				if (w.r) FdSet(readfds, w.fd), count++;
				if (w.w) FdSet(writefds, w.fd), count++;
				if (w.x) FdSet(exceptfds, w.fd), count++;
			}
			return count;
		}
		if (net::WaitHostEvents(events.data(), events.size(), deadline, 0) == net::HOSTWAIT_ABORTED) return SetErrno(net::INET_EINTR);
	}
}

int sceNetInetPoll(u32 fdsPtr, u32 nfds, int timeoutMs) {
	if (nfds > 256) nfds = 256;
	if (nfds && !mem::IsValidRange(fdsPtr, nfds * 8)) return SetErrno(net::INET_EFAULT);
	if (!net::Enabled()) {
		for (u32 i = 0; i < nfds; i++) GWrite16(fdsPtr + i * 8 + 6, 0);  // revents
		if (timeoutMs > 0) sched::WaitUntil(kernel::WAITTYPE_NET, 0, sched::NowUs() + (u64)timeoutMs * 1000, false);
		else if (timeoutMs < 0) sched::Wait(kernel::WAITTYPE_NET, 0, 0, false);
		return 0;
	}
	u64 deadline = timeoutMs > 0 ? sched::NowUs() + (u64)timeoutMs * 1000 : 0;
	for (;;) {
		int count = 0;
		std::vector<hostnet::EventHandle> events;
		for (u32 i = 0; i < nfds; i++) {
			u32 e = fdsPtr + i * 8;
			int fd = (int)GRead32(e);
			u16 want = GRead16(e + 4), got = 0;
			if (fd >= 0) {
				InetSocket *sock = GetSocket(fd);
				if (!sock) {
					got = INET_POLLNVAL;
				} else {
					hostnet::Readiness r = SocketReadiness(sock);
					if (r.read) got |= want & (INET_POLLIN | INET_POLLRDNORM);
					if (r.write) got |= want & (INET_POLLOUT | INET_POLLWRBAND);
					if (r.except) got |= want & (INET_POLLPRI | INET_POLLRDBAND);
					if (r.read && sock->savedError) got |= INET_POLLERR;
					if (sock->host.Valid()) events.push_back(sock->host.ev);
				}
			}
			GWrite16(e + 6, got);
			if (got) count++;
		}
		(void)INET_POLLHUP;
		if (count || timeoutMs == 0 || DeadlinePassed(deadline)) return count;
		if (net::WaitHostEvents(events.data(), events.size(), deadline, 0) == net::HOSTWAIT_ABORTED) return SetErrno(net::INET_EINTR);
	}
}

// Address conversion (pure functions).
u32 sceNetInetInetAddr(const char *s) {
	u32 a;
	return hostnet::ParseIpv4(s, &a) ? a : 0xFFFFFFFF;
}
int sceNetInetInetAton(const char *s, u32 inAddrPtr) {
	u32 a;
	if (!hostnet::ParseIpv4(s, &a)) return 0;
	if (mem::IsValidRange(inAddrPtr, 4)) GWrite32(inAddrPtr, a);
	return 1;
}
int sceNetInetInetPton(int af, const char *s, u32 dstPtr) {
	if (af != INET_AF_INET) return SetErrno(net::INET_EAFNOSUPPORT);
	u32 a;
	if (!hostnet::ParseIpv4(s, &a)) return 0;
	if (mem::IsValidRange(dstPtr, 4)) GWrite32(dstPtr, a);
	return 1;
}
u32 sceNetInetInetNtop(int af, u32 srcPtr, u32 dstPtr, u32 size) {
	if (af != INET_AF_INET) {
		SetErrno(net::INET_EAFNOSUPPORT);
		return 0;
	}
	if (!mem::IsValidRange(srcPtr, 4)) return 0;
	std::string text = hostnet::Ipv4ToString(GRead32(srcPtr));
	u32 len = (u32)text.size();
	if (size < len + 1 || !mem::IsValidRange(dstPtr, len + 1)) {
		SetErrno(net::INET_ENOSPC);
		return 0;
	}
	mem::WriteBytes(dstPtr, text.c_str(), len + 1);
	return dstPtr;
}

// ================================================================ sceNetResolver
// Created fine; names go to the host's resolver once the PSP has a DNS server (GOT_IP). Dotted
// addresses always "resolve".

struct Resolver {
	u32 bufferAddr = 0, bufferLen = 0;
	bool running = false;
	u64 abortKey = 0;
};
std::map<int, Resolver> g_resolvers;
int g_nextResolverId = 1;
u64 g_nextResolverKey = 1;
// Starts out initialized (Outrun 2006 creates resolvers without calling sceNetResolverInit).
bool g_resolverInited = true;

int sceNetResolverInit() {
	g_resolverInited = true;
	return 0;
}
int sceNetResolverTerm() {
	for (auto &r : g_resolvers)
		if (r.second.running) net::AbortWaits(r.second.abortKey);
	g_resolvers.clear();
	g_resolverInited = false;
	return 0;
}
int sceNetResolverCreate(u32 idPtr, u32 bufferPtr, int bufferLen) {
	if (!mem::IsValidRange(idPtr, 4)) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	if (mem::IsValidRange(bufferPtr, 4) && bufferLen < 1) return (int)SCE_NET_RESOLVER_ERROR_INVALID_BUFLEN;
	if (g_resolvers.size() >= 32) return (int)SCE_NET_RESOLVER_ERROR_ID_MAX;
	int id = g_nextResolverId++;
	Resolver r;
	r.bufferAddr = bufferPtr;
	r.bufferLen = (u32)bufferLen;
	r.abortKey = (3ull << 40) | g_nextResolverKey++;
	g_resolvers[id] = r;
	GWrite32(idPtr, (u32)id);
	return 0;
}
int sceNetResolverDelete(int id) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	auto it = g_resolvers.find(id);
	if (it == g_resolvers.end()) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	if (it->second.running) net::AbortWaits(it->second.abortKey);
	g_resolvers.erase(it);
	return 0;
}
int sceNetResolverStop(int id) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	auto it = g_resolvers.find(id);
	if (it == g_resolvers.end()) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	if (!it->second.running) return (int)SCE_NET_RESOLVER_ERROR_ALREADY_STOPPED;
	net::AbortWaits(it->second.abortKey);
	it->second.running = false;
	return 0;
}

// How long a lookup may take: `timeout` seconds per try, `retry` more tries.
u64 ResolverDeadline(int timeout, int retry) {
	u64 seconds = (u64)std::clamp(timeout, 1, 30) * (u64)(std::clamp(retry, 0, 10) + 1);
	return sched::NowUs() + std::clamp<u64>(seconds, 2, 60) * 1000000;
}

u32 ResolverError(int hostError) {
	switch (hostError) {
	case hostnet::Error_NO_DATA: return SCE_NET_RESOLVER_ERROR_NO_RECORD;
	case hostnet::Error_TRY_AGAIN: return SCE_NET_RESOLVER_ERROR_RES_SERVER_FAILURE;
	case hostnet::Error_NO_RECOVERY: return SCE_NET_RESOLVER_ERROR_RES_SERVER_REFUSED;
	default: return SCE_NET_RESOLVER_ERROR_INVALID_HOST;
	}
}

int sceNetResolverStartNtoA(int id, u32 hostnamePtr, u32 inAddrPtr, int timeout, int retry) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	auto it = g_resolvers.find(id);
	if (it == g_resolvers.end()) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	if (!mem::IsValidRange(inAddrPtr, 4)) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	const char *hostp = mem::GetCString(hostnamePtr);
	if (!hostp) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	std::string host = hostp;
	u32 a;
	if (hostnet::ParseIpv4(host.c_str(), &a)) {
		GWrite32(inAddrPtr, a);
		return 0;
	}
	if (!net::Enabled() || !net::HaveDnsServer()) {
		LOGI(LOG, "sceNetResolverStartNtoA(%s): no DNS server", host.c_str());
		return (int)net::SCE_NET_RESOLVER_NO_DNS_SERVER;
	}
	if (it->second.running) return (int)SCE_NET_RESOLVER_ERROR_CTX_BUSY;
	it->second.running = true;
	u64 key = it->second.abortKey;
	struct Result {
		int rc = hostnet::Error_HOST_NOT_FOUND;
		u32 addr = 0;
	};
	auto res = std::make_shared<Result>();
	bool done = net::RunBlockingOnHost(
		[res, host] { res->rc = net::InfraRedirect(host, &res->addr) ? 0 : hostnet::ResolveIpv4(host, &res->addr); },
		ResolverDeadline(timeout, retry), key);
	it = g_resolvers.find(id);
	bool stopped = it == g_resolvers.end() || !it->second.running;
	if (it != g_resolvers.end()) it->second.running = false;
	if (!done) {
		LOGI(LOG, "sceNetResolverStartNtoA(%s): %s", host.c_str(), stopped ? "stopped" : "timed out");
		return (int)(stopped ? SCE_NET_RESOLVER_ERROR_STOPPED : SCE_NET_RESOLVER_ERROR_TIMEOUT);
	}
	if (res->rc != 0) {
		LOGI(LOG, "sceNetResolverStartNtoA(%s): not found (%d)", host.c_str(), res->rc);
		return (int)ResolverError(res->rc);
	}
	LOGI(LOG, "sceNetResolverStartNtoA(%s) -> %s", host.c_str(), hostnet::Ipv4ToString(res->addr).c_str());
	if (mem::IsValidRange(inAddrPtr, 4)) GWrite32(inAddrPtr, res->addr);
	return 0;
}
// The asynchronous requests complete before they return: nothing is ever pending.
int sceNetResolverStartNtoAAsync(int id, u32 hostnamePtr, u32 inAddrPtr, int timeout, int retry) {
	return sceNetResolverStartNtoA(id, hostnamePtr, inAddrPtr, timeout, retry);
}
int sceNetResolverStartAtoN(int id, u32 inAddrPtr, u32 hostnamePtr, int hostnameLen, int timeout, int retry) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	auto it = g_resolvers.find(id);
	if (it == g_resolvers.end()) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	if (!mem::IsValidRange(hostnamePtr, 1) || hostnameLen < 1) return (int)SCE_NET_RESOLVER_ERROR_INVALID_BUFLEN;
	if (!mem::IsValidRange(inAddrPtr, 4)) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	if (!net::Enabled() || !net::HaveDnsServer()) return (int)net::SCE_NET_RESOLVER_NO_DNS_SERVER;
	if (it->second.running) return (int)SCE_NET_RESOLVER_ERROR_CTX_BUSY;
	u32 addr = GRead32(inAddrPtr);
	it->second.running = true;
	struct Result {
		int rc = hostnet::Error_HOST_NOT_FOUND;
		std::string name;
	};
	auto res = std::make_shared<Result>();
	bool done = net::RunBlockingOnHost([res, addr] { res->rc = hostnet::ReverseLookup(addr, &res->name); },
	                                   ResolverDeadline(timeout, retry), it->second.abortKey);
	it = g_resolvers.find(id);
	bool stopped = it == g_resolvers.end() || !it->second.running;
	if (it != g_resolvers.end()) it->second.running = false;
	if (!done) return (int)(stopped ? SCE_NET_RESOLVER_ERROR_STOPPED : SCE_NET_RESOLVER_ERROR_TIMEOUT);
	if (res->rc != 0) return (int)ResolverError(res->rc);
	if (res->name.size() + 1 > (size_t)hostnameLen) return (int)SCE_NET_RESOLVER_ERROR_BUF_NO_SPACE;
	if (!mem::IsValidRange(hostnamePtr, (u32)res->name.size() + 1)) return (int)SCE_NET_RESOLVER_ERROR_INVALID_PTR;
	mem::WriteBytes(hostnamePtr, res->name.c_str(), (u32)res->name.size() + 1);
	return 0;
}
int sceNetResolverStartAtoNAsync(int id, u32 inAddr, u32 hostnamePtr, int hostnameLen, int timeout, int retry) {
	return sceNetResolverStartAtoN(id, inAddr, hostnamePtr, hostnameLen, timeout, retry);
}
int sceNetResolverPollAsync(int id, u32 resultPtr) {
	if (!g_resolverInited) return (int)SCE_NET_RESOLVER_ERROR_STOPPED;
	if (!g_resolvers.count(id)) return (int)SCE_NET_RESOLVER_ERROR_BAD_ID;
	return 0;
}
int sceNetResolverWaitAsync(int id, u32 resultPtr) { return sceNetResolverPollAsync(id, resultPtr); }

}  // namespace

namespace net {

bool NetInited() { return g_netInited; }

int ApctlGetState() { return g_apctlState; }
int ApctlConnect(int confId) { return sceNetApctlConnect(confId); }
int ApctlDisconnect() { return sceNetApctlDisconnect(); }
std::string ApctlProfileName() { return ProfileName(); }
std::string ApctlSsid() { return APCTL_SSID; }
std::string ApctlIpAddress() { return g_apctlState == APCTL_STATE_GOT_IP ? hostnet::Ipv4ToString(g_apctlLease.ip) : ""; }
ApctlLease ApctlCurrentLease() { return g_apctlState == APCTL_STATE_GOT_IP ? g_apctlLease : ApctlLease{}; }

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
		// Unnamed exports the XMB's update_plugin and libdnas import.
		HLEFN(0x5705F6F9, sceNetStrcat),
		HLEFN(0x57CC57DA, sceNetMemset),
		HLEFN(0x750F705D, sceNetCtype),
		HLEFN(0x96EF9DA1, sceNetTolower),
		HLEFN(0xEF097716, sceNetMemcpy),
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

	// pspnet_apctl.prx's other libraries (the XMB's netconf_plugin imports these): the same
	// functions under the names the firmware exports them with.
	static const hle::HleFunction sceNetApctlLib[] = {
		HLEFN(0x0C7FFA5C, sceNetApctlGetBSSDescIDListUser),  // sceNetApctlGetBSSDescIDList
		HLEFN(0x7CFAB990, sceNetApctlAddInternalHandler),
		HLEFN(0x96BEB231, sceNetApctlGetBSSDescEntryUser),   // sceNetApctlGetBSSDescEntry
		HLEFN(0xB3CF6849, sceNetApctlScanUser),              // sceNetApctlScan
		HLEFN(0xE11BAFAB, sceNetApctlDelInternalHandler),
		HLESTUB(0xA9721D11, "sceNetApctl_lib_A9721D11"),
		HLESTUB(0xC519EDCD, "sceNetApctl_lib_C519EDCD"),
	};
	hle::RegisterModule("sceNetApctl_lib", sceNetApctlLib);
	static const hle::HleFunction sceNetApctlInternalUser[] = {
		HLEFN(0x04776994, sceNetApctlGetBSSDescEntryUser),
		HLEFN(0x6BDDCB8C, sceNetApctlGetBSSDescIDListUser),
		HLEFN(0xE9B2E5E6, sceNetApctlScanUser),
	};
	hle::RegisterModule("sceNetApctl_internal_user", sceNetApctlInternalUser);
	static const hle::HleFunction sceNetApctlLib2[] = {
		HLESTUB(0x3895B348, "sceNetApctl_lib2_3895B348"),
		HLEFN(0x69745F0A, sceNetApctl_lib2_69745F0A),
		HLEFN(0xC20A144C, sceNetApctlConnectWithMac),
		HLEFN(0x4C19731F, sceNetApctlGetInfo),
	};
	hle::RegisterModule("sceNetApctl_lib2", sceNetApctlLib2);

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
