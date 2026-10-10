// SPDX-License-Identifier: GPL-2.0-or-later
//
// The ad hoc libraries with networking off (pspc.ini network=0): the libraries initialize and keep
// their bookkeeping (handlers, matching contexts, the ad hoc ID), but the WLAN switch is off:
// creating, joining or scanning for a network fails with WLAN_SWITCH_OFF, the state stays
// disconnected, and peer / scan lists are empty. This is PSPC's behaviour from before ad hoc play
// existed, kept as it was; the HLE entry points in net_adhoc*.cpp call it when !net::Enabled().
//
// Structure layouts and error codes from PPSSPP (Core/HLE/sceNetAdhoc.cpp, sceNetAdhocMatching.cpp,
// proAdhoc.h), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include <algorithm>
#include <map>
#include <string>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/modules/net_adhoc_internal.h"
#include "runtime/kernel/errors.h"

namespace net::adhoc::offline {

namespace {

constexpr const char *LOG = "NET";

constexpr u32 ADHOCCTL_NICKNAME_LEN = 128;
constexpr u32 ADHOCCTL_ADHOCID_LEN = 9;
constexpr int MAX_ADHOCCTL_HANDLERS = 32;
constexpr int ADHOCCTL_STATE_DISCONNECTED = 0;

#pragma pack(push, 1)
struct AdhocctlAdhocId {
	s32 type;
	u8 data[ADHOCCTL_ADHOCID_LEN];
	u8 padding[3];
};
#pragma pack(pop)
static_assert(sizeof(AdhocctlAdhocId) == 16, "SceNetAdhocctlAdhocId");

bool g_adhocInited = false;
bool g_adhocctlInited = false;
AdhocctlAdhocId g_adhocId{};
std::map<int, std::pair<u32, u32>> g_adhocctlHandlers;  // id -> (entry, arg)

struct MatchingContext {
	int mode = 0, maxPeers = 0, port = 0, rxBufLen = 0;
	u32 helloInt = 0, keepAliveInt = 0, initCount = 0, rexmtInt = 0, handler = 0;
	bool running = false;
	std::string helloOpt;
};

bool g_matchingInited = false;
std::map<int, MatchingContext> g_matching;

MatchingContext *GetMatching(int id, int *err) {
	if (!g_matchingInited) {
		*err = (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
		return nullptr;
	}
	auto it = g_matching.find(id);
	if (it == g_matching.end()) {
		*err = (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
		return nullptr;
	}
	return &it->second;
}

}  // namespace

// ================================================================ sceNetAdhoc

int AdhocInit() {
	if (g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_ALREADY_INITIALIZED;
	g_adhocInited = true;
	return 0;
}
int AdhocTerm() {
	g_adhocInited = false;
	return 0;
}

// Sockets live on the ad hoc network interface, which never comes up: there is no address to
// bind to.
int PdpCreate(u32 macAddr, int port, int bufferSize, u32 flag) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6) || port < 0 || port > 65535 || bufferSize <= 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
}
int PtpOpen(u32 srcMac, u32 dstMac) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(srcMac, 6) || !mem::IsValidRange(dstMac, 6)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
}
int PtpListen(u32 srcMac) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(srcMac, 6)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
}

// No socket can exist, so every socket ID is invalid.
int SocketCall(int id) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
}

int PollSocket(u32 sdsAddr, int nsds) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (nsds <= 0 || !mem::IsValidRange(sdsAddr, (u32)nsds * 8)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
}

// Socket lists: empty.
int GetSocketStat(u32 sizeAddr) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	GWrite32(sizeAddr, 0);
	return 0;
}

// Game mode needs an ad hoc game-mode network.
int GameModeCreate() {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_IN_GAMEMODE;
}
int GameModeOther() {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
}

// ================================================================ sceNetAdhocctl

int CtlInit(int stackSize, int prio, u32 adhocIdAddr) {
	if (g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_ALREADY_INITIALIZED;
	g_adhocId = AdhocctlAdhocId{};
	if (mem::IsValidRange(adhocIdAddr, sizeof(AdhocctlAdhocId))) mem::ReadBytes(&g_adhocId, adhocIdAddr, sizeof(g_adhocId));
	g_adhocctlInited = true;
	LOGI(LOG, "sceNetAdhocctlInit(%d, %d, %.9s)", stackSize, prio, (const char *)g_adhocId.data);
	return 0;
}

int CtlTerm() {
	g_adhocctlHandlers.clear();
	g_adhocctlInited = false;
	return 0;
}

int CtlAddHandler(u32 handler, u32 arg) {
	if (!mem::IsValidAddress(handler)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	for (auto &h : g_adhocctlHandlers) {
		if (h.second.first == handler) return h.first;
	}
	if ((int)g_adhocctlHandlers.size() >= MAX_ADHOCCTL_HANDLERS) return (int)SCE_NET_ADHOCCTL_ERROR_TOO_MANY_HANDLERS;
	int id = 0;
	while (g_adhocctlHandlers.count(id)) id++;
	g_adhocctlHandlers[id] = {handler, arg};
	return id;
}

int CtlDelHandler(u32 id) {
	if (!g_adhocctlHandlers.erase((int)id)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return 0;
}

// Anything that brings up a network needs the radio.
int CtlNeedsRadio(const char *what) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	LOGI(LOG, "%s: WLAN switch is off", what);
	return (int)SCE_NET_ADHOCCTL_ERROR_WLAN_SWITCH_OFF;
}

int CtlDisconnect() {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	return 0;
}
int CtlExitGameMode() {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOCCTL_ERROR_NOT_ENTER_GAMEMODE;
}

int CtlGetState(u32 stateAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(stateAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	GWrite32(stateAddr, ADHOCCTL_STATE_DISCONNECTED);
	return 0;
}

int CtlGetAdhocId(u32 addr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(addr, sizeof(AdhocctlAdhocId))) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	mem::WriteBytes(addr, &g_adhocId, sizeof(g_adhocId));
	return 0;
}

// Not in a network: no parameters to report.
int CtlGetParameter(u32 paramAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidAddress(paramAddr)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOCCTL_ERROR_DISCONNECTED;
}

int CtlGetGameModeInfo(u32 infoAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidAddress(infoAddr)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOCCTL_ERROR_NOT_ENTER_GAMEMODE;
}

// Lists of peers / networks found: empty.
int CtlEmptyList(u32 sizeAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	GWrite32(sizeAddr, 0);
	return 0;
}
int CtlGetAddrByName(u32 nicknameAddr, u32 sizeAddr) {
	if (!mem::IsValidAddress(nicknameAddr)) return g_adhocctlInited ? (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG : (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	return CtlEmptyList(sizeAddr);
}

int CtlGetPeerInfo(u32 macAddr, int size, u32 infoAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6) || size <= 0 || !mem::IsValidAddress(infoAddr)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_NO_ENTRY;
}

// Only our own address has a name.
int CtlGetNameByAddr(u32 macAddr, u32 nicknameAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6) || !mem::IsValidRange(nicknameAddr, ADHOCCTL_NICKNAME_LEN)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	if (memcmp(GPtr(macAddr), net::kLocalMac, 6) != 0) return (int)SCE_NET_ADHOC_ERROR_NO_ENTRY;
	mem::Memset(nicknameAddr, 0, ADHOCCTL_NICKNAME_LEN);
	mem::WriteBytes(nicknameAddr, net::kNickname, (u32)strlen(net::kNickname));
	return 0;
}

// ================================================================ sceNetAdhocMatching
// Contexts can be created and started; with nobody around no hello ever arrives, so there are no
// targets to select or send to.

int MatchingInit(u32 poolSize) {
	if (g_matchingInited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_INITIALIZED;
	g_matchingInited = true;
	return 0;
}
int MatchingTerm() {
	g_matching.clear();
	g_matchingInited = false;
	return 0;
}

int MatchingCreate(int mode, int maxPeers, int port, int rxBufLen, u32 helloInt, u32 keepAliveInt, u32 initCount,
                   u32 rexmtInt, u32 handler) {
	if (!g_matchingInited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (maxPeers < 2 || maxPeers > 16) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_MAXNUM;
	if (rxBufLen < 1) return (int)SCE_NET_ADHOC_MATCHING_ERROR_RXBUF_TOO_SHORT;
	if (mode < 1 || mode > 3) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	for (auto &m : g_matching) {
		if (m.second.port == port) return (int)SCE_NET_ADHOC_MATCHING_ERROR_PORT_IN_USE;
	}
	int id = 1;
	while (g_matching.count(id)) id++;
	MatchingContext c;
	c.mode = mode;
	c.maxPeers = maxPeers;
	c.port = port;
	c.rxBufLen = rxBufLen;
	c.helloInt = helloInt;
	c.keepAliveInt = keepAliveInt;
	c.initCount = initCount;
	c.rexmtInt = rexmtInt;
	c.handler = handler;
	g_matching[id] = c;
	return id;
}

int MatchingStart(int id, u32 optLen, u32 optAddr) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_IS_RUNNING;
	if ((int)optLen < 0 || (optLen && !mem::IsValidRange(optAddr, optLen))) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
	c->helloOpt.assign(optLen ? (const char *)GPtr(optAddr) : "", optLen);
	c->running = true;
	return 0;
}

int MatchingStop(int id) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	c->running = false;
	return 0;
}
int MatchingDelete(int id) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_IS_RUNNING;
	g_matching.erase(id);
	return 0;
}

int MatchingTarget(int id, u32 macAddr) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (!mem::IsValidRange(macAddr, 6)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	if (!c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
	return (int)SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
}

int MatchingSetHelloOpt(int id, int optLen, u32 optAddr) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (optLen < 0 || (optLen > 0 && !mem::IsValidRange(optAddr, (u32)optLen))) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
	c->helloOpt.assign(optLen > 0 ? (const char *)GPtr(optAddr) : "", (size_t)std::max(optLen, 0));
	return 0;
}
int MatchingGetHelloOpt(int id, u32 optLenAddr, u32 optAddr) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (!mem::IsValidRange(optLenAddr, 4)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	u32 room = GRead32(optLenAddr);
	u32 n = std::min<u32>(room, (u32)c->helloOpt.size());
	if (n && mem::IsValidRange(optAddr, n)) mem::WriteBytes(optAddr, c->helloOpt.data(), n);
	GWrite32(optLenAddr, (u32)c->helloOpt.size());
	return 0;
}

// The member list holds only ourselves while running (8-byte entries: next pointer, MAC, padding).
int MatchingGetMembers(int id, u32 sizeAddr, u32 bufAddr) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (!c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	constexpr u32 kEntry = 12;
	u32 room = GRead32(sizeAddr);
	if (bufAddr && room >= kEntry && mem::IsValidRange(bufAddr, kEntry)) {
		GWrite32(bufAddr, 0);
		mem::WriteBytes(bufAddr + 4, net::kLocalMac, 6);
		GWrite16(bufAddr + 10, 0);
	}
	GWrite32(sizeAddr, kEntry);
	return 0;
}

int MatchingGetPoolMaxAlloc() { return g_matchingInited ? 0 : (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED; }
int MatchingGetPoolStat(u32 statAddr) {
	if (!g_matchingInited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(statAddr, 12)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	// pool size, maximum used, free
	GWrite32(statAddr, 0x20000);
	GWrite32(statAddr + 4, 0);
	GWrite32(statAddr + 8, 0x20000);
	return 0;
}

}  // namespace net::adhoc::offline
