// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ad hoc networking, offline: sceNetAdhoc, sceNetAdhocctl, sceNetAdhocMatching, sceNetAdhocDiscover.
// The libraries initialize and keep their bookkeeping (handlers, matching contexts, the ad hoc ID),
// but the WLAN switch is off: creating, joining or scanning for a network fails with
// WLAN_SWITCH_OFF, the state stays disconnected, and peer / scan lists are empty.
//
// NIDs, structure layouts and error codes from PPSSPP (Core/HLE/sceNetAdhoc.cpp,
// sceNetAdhocMatching.cpp, proAdhoc.h), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. None of
// PPSSPP's ad hoc server / relay code is used.
#include <algorithm>
#include <map>
#include <string>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/kernel/errors.h"

namespace {

constexpr const char *LOG = "NET";

constexpr u32 ADHOCCTL_GROUPNAME_LEN = 8;
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

// ================================================================ sceNetAdhoc

bool g_adhocInited = false;

int sceNetAdhocInit() {
	if (g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_ALREADY_INITIALIZED;
	g_adhocInited = true;
	return 0;
}
int sceNetAdhocTerm() {
	g_adhocInited = false;
	return 0;
}

// Sockets live on the ad hoc network interface, which never comes up: there is no address to
// bind to.
int sceNetAdhocPdpCreate(u32 macAddr, int port, int bufferSize, u32 flag) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6) || port < 0 || port > 65535 || bufferSize <= 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
}
int sceNetAdhocPtpOpen(u32 srcMac, int sport, u32 dstMac, int dport, int bufsize, int rexmtInt, int rexmtCnt, int flag) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(srcMac, 6) || !mem::IsValidRange(dstMac, 6)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
}
int sceNetAdhocPtpListen(u32 srcMac, int sport, int bufsize, int rexmtInt, int rexmtCnt, int backlog, int flag) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(srcMac, 6)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
}

// No socket can exist, so every socket ID is invalid.
int AdhocSocketCall(int id) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
}
int sceNetAdhocPdpSend(int id, u32, u32, u32, int, int, int) { return AdhocSocketCall(id); }
int sceNetAdhocPdpRecv(int id, u32, u32, u32, u32, u32, int) { return AdhocSocketCall(id); }
int sceNetAdhocPdpDelete(int id, int) { return AdhocSocketCall(id); }
int sceNetAdhocPtpClose(int id, int) { return AdhocSocketCall(id); }
int sceNetAdhocPtpSend(int id, u32, u32, int, int) { return AdhocSocketCall(id); }
int sceNetAdhocPtpRecv(int id, u32, u32, int, int) { return AdhocSocketCall(id); }
int sceNetAdhocPtpAccept(int id, u32, u32, int, int) { return AdhocSocketCall(id); }
int sceNetAdhocPtpConnect(int id, int, int) { return AdhocSocketCall(id); }
int sceNetAdhocPtpFlush(int id, int, int) { return AdhocSocketCall(id); }
int sceNetAdhocSetSocketAlert(int id, int) { return AdhocSocketCall(id); }
int sceNetAdhocGetSocketAlert(int id, u32) { return AdhocSocketCall(id); }

int sceNetAdhocPollSocket(u32 sdsAddr, int nsds, int timeout, int flags) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (nsds <= 0 || !mem::IsValidRange(sdsAddr, (u32)nsds * 8)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
}

// Socket lists: empty.
int GetSocketStat(u32 sizeAddr, u32 bufAddr) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	GWrite32(sizeAddr, 0);
	return 0;
}
int sceNetAdhocGetPdpStat(u32 sizeAddr, u32 bufAddr) { return GetSocketStat(sizeAddr, bufAddr); }
int sceNetAdhocGetPtpStat(u32 sizeAddr, u32 bufAddr) { return GetSocketStat(sizeAddr, bufAddr); }

// Game mode needs an ad hoc game-mode network.
int sceNetAdhocGameModeCreateMaster(u32 data, int size) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_IN_GAMEMODE;
}
int sceNetAdhocGameModeCreateReplica(u32 mac, u32 data, int size) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_IN_GAMEMODE;
}
int sceNetAdhocGameModeUpdateMaster() {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
}
int sceNetAdhocGameModeUpdateReplica(int id, u32) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
}
int sceNetAdhocGameModeDeleteMaster() {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
}
int sceNetAdhocGameModeDeleteReplica(int id) {
	if (!g_adhocInited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
}

// ================================================================ sceNetAdhocctl

bool g_adhocctlInited = false;
AdhocctlAdhocId g_adhocId{};
std::map<int, std::pair<u32, u32>> g_adhocctlHandlers;  // id -> (entry, arg)

int sceNetAdhocctlInit(int stackSize, int prio, u32 adhocIdAddr) {
	if (g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_ALREADY_INITIALIZED;
	g_adhocId = AdhocctlAdhocId{};
	if (mem::IsValidRange(adhocIdAddr, sizeof(AdhocctlAdhocId))) mem::ReadBytes(&g_adhocId, adhocIdAddr, sizeof(g_adhocId));
	g_adhocctlInited = true;
	LOGI(LOG, "sceNetAdhocctlInit(%d, %d, %.9s)", stackSize, prio, (const char *)g_adhocId.data);
	return 0;
}

int sceNetAdhocctlTerm() {
	g_adhocctlHandlers.clear();
	g_adhocctlInited = false;
	return 0;
}

int sceNetAdhocctlAddHandler(u32 handler, u32 arg) {
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

int sceNetAdhocctlDelHandler(u32 id) {
	if (!g_adhocctlHandlers.erase((int)id)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return 0;
}

// Anything that brings up a network needs the radio.
int AdhocctlNeedsRadio(const char *what) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	LOGI(LOG, "%s: WLAN switch is off", what);
	return (int)SCE_NET_ADHOCCTL_ERROR_WLAN_SWITCH_OFF;
}
int sceNetAdhocctlConnect(u32) { return AdhocctlNeedsRadio("sceNetAdhocctlConnect"); }
int sceNetAdhocctlCreate(u32) { return AdhocctlNeedsRadio("sceNetAdhocctlCreate"); }
int sceNetAdhocctlJoin(u32) { return AdhocctlNeedsRadio("sceNetAdhocctlJoin"); }
int sceNetAdhocctlScan() { return AdhocctlNeedsRadio("sceNetAdhocctlScan"); }
int sceNetAdhocctlCreateEnterGameMode(u32, int, int, u32, u32, int) { return AdhocctlNeedsRadio("sceNetAdhocctlCreateEnterGameMode"); }
int sceNetAdhocctlCreateEnterGameModeMin(u32, int, int, int, u32, u32, int) {
	return AdhocctlNeedsRadio("sceNetAdhocctlCreateEnterGameModeMin");
}
int sceNetAdhocctlJoinEnterGameMode(u32, u32, u32, int) { return AdhocctlNeedsRadio("sceNetAdhocctlJoinEnterGameMode"); }

int sceNetAdhocctlDisconnect() {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	return 0;
}
int sceNetAdhocctlExitGameMode() {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	return (int)SCE_NET_ADHOCCTL_ERROR_NOT_ENTER_GAMEMODE;
}

int sceNetAdhocctlGetState(u32 stateAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(stateAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	GWrite32(stateAddr, ADHOCCTL_STATE_DISCONNECTED);
	return 0;
}

int sceNetAdhocctlGetAdhocId(u32 addr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(addr, sizeof(AdhocctlAdhocId))) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	mem::WriteBytes(addr, &g_adhocId, sizeof(g_adhocId));
	return 0;
}

// Not in a network: no parameters to report.
int sceNetAdhocctlGetParameter(u32 paramAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidAddress(paramAddr)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOCCTL_ERROR_DISCONNECTED;
}

int sceNetAdhocctlGetGameModeInfo(u32 infoAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidAddress(infoAddr)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOCCTL_ERROR_NOT_ENTER_GAMEMODE;
}

// Lists of peers / networks found: empty.
int EmptyList(u32 sizeAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	GWrite32(sizeAddr, 0);
	return 0;
}
int sceNetAdhocctlGetPeerList(u32 sizeAddr, u32 bufAddr) { return EmptyList(sizeAddr); }
int sceNetAdhocctlGetScanInfo(u32 sizeAddr, u32 bufAddr) { return EmptyList(sizeAddr); }
int sceNetAdhocctlGetAddrByName(u32 nicknameAddr, u32 sizeAddr, u32 bufAddr) {
	if (!mem::IsValidAddress(nicknameAddr)) return g_adhocctlInited ? (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG : (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	return EmptyList(sizeAddr);
}

int sceNetAdhocctlGetPeerInfo(u32 macAddr, int size, u32 infoAddr) {
	if (!g_adhocctlInited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6) || size <= 0 || !mem::IsValidAddress(infoAddr)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	return (int)SCE_NET_ADHOC_ERROR_NO_ENTRY;
}

// Only our own address has a name.
int sceNetAdhocctlGetNameByAddr(u32 macAddr, u32 nicknameAddr) {
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

struct MatchingContext {
	int mode = 0, maxPeers = 0, port = 0, rxBufLen = 0;
	u32 helloInt = 0, keepAliveInt = 0, initCount = 0, rexmtInt = 0, handler = 0;
	bool running = false;
	std::string helloOpt;
};

bool g_matchingInited = false;
std::map<int, MatchingContext> g_matching;

int sceNetAdhocMatchingInit(u32 poolSize) {
	if (g_matchingInited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_INITIALIZED;
	g_matchingInited = true;
	return 0;
}
int sceNetAdhocMatchingTerm() {
	g_matching.clear();
	g_matchingInited = false;
	return 0;
}

int sceNetAdhocMatchingCreate(int mode, int maxPeers, int port, int rxBufLen, u32 helloInt, u32 keepAliveInt,
                              u32 initCount, u32 rexmtInt, u32 handler) {
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

int StartMatching(int id, u32 optLen, u32 optAddr) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_IS_RUNNING;
	if ((int)optLen < 0 || (optLen && !mem::IsValidRange(optAddr, optLen))) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
	c->helloOpt.assign(optLen ? (const char *)GPtr(optAddr) : "", optLen);
	c->running = true;
	return 0;
}
int sceNetAdhocMatchingStart(int id, int evthPri, int evthStack, int inthPri, int inthStack, u32 optLen, u32 optAddr) {
	return StartMatching(id, optLen, optAddr);
}
int sceNetAdhocMatchingStart2(int id, int evthPri, int evthPartition, int evthStack, int inthPri, int inthPartition,
                              int inthStack, u32 optLen, u32 optAddr) {
	return StartMatching(id, optLen, optAddr);
}

// Nine arguments: the ninth is passed on the stack.
u32 StackArg0(Ctx *c) { return mem::IsValidRange(c->r[29], 4) ? GRead32(c->r[29]) : 0; }
void sceNetAdhocMatchingCreate_Raw(Ctx *c) {
	c->r[2] = (u32)sceNetAdhocMatchingCreate((int)c->r[4], (int)c->r[5], (int)c->r[6], (int)c->r[7], c->r[8], c->r[9],
	                                          c->r[10], c->r[11], StackArg0(c));
}
void sceNetAdhocMatchingStart2_Raw(Ctx *c) {
	c->r[2] = (u32)sceNetAdhocMatchingStart2((int)c->r[4], (int)c->r[5], (int)c->r[6], (int)c->r[7], (int)c->r[8],
	                                          (int)c->r[9], (int)c->r[10], c->r[11], StackArg0(c));
}

int sceNetAdhocMatchingStop(int id) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	c->running = false;
	return 0;
}
int sceNetAdhocMatchingDelete(int id) {
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
int sceNetAdhocMatchingSelectTarget(int id, u32 mac, int optLen, u32 opt) { return MatchingTarget(id, mac); }
int sceNetAdhocMatchingCancelTarget(int id, u32 mac) { return MatchingTarget(id, mac); }
int sceNetAdhocMatchingCancelTargetWithOpt(int id, u32 mac, int optLen, u32 opt) { return MatchingTarget(id, mac); }
int sceNetAdhocMatchingSendData(int id, u32 mac, int dataLen, u32 data) { return MatchingTarget(id, mac); }
int sceNetAdhocMatchingAbortSendData(int id, u32 mac) { return MatchingTarget(id, mac); }

int sceNetAdhocMatchingSetHelloOpt(int id, int optLen, u32 optAddr) {
	int err;
	MatchingContext *c = GetMatching(id, &err);
	if (!c) return err;
	if (optLen < 0 || (optLen > 0 && !mem::IsValidRange(optAddr, (u32)optLen))) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
	c->helloOpt.assign(optLen > 0 ? (const char *)GPtr(optAddr) : "", (size_t)std::max(optLen, 0));
	return 0;
}
int sceNetAdhocMatchingGetHelloOpt(int id, u32 optLenAddr, u32 optAddr) {
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
int sceNetAdhocMatchingGetMembers(int id, u32 sizeAddr, u32 bufAddr) {
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

int sceNetAdhocMatchingGetPoolMaxAlloc() { return g_matchingInited ? 0 : (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED; }
int sceNetAdhocMatchingGetPoolStat(u32 statAddr) {
	if (!g_matchingInited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(statAddr, 12)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	// pool size, maximum used, free
	GWrite32(statAddr, 0x20000);
	GWrite32(statAddr + 4, 0);
	GWrite32(statAddr + 8, 0x20000);
	return 0;
}

// ================================================================ sceNetAdhocDiscover
// The discovery runs and ends at once without finding anyone.

enum { DISCOVER_STATUS_NONE = 0, DISCOVER_STATUS_IN_PROGRESS = 1, DISCOVER_STATUS_COMPLETED = 2 };
enum { DISCOVER_RESULT_NO_PEER_FOUND = 0, DISCOVER_RESULT_CANCELED = 1 };
// SceNetAdhocDiscoverParam: u32 unknown1; char groupName[8]; u32 unknown2; u32 result.
constexpr u32 DISCOVER_PARAM_RESULT_OFFSET = 16;

int g_discoverStatus = DISCOVER_STATUS_NONE;
u32 g_discoverParam = 0;
bool g_discoverStopping = false;

void SetDiscoverResult(int result) {
	if (mem::IsValidRange(g_discoverParam + DISCOVER_PARAM_RESULT_OFFSET, 4)) {
		GWrite32(g_discoverParam + DISCOVER_PARAM_RESULT_OFFSET, (u32)result);
	}
}

int sceNetAdhocDiscoverInitStart(u32 paramAddr) {
	if (!mem::IsValidRange(paramAddr, 20)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	g_discoverParam = paramAddr;
	g_discoverStopping = false;
	g_discoverStatus = DISCOVER_STATUS_IN_PROGRESS;
	SetDiscoverResult(DISCOVER_RESULT_NO_PEER_FOUND);
	return 0;
}
int sceNetAdhocDiscoverUpdate() {
	if (g_discoverStatus == DISCOVER_STATUS_IN_PROGRESS) {
		g_discoverStatus = DISCOVER_STATUS_COMPLETED;
		SetDiscoverResult(g_discoverStopping ? DISCOVER_RESULT_CANCELED : DISCOVER_RESULT_NO_PEER_FOUND);
	}
	return 0;
}
int sceNetAdhocDiscoverGetStatus() { return g_discoverStatus; }
int sceNetAdhocDiscoverStop() {
	g_discoverStopping = true;
	return 0;
}
int sceNetAdhocDiscoverRequestSuspend() {
	g_discoverStopping = true;
	return 0;
}
int sceNetAdhocDiscoverTerm() {
	g_discoverStatus = DISCOVER_STATUS_NONE;
	g_discoverParam = 0;
	g_discoverStopping = false;
	return 0;
}

}  // namespace

namespace net {

void AdhocShutdown() {
	sceNetAdhocMatchingTerm();
	sceNetAdhocctlTerm();
	sceNetAdhocTerm();
	sceNetAdhocDiscoverTerm();
}

void RegisterAdhocLibraries() {
	static const hle::HleFunction sceNetAdhoc[] = {
		HLEFN(0xE1D621D7, sceNetAdhocInit),
		HLEFN(0xA62C6F57, sceNetAdhocTerm),
		HLEFN(0x0AD043ED, sceNetAdhocctlConnect),
		HLEFN(0x6F92741B, sceNetAdhocPdpCreate),
		HLEFN(0xABED3790, sceNetAdhocPdpSend),
		HLEFN(0xDFE53E03, sceNetAdhocPdpRecv),
		HLEFN(0x7F27BB5E, sceNetAdhocPdpDelete),
		HLEFN(0xC7C1FC57, sceNetAdhocGetPdpStat),
		HLEFN(0x157E6225, sceNetAdhocPtpClose),
		HLEFN(0x4DA4C788, sceNetAdhocPtpSend),
		HLEFN(0x877F6D66, sceNetAdhocPtpOpen),
		HLEFN(0x8BEA2B3E, sceNetAdhocPtpRecv),
		HLEFN(0x9DF81198, sceNetAdhocPtpAccept),
		HLEFN(0xE08BDAC1, sceNetAdhocPtpListen),
		HLEFN(0xFC6FC07B, sceNetAdhocPtpConnect),
		HLEFN(0x9AC2EEAC, sceNetAdhocPtpFlush),
		HLEFN(0xB9685118, sceNetAdhocGetPtpStat),
		HLEFN(0x3278AB0C, sceNetAdhocGameModeCreateReplica),
		HLEFN(0x98C204C8, sceNetAdhocGameModeUpdateMaster),
		HLEFN(0xFA324B4E, sceNetAdhocGameModeUpdateReplica),
		HLEFN(0xA0229362, sceNetAdhocGameModeDeleteMaster),
		HLEFN(0x0B2228E9, sceNetAdhocGameModeDeleteReplica),
		HLEFN(0x7F75C338, sceNetAdhocGameModeCreateMaster),
		HLEFN(0x73BFD52D, sceNetAdhocSetSocketAlert),
		HLEFN(0x4D2CE199, sceNetAdhocGetSocketAlert),
		HLEFN(0x7A662D6B, sceNetAdhocPollSocket),
	};
	hle::RegisterModule("sceNetAdhoc", sceNetAdhoc);

	static const hle::HleFunction sceNetAdhocctl[] = {
		HLEFN(0xE26F226E, sceNetAdhocctlInit),
		HLEFN(0x9D689E13, sceNetAdhocctlTerm),
		HLEFN(0x20B317A0, sceNetAdhocctlAddHandler),
		HLEFN(0x6402490B, sceNetAdhocctlDelHandler),
		HLEFN(0x34401D65, sceNetAdhocctlDisconnect),
		HLEFN(0x0AD043ED, sceNetAdhocctlConnect),
		HLEFN(0x08FFF7A0, sceNetAdhocctlScan),
		HLEFN(0x75ECD386, sceNetAdhocctlGetState),
		HLEFN(0x8916C003, sceNetAdhocctlGetNameByAddr),
		HLEFN(0xDED9D28E, sceNetAdhocctlGetParameter),
		HLEFN(0x81AEE1BE, sceNetAdhocctlGetScanInfo),
		HLEFN(0x5E7F79C9, sceNetAdhocctlJoin),
		HLEFN(0x8DB83FDC, sceNetAdhocctlGetPeerInfo),
		HLEFN(0xEC0635C1, sceNetAdhocctlCreate),
		HLEFN(0xA5C055CE, sceNetAdhocctlCreateEnterGameMode),
		HLEFN(0x1FF89745, sceNetAdhocctlJoinEnterGameMode),
		HLEFN(0xCF8E084D, sceNetAdhocctlExitGameMode),
		HLEFN(0xE162CB14, sceNetAdhocctlGetPeerList),
		HLEFN(0x362CBE8F, sceNetAdhocctlGetAdhocId),
		HLEFN(0x5A014CE0, sceNetAdhocctlGetGameModeInfo),
		HLEFN(0x99560ABE, sceNetAdhocctlGetAddrByName),
		HLEFN(0xB0B80E80, sceNetAdhocctlCreateEnterGameModeMin),
	};
	hle::RegisterModule("sceNetAdhocctl", sceNetAdhocctl);

	static const hle::HleFunction sceNetAdhocMatching[] = {
		HLEFN(0x2A2A1E07, sceNetAdhocMatchingInit),
		HLEFN(0x7945ECDA, sceNetAdhocMatchingTerm),
		HLERAW(0xCA5EDA6F, sceNetAdhocMatchingCreate_Raw),
		HLEFN(0x93EF3843, sceNetAdhocMatchingStart),
		HLERAW(0xE8454C65, sceNetAdhocMatchingStart2_Raw),
		HLEFN(0x32B156B3, sceNetAdhocMatchingStop),
		HLEFN(0xF16EAF4F, sceNetAdhocMatchingDelete),
		HLEFN(0x5E3D4B79, sceNetAdhocMatchingSelectTarget),
		HLEFN(0xEA3C6108, sceNetAdhocMatchingCancelTarget),
		HLEFN(0x8F58BEDF, sceNetAdhocMatchingCancelTargetWithOpt),
		HLEFN(0xB5D96C2A, sceNetAdhocMatchingGetHelloOpt),
		HLEFN(0xB58E61B7, sceNetAdhocMatchingSetHelloOpt),
		HLEFN(0xC58BCD9E, sceNetAdhocMatchingGetMembers),
		HLEFN(0xF79472D7, sceNetAdhocMatchingSendData),
		HLEFN(0xEC19337D, sceNetAdhocMatchingAbortSendData),
		HLEFN(0x40F8F435, sceNetAdhocMatchingGetPoolMaxAlloc),
		HLEFN(0x9C5CFB7D, sceNetAdhocMatchingGetPoolStat),
	};
	hle::RegisterModule("sceNetAdhocMatching", sceNetAdhocMatching);

	static const hle::HleFunction sceNetAdhocDiscover[] = {
		HLEFN(0x941B3877, sceNetAdhocDiscoverInitStart),
		HLEFN(0x52DE1B97, sceNetAdhocDiscoverUpdate),
		HLEFN(0x944DDBC6, sceNetAdhocDiscoverGetStatus),
		HLEFN(0xA2246614, sceNetAdhocDiscoverTerm),
		HLEFN(0xF7D13214, sceNetAdhocDiscoverStop),
		HLEFN(0xA423A21B, sceNetAdhocDiscoverRequestSuspend),
	};
	hle::RegisterModule("sceNetAdhocDiscover", sceNetAdhocDiscover);
}

}  // namespace net
