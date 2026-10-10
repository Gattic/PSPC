// SPDX-License-Identifier: GPL-2.0-or-later
//
// The ad hoc scenarios of `pspc --net-selftest` (see net_selftest.h):
//
//   adhoc:host     instance 1: creates group PSPCTEST, hosts a matching context, receives / answers
//                  PDP datagrams and accepts a PTP connection, waits for the client to leave
//   adhoc:client   instance 2: scans, joins the group from the scan results, joins the host's
//                  matching context, sends PDP datagrams and opens a PTP connection
//   adhoc-gamemode:host / adhoc-gamemode:client
//                  game mode: the host creates group PSPCGAME for two, the client finds it and joins;
//                  both create their master data and a replica of the other's, which blocks until
//                  the first data arrived, then exchange an update
//   adhoc-dialog   one instance: the Netconf dialog (sceUtilityNetconf, "create ad hoc") connects
//   adhoc-noserver no ad hoc server listening: Init still returns, Create gives up with BUSY after
//                  5 s, Disconnect recovers; nothing hangs
//   adhoc-offline  network=0: everything that needs the radio fails with WLAN_SWITCH_OFF
//
// tools/net_selftest.py runs host and client against `pspc --adhoc-server` on a loopback port. The
// two programs synchronise through the network operations themselves (blocking calls with time-outs,
// retries), and check the adhocctl handler events, peer lists, scan info, name <-> address lookups,
// matching events and members, socket lists, polling, time-outs and non-blocking calls on the way.
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "allegrex/guestmem.h"
#include "runtime/core/memory.h"
#include "runtime/hle/modules/net_adhoc_internal.h"
#include "runtime/hle/modules/net_selftest.h"
#include "runtime/kernel/errors.h"

namespace net {

namespace {

using namespace net::selftest;

constexpr const char *NET = "sceNet";
constexpr const char *ADHOC = "sceNetAdhoc";
constexpr const char *CTL = "sceNetAdhocctl";
constexpr const char *MATCH = "sceNetAdhocMatching";
constexpr const char *GROUP = "PSPCTEST";
constexpr const char *PRODUCT = "PSPC00001";

constexpr u16 MATCHING_PORT = 1;
constexpr u16 HOST_PDP_PORT = 100;
constexpr u16 CLIENT_PDP_PORT = 101;
constexpr u16 HOST_PTP_PORT = 200;

enum { EV_CONNECT = 1, EV_DISCONNECT = 2, EV_SCAN = 3, EV_GAME = 4 };
constexpr const char *GM_GROUP = "PSPCGAME";
constexpr u32 GM_SIZE = 32;
enum { M_HELLO = 1, M_REQUEST = 2, M_ACCEPT = 6, M_ESTABLISHED = 7, M_DATA = 11, M_DATA_ACK = 12 };

using Mac = std::array<u8, 6>;

struct CtlEvent {
	u32 event, error, arg;
};
struct MatchEvent {
	int id, event;
	Mac mac;
	std::string opt;
};

struct State {
	std::vector<CtlEvent> ctl;
	std::vector<MatchEvent> match;
	Mac me{};
};

std::string MacStr(const Mac &m) { return adhoc::MacStr(m.data()); }

Mac ReadMac(u32 addr) {
	Mac m;
	mem::ReadBytes(m.data(), addr, 6);
	return m;
}

u32 MacArg(const Mac &m) {
	u32 a = Alloc(8);
	mem::WriteBytes(a, m.data(), 6);
	return a;
}

u32 Bytes(const std::string &s) {
	u32 a = Alloc((u32)s.size() + 1);
	mem::WriteBytes(a, s.data(), (u32)s.size());
	return a;
}

// Polls `done` every 10 ms (other threads run meanwhile) until it holds or `timeoutMs` passes.
template <typename F>
bool WaitUntil(F done, u32 timeoutMs) {
	u64 start = NowUs();
	while (!done()) {
		if (NowUs() - start > (u64)timeoutMs * 1000) return false;
		Delay(10000);
	}
	return true;
}

bool HasCtlEvent(State &st, u32 ev) {
	for (auto &e : st.ctl)
		if (e.event == ev) return true;
	return false;
}

const MatchEvent *FindMatchEvent(State &st, int ev) {
	for (auto &e : st.match)
		if (e.event == ev) return &e;
	return nullptr;
}

int CtlState() {
	u32 p = Alloc(4);
	Call(CTL, 0x75ECD386, {p});
	return (int)GRead32(p);
}

// The peer list: MACs of the players listed.
std::vector<Mac> PeerList(Checker *ck = nullptr) {
	u32 size = Alloc(4);
	Call(CTL, 0xE162CB14, {size, 0});
	u32 bytes = GRead32(size);
	std::vector<Mac> out;
	if (!bytes) return out;
	u32 buf = Alloc(bytes);
	GWrite32(size, bytes);
	Call(CTL, 0xE162CB14, {size, buf});
	u32 n = GRead32(size) / 152;
	for (u32 i = 0; i < n; i++) {
		u32 e = buf + i * 152;
		out.push_back(ReadMac(e + 4 + 128));
		if (ck && i + 1 < n) ck->Check(GRead32(e) == e + 152, "peer list entry %u links to the next", i);
	}
	return out;
}

// Common start: sceNet, sceNetAdhoc, sceNetAdhocctl with an event recorder.
bool Up(Checker &ck, std::shared_ptr<State> st) {
	Call("sceUtility", 0x1579A159, {1});
	Call("sceUtility", 0x1579A159, {2});
	ck.Check(Call(NET, 0x39AF39A6, {0x20000, 0x20, 0x1000, 0x20, 0x1000}) == 0, "sceNetInit");
	ck.Check(Call(ADHOC, 0xE1D621D7, {}) == 0, "sceNetAdhocInit");
	u32 macAddr = Alloc(8);
	Call(NET, 0x0BF0A3AE, {macAddr});
	st->me = ReadMac(macAddr);
	Print("  my MAC %s\n", MacStr(st->me).c_str());
	u32 adhocId = Alloc(16);
	mem::WriteBytes(adhocId + 4, PRODUCT, 9);
	u64 t0 = NowUs();
	int r = (int)Call(CTL, 0xE26F226E, {0x2000, 0x30, adhocId});
	u64 initMs = (NowUs() - t0) / 1000;
	ck.Check(r == 0, "sceNetAdhocctlInit -> %08x (after %llu ms)", (u32)r, (unsigned long long)initMs);
	u32 handler = Callback([st](Ctx *c) {
		st->ctl.push_back(CtlEvent{c->r[4], c->r[5], c->r[6]});
		Print("  adhocctl handler: event %u, error %08x, arg %08x\n", c->r[4], c->r[5], c->r[6]);
		c->r[2] = 0;
	});
	int hid = (int)Call(CTL, 0x20B317A0, {handler, 0xAD0C});
	ck.Check(hid >= 0, "sceNetAdhocctlAddHandler -> %d", hid);
	u32 idBack = Alloc(16);
	Call(CTL, 0x362CBE8F, {idBack});
	ck.Check(!memcmp(GPtr(idBack + 4), PRODUCT, 9), "sceNetAdhocctlGetAdhocId gives the product code back");
	ck.Check(CtlState() == 0, "state DISCONNECTED after init");
	return r == 0;
}

void Down(Checker &ck, std::shared_ptr<State> st) {
	st->ctl.clear();
	ck.Check(Call(CTL, 0x34401D65, {}) == 0, "sceNetAdhocctlDisconnect");
	ck.Check(WaitUntil([&] { return HasCtlEvent(*st, EV_DISCONNECT); }, 5000), "handler: DISCONNECT event");
	ck.Check(WaitUntil([] { return CtlState() == 0; }, 2000), "state DISCONNECTED");
	ck.Check(Call(CTL, 0x9D689E13, {}) == 0, "sceNetAdhocctlTerm");
	ck.Check(Call(ADHOC, 0xA62C6F57, {}) == 0, "sceNetAdhocTerm");
	ck.Check(Call(NET, 0x281928A9, {}) == 0, "sceNetTerm");
}

u32 MatchingHandler(std::shared_ptr<State> st) {
	return Callback([st](Ctx *c) {
		MatchEvent e;
		e.id = (int)c->r[4];
		e.event = (int)c->r[5];
		e.mac = ReadMac(c->r[6]);
		int optlen = (int)c->r[7];
		if (optlen > 0) e.opt.assign((const char *)GPtr(c->r[8]), (size_t)optlen);
		Print("  matching handler: id %d, event %d, %s, opt \"%s\"\n", e.id, e.event, MacStr(e.mac).c_str(), e.opt.c_str());
		st->match.push_back(e);
		c->r[2] = 0;
	});
}

int MatchingCreate(int mode, u32 handler) {
	// mode, max peers, port, rx buffer, hello 200 ms, keep-alive 500 ms, count 10, resend 200 ms
	return (int)Call(MATCH, 0xCA5EDA6F, {(u32)mode, 2, MATCHING_PORT, 2048, 200000, 500000, 10, 200000, handler});
}

int MemberCount(int id) {
	u32 size = Alloc(4);
	Call(MATCH, 0xC58BCD9E, {(u32)id, size, 0});
	return (int)GRead32(size) / 12;
}

// A guest thread counting while the test thread is blocked in a call.
struct Ticker {
	std::shared_ptr<int> count = std::make_shared<int>(0);
	std::shared_ptr<bool> stop = std::make_shared<bool>(false);
	void Start() {
		auto c = count;
		auto s = stop;
		u32 entry = Callback([c, s](Ctx *ctx) {
			while (!*s) {
				(*c)++;
				Delay(10000);
			}
			ctx->r[2] = 0;
		});
		int th = (int)Call("ThreadManForUser", 0x446D8DE6, {Str("ticker"), entry, 0x20, 0x4000, 0, 0});
		if (th > 0) Call("ThreadManForUser", 0xF475845D, {(u32)th, 0, 0});
	}
};

// ================================================================ host

int HostScenario(Checker &ck) {
	auto st = std::make_shared<State>();
	if (!Up(ck, st)) return ck.failures;

	// ---- create the group
	int r = (int)Call(CTL, 0xEC0635C1, {Str(GROUP)});
	ck.Check(r == 0, "sceNetAdhocctlCreate(%s) -> %08x", GROUP, (u32)r);
	ck.Check(WaitUntil([&] { return HasCtlEvent(*st, EV_CONNECT); }, 10000), "handler: CONNECT event");
	ck.Check(CtlState() == 1, "state CONNECTED");
	u32 param = Alloc(148);
	Call(CTL, 0xDED9D28E, {param});
	ck.Check(!strncmp((const char *)GPtr(param + 4), GROUP, 8), "parameter: group %.8s", (const char *)GPtr(param + 4));
	ck.Check(ReadMac(param + 140) == st->me, "parameter: BSSID is ours (we founded the group)");
	r = (int)Call(CTL, 0xEC0635C1, {Str(GROUP)});
	ck.Check(r == 0, "Create again while connected -> 0 (handlers get ALREADY_CONNECTED)");

	// ---- wait for the client
	Print("  waiting for the client to join...\n");
	std::vector<Mac> peers;
	ck.Check(WaitUntil([&] { return !(peers = PeerList()).empty(); }, 40000), "a peer joined");
	if (peers.empty()) return ck.failures;
	Mac client = peers[0];
	Print("  client %s\n", MacStr(client).c_str());
	PeerList(&ck);
	u32 info = Alloc(152);
	ck.Check(Call(CTL, 0x8DB83FDC, {MacArg(client), 152, info}) == 0 && ReadMac(info + 132) == client, "sceNetAdhocctlGetPeerInfo(client)");
	u32 name = Alloc(128);
	r = (int)Call(CTL, 0x8916C003, {MacArg(client), name});
	std::string clientName = ReadStr(name, 128);
	ck.Check(r == 0 && !clientName.empty(), "sceNetAdhocctlGetNameByAddr(client) = \"%s\"", clientName.c_str());
	u32 size = Alloc(4);
	Call(CTL, 0x99560ABE, {Str(ReadStr(name, 128)), size, 0});
	ck.Check(GRead32(size) >= 152, "sceNetAdhocctlGetAddrByName(\"%s\") finds %u player(s)", ReadStr(name, 128).c_str(), GRead32(size) / 152);
	Mac stranger = {0x00, 0x16, 0xFE, 0x99, 0x99, 0x99};
	ck.Check((u32)Call(CTL, 0x8DB83FDC, {MacArg(stranger), 152, info}) == SCE_NET_ADHOC_ERROR_NO_ENTRY, "GetPeerInfo(stranger) -> NO_ENTRY");

	// ---- matching: we are the parent
	ck.Check(Call(MATCH, 0x2A2A1E07, {0x20000}) == 0, "sceNetAdhocMatchingInit");
	int mid = MatchingCreate(1, MatchingHandler(st));
	ck.Check(mid > 0, "sceNetAdhocMatchingCreate(parent) -> %d", mid);
	std::string hello = "HOSTHELLO";
	ck.Check(Call(MATCH, 0x93EF3843, {(u32)mid, 0x30, 0x2000, 0x30, 0x2000, (u32)hello.size(), Bytes(hello)}) == 0, "sceNetAdhocMatchingStart");
	u32 optLen = Alloc(4), opt = Alloc(64);
	Call(MATCH, 0xB5D96C2A, {(u32)mid, optLen, opt});
	ck.Check(GRead32(optLen) == hello.size() && !memcmp(GPtr(opt), hello.data(), hello.size()), "sceNetAdhocMatchingGetHelloOpt");
	const MatchEvent *req = nullptr;
	ck.Check(WaitUntil([&] { return (req = FindMatchEvent(*st, M_REQUEST)) != nullptr; }, 30000), "matching: REQUEST from the client");
	if (!req) return ck.failures;
	ck.Check(req->mac == client && req->opt == "JOINOPT", "REQUEST from %s with \"%s\"", MacStr(req->mac).c_str(), req->opt.c_str());
	std::string acc = "WELCOME";
	r = (int)Call(MATCH, 0x5E3D4B79, {(u32)mid, MacArg(client), (u32)acc.size(), Bytes(acc)});
	ck.Check(r == 0, "sceNetAdhocMatchingSelectTarget(client) -> %08x", (u32)r);
	ck.Check(WaitUntil([&] { return FindMatchEvent(*st, M_ESTABLISHED) != nullptr; }, 10000), "matching: ESTABLISHED");
	const MatchEvent *data = nullptr;
	ck.Check(WaitUntil([&] { return (data = FindMatchEvent(*st, M_DATA)) != nullptr; }, 15000), "matching: DATA from the client");
	if (data) ck.Check(data->opt == "CLIENTDATA", "DATA \"%s\"", data->opt.c_str());
	std::string hd = "HOSTDATA";
	ck.Check(Call(MATCH, 0xF79472D7, {(u32)mid, MacArg(client), (u32)hd.size(), Bytes(hd)}) == 0, "sceNetAdhocMatchingSendData");
	ck.Check(WaitUntil([&] { return FindMatchEvent(*st, M_DATA_ACK) != nullptr; }, 5000), "matching: DATA_ACK");
	ck.Check(MemberCount(mid) == 2, "sceNetAdhocMatchingGetMembers: 2 members (%d)", MemberCount(mid));

	// ---- PDP
	u32 mymac = MacArg(st->me);
	int pdp = (int)Call(ADHOC, 0x6F92741B, {mymac, HOST_PDP_PORT, 0x1000, 0});
	ck.Check(pdp > 0, "sceNetAdhocPdpCreate(%u) -> %d", HOST_PDP_PORT, pdp);
	ck.Check((u32)Call(ADHOC, 0x6F92741B, {mymac, HOST_PDP_PORT, 0x1000, 0}) == SCE_NET_ADHOC_ERROR_PORT_IN_USE, "the same PDP port again -> PORT_IN_USE");
	u32 smac = Alloc(8), sport = Alloc(4), buf = Alloc(256), len = Alloc(4);
	GWrite32(len, 256);
	ck.Check((u32)Call(ADHOC, 0xDFE53E03, {(u32)pdp, smac, sport, buf, len, 0, 1}) == SCE_NET_ADHOC_ERROR_WOULD_BLOCK, "non-blocking PdpRecv -> WOULD_BLOCK");
	Ticker ticker;
	ticker.Start();
	Print("  waiting for the client's datagram...\n");
	GWrite32(len, 256);
	u64 t0 = NowUs();
	r = (int)Call(ADHOC, 0xDFE53E03, {(u32)pdp, smac, sport, buf, len, 30000000, 0});
	*ticker.stop = true;
	ck.Check(r == 0, "blocking PdpRecv -> %08x after %llu ms", (u32)r, (unsigned long long)((NowUs() - t0) / 1000));
	ck.Check(r == 0 && ReadStr(buf, GRead32(len)) == "pdp-ping" && ReadMac(smac) == client && GRead16(sport) == CLIENT_PDP_PORT,
	         "datagram \"%s\" from %s port %u", ReadStr(buf, GRead32(len)).c_str(), MacStr(ReadMac(smac)).c_str(), GRead16(sport));
	ck.Check(*ticker.count > 0, "another guest thread ran meanwhile (%d ticks)", *ticker.count);
	std::string pong = "pdp-pong";
	ck.Check(Call(ADHOC, 0xABED3790, {(u32)pdp, MacArg(client), CLIENT_PDP_PORT, Bytes(pong), (u32)pong.size(), 0, 0}) == 0, "PdpSend reply");
	u32 statSize = Alloc(4);
	Call(ADHOC, 0xC7C1FC57, {statSize, 0});
	ck.Check(GRead32(statSize) == 2 * 20, "sceNetAdhocGetPdpStat: 2 PDP sockets (matching + ours): %u bytes", GRead32(statSize));

	// ---- PTP: listen, poll, accept, receive, answer
	int lsock = (int)Call(ADHOC, 0xE08BDAC1, {mymac, HOST_PTP_PORT, 0x2000, 200000, 10, 1, 0});
	ck.Check(lsock > 0, "sceNetAdhocPtpListen(%u) -> %d", HOST_PTP_PORT, lsock);
	u32 pmac = Alloc(8), pport = Alloc(4);
	ck.Check((u32)Call(ADHOC, 0x9DF81198, {(u32)lsock, pmac, pport, 0, 1}) == SCE_NET_ADHOC_ERROR_WOULD_BLOCK, "non-blocking PtpAccept -> WOULD_BLOCK");
	u32 sds = Alloc(12);
	GWrite32(sds, (u32)lsock);
	GWrite32(sds + 4, 0x0008);  // ADHOC_EV_ACCEPT
	Print("  waiting for the client's PTP connection...\n");
	r = (int)Call(ADHOC, 0x7A662D6B, {sds, 1, 30000000, 0});
	ck.Check(r == 1 && (GRead32(sds + 8) & 0x0008), "sceNetAdhocPollSocket(ACCEPT) -> %d, revents %04x", r, GRead32(sds + 8));
	int csock = (int)Call(ADHOC, 0x9DF81198, {(u32)lsock, pmac, pport, 5000000, 0});
	ck.Check(csock > 0 && ReadMac(pmac) == client, "sceNetAdhocPtpAccept -> %d from %s port %u", csock, MacStr(ReadMac(pmac)).c_str(), GRead16(pport));
	GWrite32(len, 256);
	r = (int)Call(ADHOC, 0x8BEA2B3E, {(u32)csock, buf, len, 10000000, 0});
	ck.Check(r == 0 && ReadStr(buf, GRead32(len)) == "ptp-hello", "PtpRecv \"%s\"", r == 0 ? ReadStr(buf, GRead32(len)).c_str() : "");
	std::string reply = "ptp-reply";
	GWrite32(len, (u32)reply.size());
	ck.Check(Call(ADHOC, 0x4DA4C788, {(u32)csock, Bytes(reply), len, 0, 0}) == 0 && GRead32(len) == reply.size(), "PtpSend reply");
	Call(ADHOC, 0xB9685118, {statSize, 0});
	u32 ptpStat = Alloc(GRead32(statSize) + 4);
	Call(ADHOC, 0xB9685118, {statSize, ptpStat});
	bool sawEstablished = false;
	for (u32 i = 0; i < GRead32(statSize) / 36; i++)
		if ((int)GRead32(ptpStat + i * 36 + 4) == csock && GRead32(ptpStat + i * 36 + 32) == 4) sawEstablished = true;
	ck.Check(sawEstablished, "sceNetAdhocGetPtpStat: the accepted socket is ESTABLISHED");
	// The client closes its end: our receive sees the end of the stream.
	GWrite32(len, 256);
	r = (int)Call(ADHOC, 0x8BEA2B3E, {(u32)csock, buf, len, 10000000, 0});
	ck.Check((u32)r == SCE_NET_ADHOC_ERROR_DISCONNECTED, "PtpRecv after the client closed -> DISCONNECTED (%08x)", (u32)r);
	ck.Check(Call(ADHOC, 0x157E6225, {(u32)csock, 0}) == 0 && Call(ADHOC, 0x157E6225, {(u32)lsock, 0}) == 0, "PtpClose");
	ck.Check(Call(ADHOC, 0x7F27BB5E, {(u32)pdp, 0}) == 0, "PdpDelete");

	// ---- the client leaves the group: the server tells us
	Print("  waiting for the client to leave...\n");
	ck.Check(WaitUntil([] { return PeerList().empty(); }, 30000), "the client left (peer list empty)");
	ck.Check(Call(MATCH, 0x32B156B3, {(u32)mid}) == 0 && Call(MATCH, 0xF16EAF4F, {(u32)mid}) == 0 && Call(MATCH, 0x7945ECDA, {}) == 0,
	         "matching stop / delete / term");
	Down(ck, st);
	return ck.failures;
}

// ================================================================ client

int ClientScenario(Checker &ck) {
	auto st = std::make_shared<State>();
	if (!Up(ck, st)) return ck.failures;

	// ---- find the host's group
	Mac host{};
	u32 scanInfo = 0;
	u64 start = NowUs();
	while (NowUs() - start < 40000000) {
		st->ctl.clear();
		int r = (int)Call(CTL, 0x08FFF7A0, {});
		if (r != 0) {
			ck.Check(false, "sceNetAdhocctlScan -> %08x", (u32)r);
			return ck.failures;
		}
		if (!WaitUntil([&] { return HasCtlEvent(*st, EV_SCAN); }, 5000)) {
			ck.Check(false, "handler: SCAN event");
			return ck.failures;
		}
		WaitUntil([] { return CtlState() == 0; }, 2000);
		u32 size = Alloc(4);
		Call(CTL, 0x81AEE1BE, {size, 0});
		u32 bytes = GRead32(size);
		if (bytes) {
			u32 buf = Alloc(bytes);
			GWrite32(size, bytes);
			Call(CTL, 0x81AEE1BE, {size, buf});
			for (u32 i = 0; i < GRead32(size) / 28; i++) {
				u32 e = buf + i * 28;
				if (!strncmp((const char *)GPtr(e + 8), GROUP, 8)) {
					scanInfo = e;
					host = ReadMac(e + 16);
				}
			}
		}
		if (scanInfo) break;
		Delay(500000);  // the host hasn't created the group yet
	}
	if (!ck.Check(scanInfo != 0, "scan found group %s, host %s", GROUP, MacStr(host).c_str())) return ck.failures;
	ck.Check(GRead32(scanInfo + 4) != 0, "scan info: channel %u", GRead32(scanInfo + 4));

	// ---- join it
	st->ctl.clear();
	int r = (int)Call(CTL, 0x5E7F79C9, {scanInfo});
	ck.Check(r == 0, "sceNetAdhocctlJoin -> %08x", (u32)r);
	ck.Check(WaitUntil([&] { return HasCtlEvent(*st, EV_CONNECT); }, 10000), "handler: CONNECT event");
	ck.Check(CtlState() == 1, "state CONNECTED");
	std::vector<Mac> peers = PeerList(&ck);
	ck.Check(peers.size() == 1 && peers[0] == host, "peer list: the host (%zu peer(s))", peers.size());
	u32 param = Alloc(148);
	Call(CTL, 0xDED9D28E, {param});
	ck.Check(ReadMac(param + 140) == host, "parameter: BSSID is the host's");

	// ---- matching: join the host
	ck.Check(Call(MATCH, 0x2A2A1E07, {0x20000}) == 0, "sceNetAdhocMatchingInit");
	int mid = MatchingCreate(2, MatchingHandler(st));
	ck.Check(mid > 0, "sceNetAdhocMatchingCreate(child) -> %d", mid);
	ck.Check(Call(MATCH, 0x93EF3843, {(u32)mid, 0x30, 0x2000, 0x30, 0x2000, 0, 0}) == 0, "sceNetAdhocMatchingStart");
	const MatchEvent *hello = nullptr;
	ck.Check(WaitUntil([&] { return (hello = FindMatchEvent(*st, M_HELLO)) != nullptr; }, 15000), "matching: HELLO from the host");
	if (!hello) return ck.failures;
	ck.Check(hello->mac == host && hello->opt == "HOSTHELLO", "HELLO from %s with \"%s\"", MacStr(hello->mac).c_str(), hello->opt.c_str());
	std::string join = "JOINOPT";
	r = (int)Call(MATCH, 0x5E3D4B79, {(u32)mid, MacArg(host), (u32)join.size(), Bytes(join)});
	ck.Check(r == 0, "sceNetAdhocMatchingSelectTarget(host) -> %08x", (u32)r);
	ck.Check((u32)Call(MATCH, 0x5E3D4B79, {(u32)mid, MacArg(host), 0, 0}) == SCE_NET_ADHOC_MATCHING_ERROR_REQUEST_IN_PROGRESS,
	         "SelectTarget again -> REQUEST_IN_PROGRESS");
	const MatchEvent *accept = nullptr;
	ck.Check(WaitUntil([&] { return (accept = FindMatchEvent(*st, M_ACCEPT)) != nullptr; }, 15000), "matching: ACCEPT");
	if (accept) ck.Check(accept->opt == "WELCOME", "ACCEPT with \"%s\"", accept->opt.c_str());
	ck.Check(WaitUntil([&] { return FindMatchEvent(*st, M_ESTABLISHED) != nullptr; }, 5000), "matching: ESTABLISHED");
	size_t acceptAt = 0, establishedAt = 0;
	for (size_t i = 0; i < st->match.size(); i++) {
		if (st->match[i].event == M_ACCEPT && !acceptAt) acceptAt = i + 1;
		if (st->match[i].event == M_ESTABLISHED && !establishedAt) establishedAt = i + 1;
	}
	ck.Check(acceptAt && acceptAt < establishedAt, "ACCEPT comes before ESTABLISHED");
	std::string cd = "CLIENTDATA";
	ck.Check(Call(MATCH, 0xF79472D7, {(u32)mid, MacArg(host), (u32)cd.size(), Bytes(cd)}) == 0, "sceNetAdhocMatchingSendData");
	const MatchEvent *data = nullptr;
	ck.Check(WaitUntil([&] { return (data = FindMatchEvent(*st, M_DATA)) != nullptr; }, 15000), "matching: DATA from the host");
	if (data) ck.Check(data->opt == "HOSTDATA", "DATA \"%s\"", data->opt.c_str());
	ck.Check(MemberCount(mid) == 2, "sceNetAdhocMatchingGetMembers: 2 members (%d)", MemberCount(mid));

	// ---- PDP: ping until the host answers
	u32 mymac = MacArg(st->me);
	int pdp = (int)Call(ADHOC, 0x6F92741B, {mymac, CLIENT_PDP_PORT, 0x1000, 0});
	ck.Check(pdp > 0, "sceNetAdhocPdpCreate(%u) -> %d", CLIENT_PDP_PORT, pdp);
	u32 smac = Alloc(8), sport = Alloc(4), buf = Alloc(256), len = Alloc(4);
	GWrite32(len, 256);
	u64 t0 = NowUs();
	r = (int)Call(ADHOC, 0xDFE53E03, {(u32)pdp, smac, sport, buf, len, 200000, 0});
	u64 waited = NowUs() - t0;
	ck.Check((u32)r == SCE_NET_ADHOC_ERROR_TIMEOUT && waited >= 150000, "PdpRecv with a 200 ms time-out -> TIMEOUT after %llu ms",
	         (unsigned long long)(waited / 1000));
	std::string ping = "pdp-ping";
	bool answered = false;
	start = NowUs();
	while (!answered && NowUs() - start < 30000000) {
		r = (int)Call(ADHOC, 0xABED3790, {(u32)pdp, MacArg(host), HOST_PDP_PORT, Bytes(ping), (u32)ping.size(), 0, 0});
		if (r != 0) {
			ck.Check(false, "PdpSend -> %08x", (u32)r);
			break;
		}
		GWrite32(len, 256);
		r = (int)Call(ADHOC, 0xDFE53E03, {(u32)pdp, smac, sport, buf, len, 500000, 0});
		answered = r == 0;
	}
	ck.Check(answered && ReadStr(buf, GRead32(len)) == "pdp-pong" && ReadMac(smac) == host && GRead16(sport) == HOST_PDP_PORT,
	         "PDP answer \"%s\" from %s port %u", answered ? ReadStr(buf, GRead32(len)).c_str() : "", MacStr(ReadMac(smac)).c_str(), GRead16(sport));

	// ---- PTP: connect (the host may not listen yet: the connect keeps trying), send, receive
	int sock = (int)Call(ADHOC, 0x877F6D66, {mymac, 0, MacArg(host), HOST_PTP_PORT, 0x2000, 200000, 10, 0});
	ck.Check(sock > 0, "sceNetAdhocPtpOpen(-> %u) -> %d", HOST_PTP_PORT, sock);
	r = (int)Call(ADHOC, 0xFC6FC07B, {(u32)sock, 30000000, 0});
	ck.Check(r == 0, "blocking sceNetAdhocPtpConnect -> %08x", (u32)r);
	std::string hi = "ptp-hello";
	GWrite32(len, (u32)hi.size());
	ck.Check(Call(ADHOC, 0x4DA4C788, {(u32)sock, Bytes(hi), len, 0, 0}) == 0, "PtpSend");
	ck.Check(Call(ADHOC, 0x9AC2EEAC, {(u32)sock, 0, 0}) == 0, "PtpFlush");
	GWrite32(len, 256);
	r = (int)Call(ADHOC, 0x8BEA2B3E, {(u32)sock, buf, len, 10000000, 0});
	ck.Check(r == 0 && ReadStr(buf, GRead32(len)) == "ptp-reply", "PtpRecv \"%s\"", r == 0 ? ReadStr(buf, GRead32(len)).c_str() : "");
	ck.Check(Call(ADHOC, 0x157E6225, {(u32)sock, 0}) == 0, "PtpClose");
	ck.Check(Call(ADHOC, 0x7F27BB5E, {(u32)pdp, 0}) == 0, "PdpDelete");
	ck.Check((u32)Call(ADHOC, 0x7F27BB5E, {(u32)pdp, 0}) == SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID, "PdpDelete again -> INVALID_SOCKET_ID");

	Delay(500000);  // let the host see the end of the PTP stream first
	ck.Check(Call(MATCH, 0x32B156B3, {(u32)mid}) == 0 && Call(MATCH, 0xF16EAF4F, {(u32)mid}) == 0 && Call(MATCH, 0x7945ECDA, {}) == 0,
	         "matching stop / delete / term");
	Down(ck, st);
	return ck.failures;
}

// ================================================================ game mode

// Scans until `group` shows up; returns its host's MAC (zero if not found).
Mac ScanFor(Checker &ck, std::shared_ptr<State> st, const char *group) {
	u64 start = NowUs();
	while (NowUs() - start < 40000000) {
		st->ctl.clear();
		if (Call(CTL, 0x08FFF7A0, {}) != 0 || !WaitUntil([&] { return HasCtlEvent(*st, EV_SCAN); }, 5000)) break;
		WaitUntil([] { return CtlState() == 0; }, 2000);
		u32 size = Alloc(4);
		Call(CTL, 0x81AEE1BE, {size, 0});
		u32 bytes = GRead32(size);
		if (bytes) {
			u32 buf = Alloc(bytes);
			GWrite32(size, bytes);
			Call(CTL, 0x81AEE1BE, {size, buf});
			for (u32 i = 0; i < GRead32(size) / 28; i++)
				if (!strncmp((const char *)GPtr(buf + i * 28 + 8), group, 8)) return ReadMac(buf + i * 28 + 16);
		}
		Delay(500000);
	}
	ck.Check(false, "scan found group %s", group);
	return Mac{};
}

// Both sides once the group is entered: master + replica (blocks until synced), one update each way.
void GameModeExchange(Checker &ck, std::shared_ptr<State> st, const Mac &other, const std::string &tag) {
	u32 info = Alloc(100);
	int r = (int)Call(CTL, 0x5A014CE0, {info});
	u32 members = GRead32(info);
	ck.Check(r == 0 && members == 2, "sceNetAdhocctlGetGameModeInfo: %u members", members);
	u32 master = Alloc(GM_SIZE), replica = Alloc(GM_SIZE);
	mem::WriteBytes(master, ("MASTER-" + tag).c_str(), 9);
	r = (int)Call(ADHOC, 0x7F75C338, {master, GM_SIZE});
	ck.Check(r == 0, "sceNetAdhocGameModeCreateMaster -> %08x", (u32)r);
	u64 t0 = NowUs();
	int rid = (int)Call(ADHOC, 0x3278AB0C, {MacArg(other), replica, GM_SIZE});
	ck.Check(rid > 0, "sceNetAdhocGameModeCreateReplica -> %d (synced after %llu ms)", rid, (unsigned long long)((NowUs() - t0) / 1000));
	u32 ui = Alloc(16);
	ck.Check(Call(ADHOC, 0xFA324B4E, {(u32)rid, ui}) == 0 && GRead32(ui + 4) == 1, "sceNetAdhocGameModeUpdateReplica: updated");
	std::string otherTag = tag == "H" ? "C" : "H";
	// The other side's first data, or already its update (a replica holds the latest data).
	std::string first = ReadStr(replica, 9);
	ck.Check(first == "MASTER-" + otherTag || first == "UPDATE-" + otherTag, "replica holds \"%s\"", first.c_str());
	mem::WriteBytes(master, ("UPDATE-" + tag).c_str(), 9);
	ck.Check(Call(ADHOC, 0x98C204C8, {}) == 0, "sceNetAdhocGameModeUpdateMaster");
	bool updated = WaitUntil([&] {
		Call(ADHOC, 0xFA324B4E, {(u32)rid, ui});
		return ReadStr(replica, 9) == "UPDATE-" + otherTag;
	}, 10000);
	ck.Check(updated, "the other side's update arrived (\"%s\")", ReadStr(replica, 9).c_str());
	Delay(500000);  // let our update reach the other side too
	ck.Check(Call(ADHOC, 0x0B2228E9, {(u32)rid}) == 0, "sceNetAdhocGameModeDeleteReplica");
	ck.Check(Call(ADHOC, 0xA0229362, {}) == 0, "sceNetAdhocGameModeDeleteMaster");
	st->ctl.clear();
	ck.Check(Call(CTL, 0xCF8E084D, {}) == 0, "sceNetAdhocctlExitGameMode");
	ck.Check(WaitUntil([&] { return HasCtlEvent(*st, EV_DISCONNECT); }, 5000), "handler: DISCONNECT event");
	ck.Check(WaitUntil([] { return CtlState() == 0; }, 2000), "state DISCONNECTED");
}

int GameModeHost(Checker &ck) {
	auto st = std::make_shared<State>();
	if (!Up(ck, st)) return ck.failures;
	// The members' MACs: only their number matters here (the client's isn't known in advance).
	u32 macs = Alloc(12);
	mem::WriteBytes(macs, st->me.data(), 6);
	int r = (int)Call(CTL, 0xA5C055CE, {Str(GM_GROUP), 1, 2, macs, 30000000, 0});
	ck.Check(r == 0, "sceNetAdhocctlCreateEnterGameMode(%s, 2 players) -> %08x", GM_GROUP, (u32)r);
	Print("  waiting for the client...\n");
	ck.Check(WaitUntil([&] { return HasCtlEvent(*st, EV_GAME); }, 40000), "handler: GAME event");
	ck.Check(CtlState() == 3, "state GAMEMODE");
	std::vector<Mac> peers = PeerList();
	if (!ck.Check(peers.size() == 1, "one peer")) return ck.failures;
	GameModeExchange(ck, st, peers[0], "H");
	ck.Check(Call(CTL, 0x9D689E13, {}) == 0 && Call(ADHOC, 0xA62C6F57, {}) == 0 && Call(NET, 0x281928A9, {}) == 0, "terms");
	return ck.failures;
}

int GameModeClient(Checker &ck) {
	auto st = std::make_shared<State>();
	if (!Up(ck, st)) return ck.failures;
	Mac host = ScanFor(ck, st, GM_GROUP);
	if (adhoc::IsZeroMac(host.data())) return ck.failures;
	Print("  host %s\n", MacStr(host).c_str());
	st->ctl.clear();
	int r = (int)Call(CTL, 0x1FF89745, {Str(GM_GROUP), MacArg(host), 30000000, 0});
	ck.Check(r == 0, "sceNetAdhocctlJoinEnterGameMode -> %08x", (u32)r);
	ck.Check(WaitUntil([&] { return HasCtlEvent(*st, EV_GAME); }, 15000), "handler: GAME event");
	ck.Check(CtlState() == 3, "state GAMEMODE");
	u32 info = Alloc(100);
	Call(CTL, 0x5A014CE0, {info});
	ck.Check(ReadMac(info + 4) == host && ReadMac(info + 10) == st->me, "game mode members: host first, then us");
	GameModeExchange(ck, st, host, "C");
	ck.Check(Call(CTL, 0x9D689E13, {}) == 0 && Call(ADHOC, 0xA62C6F57, {}) == 0 && Call(NET, 0x281928A9, {}) == 0, "terms");
	return ck.failures;
}

int GameModeScenario(const std::vector<std::string> &args) {
	Checker ck;
	std::string role = args.empty() ? "" : args[0];
	if (role == "host") return GameModeHost(ck);
	if (role == "client") return GameModeClient(ck);
	Print("adhoc-gamemode: give the role, adhoc-gamemode:host or adhoc-gamemode:client\n");
	return 1;
}

int AdhocScenario(const std::vector<std::string> &args) {
	Checker ck;
	std::string role = args.empty() ? "" : args[0];
	if (role == "host") return HostScenario(ck);
	if (role == "client") return ClientScenario(ck);
	Print("adhoc: give the role, adhoc:host or adhoc:client\n");
	return 1;
}

// network=0: the radio is off.
// The Netconf dialog creating group PSPCDLG (it doesn't block the caller; it finishes once connected).
int DialogScenario(const std::vector<std::string> &) {
	Checker ck;
	auto st = std::make_shared<State>();
	if (!Up(ck, st)) return ck.failures;
	u32 data = Alloc(12);  // SceUtilityNetconfData: group name, time-out
	mem::WriteBytes(data, "PSPCDLG", 7);
	u32 param = Alloc(0x44);
	GWrite32(param, 0x44);
	GWrite32(param + 48, 4);  // netAction: create an ad hoc group
	GWrite32(param + 52, data);
	int r = (int)Call("sceUtility", 0x4DB1E739, {param});
	ck.Check(r == 0, "sceUtilityNetconfInitStart(create ad hoc) -> %08x", (u32)r);
	u64 start = NowUs();
	int status = 0;
	while (NowUs() - start < 20000000) {
		status = (int)Call("sceUtility", 0x6332AA39, {});
		if (status == 2) Call("sceUtility", 0x91E70E35, {1});
		else if (status == 3) Call("sceUtility", 0xF88155F6, {});
		else if (status == 0 && NowUs() - start > 100000) break;
		Delay(16666);
	}
	ck.Check(status == 0, "the dialog finished (status %d)", status);
	ck.Check(GRead32(param + 28) == 0, "dialog result %08x (0 = connected)", GRead32(param + 28));
	ck.Check(CtlState() == 1, "state CONNECTED");
	ck.Check(HasCtlEvent(*st, EV_CONNECT), "handler: CONNECT event");
	u32 p = Alloc(148);
	Call(CTL, 0xDED9D28E, {p});
	ck.Check(!strncmp((const char *)GPtr(p + 4), "PSPCDLG", 8), "parameter: group %.8s", (const char *)GPtr(p + 4));
	Down(ck, st);
	return ck.failures;
}

// The ad hoc server is down: nothing hangs, the calls fail the way PPSSPP's do.
int NoServerScenario(const std::vector<std::string> &) {
	Checker ck;
	auto st = std::make_shared<State>();
	u64 t0 = NowUs();
	Up(ck, st);
	ck.Check(NowUs() - t0 < 7000000, "sceNetAdhocctlInit returned after %llu ms", (unsigned long long)((NowUs() - t0) / 1000));
	t0 = NowUs();
	int r = (int)Call(CTL, 0xEC0635C1, {Str(GROUP)});
	u64 took = NowUs() - t0;
	ck.Check((u32)r == SCE_NET_ADHOCCTL_ERROR_BUSY && took >= 4500000 && took < 8000000, "Create -> %08x (BUSY) after %llu ms", (u32)r,
	         (unsigned long long)(took / 1000));
	ck.Check(CtlState() == 0, "state DISCONNECTED");
	ck.Check(PeerList().empty(), "no peers");
	Down(ck, st);
	return ck.failures;
}

int OfflineScenario(const std::vector<std::string> &) {
	Checker ck;
	ck.Check(Call(NET, 0x39AF39A6, {0x20000, 0x20, 0x1000, 0x20, 0x1000}) == 0, "sceNetInit");
	ck.Check(Call(ADHOC, 0xE1D621D7, {}) == 0, "sceNetAdhocInit");
	ck.Check(Call(CTL, 0xE26F226E, {0x2000, 0x30, 0}) == 0, "sceNetAdhocctlInit");
	ck.Check((u32)Call(CTL, 0xEC0635C1, {Str(GROUP)}) == SCE_NET_ADHOCCTL_ERROR_WLAN_SWITCH_OFF, "Create -> WLAN_SWITCH_OFF");
	ck.Check((u32)Call(CTL, 0x08FFF7A0, {}) == SCE_NET_ADHOCCTL_ERROR_WLAN_SWITCH_OFF, "Scan -> WLAN_SWITCH_OFF");
	ck.Check(CtlState() == 0, "state DISCONNECTED");
	u32 mac = Alloc(8);
	ck.Check((u32)Call(ADHOC, 0x6F92741B, {mac, 1, 0x1000, 0}) == SCE_NET_ADHOC_ERROR_INVALID_ADDR, "PdpCreate -> INVALID_ADDR");
	ck.Check(PeerList().empty(), "no peers");
	ck.Check(Call(CTL, 0x9D689E13, {}) == 0 && Call(ADHOC, 0xA62C6F57, {}) == 0 && Call(NET, 0x281928A9, {}) == 0, "terms");
	return ck.failures;
}

}  // namespace

void RegisterAdhocSelftests() {
	selftest::Register("adhoc", "ad hoc play with a second instance: adhoc:host / adhoc:client (tools/net_selftest.py)", AdhocScenario);
	selftest::Register("adhoc-gamemode", "ad hoc game mode with a second instance: adhoc-gamemode:host / :client", GameModeScenario);
	selftest::Register("adhoc-dialog", "the Netconf dialog creates an ad hoc group (needs an ad hoc server)", DialogScenario);
	selftest::Register("adhoc-noserver", "ad hoc with no ad hoc server listening", NoServerScenario);
	selftest::Register("adhoc-offline", "ad hoc libraries with network=0", OfflineScenario);
}

}  // namespace net
