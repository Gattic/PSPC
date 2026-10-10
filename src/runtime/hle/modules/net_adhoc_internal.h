// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared pieces of the ad hoc libraries (net_adhoc*.cpp):
//   net_adhoc.cpp           sceNetAdhoc (PDP over UDP, PTP over TCP, polling, game mode data),
//                           sceNetAdhocDiscover, the NID tables
//   net_adhocctl.cpp        sceNetAdhocctl and the connection to the ad hoc server
//   net_adhoc_matching.cpp  sceNetAdhocMatching
//   net_adhoc_offline.cpp   all of them with networking off (network=0)
//   net_adhoc_selftest.cpp  the ad hoc scenarios of `pspc --net-selftest`
//
// All the state lives on the emulation thread; only the ad hoc server connection runs on a host
// thread of its own (net_adhocctl.cpp), which talks to the rest through sched::PostFromHost.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/common.h"
#include "net/adhoc_proto.h"
#include "runtime/hle/modules/net_internal.h"

namespace net::adhoc {

// Game data goes to the peer's IP at PSP port + this offset (PPSSPP's default PortOffset, so PSPC and
// PPSSPP players can play together).
constexpr u16 kPortOffset = 10000;
constexpr int kMaxSockets = 255;

// ---------------------------------------------------------------- MAC addresses

// Equal, ignoring the first byte (Gran Turismo changes its unicast/multicast bit).
bool MacMatch(const u8 *a, const u8 *b);
bool IsLocalMac(const u8 *mac);
bool IsZeroMac(const u8 *mac);
bool IsBroadcastMac(const u8 *mac);
std::string MacStr(const u8 *mac);

// ---------------------------------------------------------------- the group (net_adhocctl.cpp)

enum CtlMode : int {
	CTLMODE_NONE = -1,     // nothing created / connected / joined / scanned yet
	CTLMODE_NORMAL = 0,
	CTLMODE_GAMEMODE = 1,
};

// A player in the same ad hoc group, as the ad hoc server announced it.
struct Peer {
	u8 mac[6] = {};
	u8 nickname[128] = {};
	u32 ip = 0;         // network byte order
	u64 lastRecv = 0;   // sched::NowUs of the last sign of life; 0 = gone (kept for the matching)
};
// Newest first, like PPSSPP's friends list.
std::vector<Peer> &Peers();
Peer *FindPeer(const u8 *mac);
// MAC of the player at `ip` (ourselves included). False if unknown.
bool ResolveIp(u32 ip, u8 *mac);
// IP of the player with `mac` (ourselves included). False if unknown.
bool ResolveMac(const u8 *mac, u32 *ip);
// Data from this player arrived: it is alive (unless the server said it left).
void TouchPeer(const u8 *mac);
int ActivePeerCount(bool excludeTimedOut = true);

// The ad hoc interface's address (network byte order): 127.0.0.<instance> with a local ad hoc server,
// else the address the connection to the server goes out from.
u32 LocalIp();
bool CtlInited();
int CtlCurrentMode();
// The state of sceNetAdhocctl (net::ADHOCCTL_STATE_*).
int CtlState();
void CtlShutdown();

// Game mode: the members' MACs (host first), and whether the group was entered in game mode.
std::vector<std::array<u8, 6>> &GameModeMacs();
bool GameModeEntered();

void RegisterAdhocctlLibrary();
// sceNetAdhoc's NID table lists sceNetAdhocctlConnect too.
int AdhocctlConnectFromAdhocLib(u32 groupAddr);

// ---------------------------------------------------------------- sockets (net_adhoc.cpp)

bool AdhocInited();
// Internal entry points for the matching library and game mode (non-blocking only).
int PdpCreateInternal(const u8 *mac, int port, int bufferSize, u32 flag);
int PdpSendInternal(int id, const u8 *mac, u16 port, const void *data, int len);
// Returns 0 with the datagram, SCE_NET_ADHOC_ERROR_WOULD_BLOCK, or another error.
int PdpRecvInternal(int id, u8 *mac, u16 *port, void *buf, int *len);
int PdpDeleteInternal(int id);
bool PdpWritable(int id);
bool PdpReadable(int id);
WSAEVENT SocketEvent(int id);
int SetSocketAlertInternal(int id, s32 flags);
void AdhocShutdownSockets();

// Game mode data (master / replicas).
void GameModeDeleteMaster();   // blocks until the synchronizer stopped, like the firmware
void GameModeDeleteBuffers();
void GameModeCloseSocket();
// A member left (the server said so): its replica gets no more data.
void GameModePeerLeft(const u8 *mac);

// ---------------------------------------------------------------- matching (net_adhoc_matching.cpp)

void MatchingShutdown();
void RegisterMatchingLibrary();

// ---------------------------------------------------------------- networking off (net_adhoc_offline.cpp)

namespace offline {
int AdhocInit();
int AdhocTerm();
int PdpCreate(u32 macAddr, int port, int bufferSize, u32 flag);
int PtpOpen(u32 srcMac, u32 dstMac);
int PtpListen(u32 srcMac);
int SocketCall(int id);
int PollSocket(u32 sdsAddr, int nsds);
int GetSocketStat(u32 sizeAddr);
int GameModeCreate();
int GameModeOther();

int CtlInit(int stackSize, int prio, u32 adhocIdAddr);
int CtlTerm();
int CtlAddHandler(u32 handler, u32 arg);
int CtlDelHandler(u32 id);
int CtlNeedsRadio(const char *what);
int CtlDisconnect();
int CtlExitGameMode();
int CtlGetState(u32 stateAddr);
int CtlGetAdhocId(u32 addr);
int CtlGetParameter(u32 paramAddr);
int CtlGetGameModeInfo(u32 infoAddr);
int CtlEmptyList(u32 sizeAddr);
int CtlGetAddrByName(u32 nicknameAddr, u32 sizeAddr);
int CtlGetPeerInfo(u32 macAddr, int size, u32 infoAddr);
int CtlGetNameByAddr(u32 macAddr, u32 nicknameAddr);

int MatchingInit(u32 poolSize);
int MatchingTerm();
int MatchingCreate(int mode, int maxPeers, int port, int rxBufLen, u32 helloInt, u32 keepAliveInt, u32 initCount,
                   u32 rexmtInt, u32 handler);
int MatchingStart(int id, u32 optLen, u32 optAddr);
int MatchingStop(int id);
int MatchingDelete(int id);
int MatchingTarget(int id, u32 macAddr);
int MatchingSetHelloOpt(int id, int optLen, u32 optAddr);
int MatchingGetHelloOpt(int id, u32 optLenAddr, u32 optAddr);
int MatchingGetMembers(int id, u32 sizeAddr, u32 bufAddr);
int MatchingGetPoolMaxAlloc();
int MatchingGetPoolStat(u32 statAddr);
}  // namespace offline

}  // namespace net::adhoc
