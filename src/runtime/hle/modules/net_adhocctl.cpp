// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceNetAdhocctl: ad hoc groups through a "pro ad hoc server" (net/adhoc_proto.h), as PPSSPP does it.
//
// sceNetAdhocctlInit opens a TCP connection to the ad hoc server (pspc.ini adhoc_server=, default
// localhost:27312) and logs in with the console's MAC, nickname and the game's product code. Create /
// Connect / Join send the group name, Scan asks for the game's groups; the server's answers (the
// group's members, the founder's MAC, the list of groups) become the peer and scan lists, and the
// events (CONNECT, SCAN, DISCONNECT, GAME, ERROR) reach the game's handlers on the SceNetAdhocctl
// thread with the delays and state changes PPSSPP found games to need.
//
// The server connection runs on a host thread of its own (Link below): name resolution, connecting,
// pings every 2 s, sending requests in order and splitting the received stream into packets. It hands
// everything to the emulation thread with sched::PostFromHost; every message carries the connection's
// generation, so a stopped link can't disturb a newer one. The emulation thread never waits for the
// network; a guest call that has to (Init until the login, Create / Scan until the request went out)
// sleeps its own guest thread.
//
// With a local server (localhost / 127.x) this instance uses 127.0.0.<instance number> as its ad hoc
// address, binds the server connection to it (the server tells players apart by IP) and the game's
// sockets too, so several PSPC instances on one PC can play together. With a server elsewhere the
// address is the one the server connection goes out from.
//
// Behaviour ported from PPSSPP (Core/HLE/sceNetAdhoc.cpp, proAdhoc.cpp: friendFinder, __AdhocctlNotify,
// __NetTriggerCallbacks, NetAdhocctl_*), Copyright (c) 2013- PPSSPP Project, GPL-2.0-or-later,
// itself based on Coldbird's aemu.
#include "net/socket.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "common/host.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_adhoc_internal.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

namespace net::adhoc {

namespace {

using namespace adhocproto;

constexpr const char *LOG = "ADHOC";

// Timing, from PPSSPP's NetAdhocCommon.h.
constexpr u64 kDefaultTimeoutUs = 5000000;    // adhocDefaultTimeout: server requests, joining
constexpr u64 kDefaultDelayUs = 10000;        // adhocDefaultDelay
constexpr u64 kExtraDelayUs = 20000;          // adhocExtraDelay
constexpr u64 kEventPollDelayUs = 100000;     // adhocEventPollDelay
constexpr u64 kEventDelayUs = 2000000;        // adhocEventDelay
constexpr u64 kLastRecvDeltaUs = 10000;       // defaultLastRecvDelta
constexpr u64 kPingIntervalMs = 2000;         // PSP_ADHOCCTL_PING_TIMEOUT
constexpr u64 kConnectTimeoutMs = 5000;
constexpr int kDefaultChannel = 11;           // an "automatic" channel reads as 11 (JPCSP, PPSSPP)
constexpr int kMaxHandlers = 32;

enum CtlEvent : u32 {
	EVENT_ERROR = 0,
	EVENT_CONNECT = 1,
	EVENT_DISCONNECT = 2,
	EVENT_SCAN = 3,
	EVENT_GAME = 4,
	EVENT_DISCOVER = 5,
	EVENT_WOL = 6,
	EVENT_WOL_INTERRUPT = 7,
};

enum ConnType : int { CONNTYPE_CONNECT = 0, CONNTYPE_CREATE = 1, CONNTYPE_JOIN = 2 };

#pragma pack(push, 1)
struct AdhocId {
	s32 type;
	u8 data[kProductCodeLen];
	u8 padding[3];
};
struct Parameter {
	s32 channel;
	u8 group[kGroupNameLen];
	u8 nickname[kNicknameLen];
	u8 bssid[6];
	u8 padding[2];
};
struct ScanInfoEmu {
	u32 next;
	s32 channel;
	u8 group[kGroupNameLen];
	u8 bssid[6];
	u8 padding[2];
	s32 mode;
};
struct PeerInfoEmu {
	u32 next;
	u8 nickname[kNicknameLen];
	u8 mac[6];
	u16 padding;
	u32 flags;
	u64 lastRecv;
};
struct GameModeInfo {
	s32 num;
	u8 members[16][6];
};
#pragma pack(pop)
static_assert(sizeof(AdhocId) == 16, "SceNetAdhocctlAdhocId");
static_assert(sizeof(Parameter) == 148, "SceNetAdhocctlParameter");
static_assert(sizeof(ScanInfoEmu) == 28, "SceNetAdhocctlScanInfo");
static_assert(sizeof(PeerInfoEmu) == 152, "SceNetAdhocctlPeerInfo");
static_assert(sizeof(GameModeInfo) == 100, "SceNetAdhocctlGameModeInfo");

struct Handler {
	u32 entry, arg;
};

struct Network {
	u8 group[kGroupNameLen];
	u8 bssid[6];
	s32 channel;
	s32 mode;
};

// ---------------------------------------------------------------- state (emulation thread)

bool g_inited = false;
u32 g_ctlGen = 1;  // bumped by Term: queued events of an earlier session are dropped
AdhocId g_productCode{};
Parameter g_parameter{};
std::map<int, Handler> g_handlers;
int g_state = net::ADHOCCTL_STATE_DISCONNECTED;
int g_mode = CTLMODE_NONE;
int g_connType = CONNTYPE_CONNECT;
bool g_busy = false;
u64 g_startTime = 0;  // adhocctlStartTime
net::HandlerThread g_thread;
bool g_holdingRealTime = false;

std::vector<Peer> g_peers;              // newest first
std::vector<Network> g_networks;        // the last complete scan, newest first
std::vector<Network> g_newNetworks;     // the scan being received

// Game mode.
std::vector<std::array<u8, 6>> g_gmMacs, g_gmRequired;
bool g_gmEntered = false;
u64 g_gmEnterTimeoutUs = 15000000;
u64 g_gmTimeoutEvent = 0;

// The ad hoc server connection, seen from the emulation thread.
bool g_serverConnected = false;
bool g_loginFailed = false;
u32 g_localIp = 0;
bool g_localServer = true;
int g_requestsInFlight = 0;

// Guest threads sleeping until something about the server connection changes.
struct CtlWaiter {
	SceUID tid;
	SceUID waitId;
};
std::vector<CtlWaiter> g_waiters;
u32 g_nextWaitId = 1;
std::map<u32, bool> g_sentRequests;  // request id -> written to the server

// ---------------------------------------------------------------- the server connection (host thread)

struct Outgoing {
	std::string bytes;
	u32 reqId;
};

struct LinkShared {
	std::mutex mu;
	std::deque<Outgoing> queue;
	bool loginRequested = false;
	std::atomic<bool> stop{false};
	hostnet::EventHandle wake = nullptr;
	hostnet::EventHandle done = nullptr;  // set when the thread has ended
	// Fixed at start.
	u32 gen = 0;
	std::string host;
	u16 port = kServerPort;
	u32 bindIp = 0;  // network byte order; 0 = let the OS choose
	LoginC2S login{};

	~LinkShared() {
		if (wake) hostnet::CloseEvent(wake);
		if (done) hostnet::CloseEvent(done);
	}
};

std::shared_ptr<LinkShared> g_link;
u32 g_nextLinkGen = 1;

void OnLinkLogin(u32 gen, bool ok, u32 localIp);
void OnLinkPacket(u32 gen, const std::string &packet);
void OnLinkLost(u32 gen);
void OnLinkSent(u32 gen, u32 reqId);

// Hands a result to the emulation thread (nothing once the link was told to stop).
void Post(LinkShared &sh, std::function<void()> fn) {
	if (!sh.stop) sched::PostFromHost(std::move(fn));
}

// Waits on the link's wake event and (optionally) the socket's event. Returns false if stopping.
bool LinkWait(LinkShared &sh, hostnet::EventHandle sockEv, int ms) {
	hostnet::EventHandle hs[2] = {sh.wake, sockEv};
	hostnet::WaitEvents(hs, sockEv != hostnet::InvalidEvent ? 2 : 1, ms);
	if (sockEv != hostnet::InvalidEvent) hostnet::ResetEvent(sockEv);
	return !sh.stop;
}

// Resolves, connects (5 s at most) and logs in. Returns the connected socket or an invalid one.
hostnet::Socket LinkConnect(LinkShared &sh, u32 *localIp, std::string *why) {
	hostnet::Socket sock;
	u32 addr = 0;
	int rerr = hostnet::ResolveIpv4(sh.host, &addr);
	if (rerr != 0) {
		*why = "cannot resolve " + sh.host + " (" + std::to_string(rerr) + ")";
		return sock;
	}
	int err = 0;
	sock = hostnet::OpenSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &err);
	if (!sock.Valid()) {
		*why = "socket failed (" + std::to_string(err) + ")";
		return sock;
	}
	int on = 1;
	setsockopt(sock.s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
	setsockopt(sock.s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof(on));
	if (sh.bindIp) {
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = sh.bindIp;
		if (bind(sock.s, (const sockaddr *)&local, sizeof(local)) != 0) {
			*why = "cannot bind " + hostnet::Ipv4ToString(sh.bindIp) + " (" + std::to_string(hostnet::LastSocketError()) + ")";
			hostnet::CloseSocket(sock);
			return sock;
		}
	}
	sockaddr_in srv{};
	srv.sin_family = AF_INET;
	srv.sin_addr.s_addr = addr;
	srv.sin_port = htons(sh.port);
	if (connect(sock.s, (const sockaddr *)&srv, sizeof(srv)) != 0 && hostnet::LastSocketError() != hostnet::Error_EWOULDBLOCK) {
		*why = "connect failed (" + std::to_string(hostnet::LastSocketError()) + ")";
		hostnet::CloseSocket(sock);
		return sock;
	}
	u64 start = (hostsys::MonotonicUs() / 1000);
	for (;;) {
		hostnet::Readiness r;
		if (!hostnet::CheckReady(sock.s, &r) || r.except) {
			int e = hostnet::PendingError(sock.s);
			*why = "cannot connect to " + hostnet::Ipv4ToString(addr) + ":" + std::to_string(sh.port) + " (" + std::to_string(e) + ")";
			hostnet::CloseSocket(sock);
			return sock;
		}
		if (r.write) break;
		if ((hostsys::MonotonicUs() / 1000) - start > kConnectTimeoutMs) {
			*why = "connecting to " + hostnet::Ipv4ToString(addr) + ":" + std::to_string(sh.port) + " timed out";
			hostnet::CloseSocket(sock);
			return sock;
		}
		if (!LinkWait(sh, sock.ev, 50)) {
			hostnet::CloseSocket(sock);
			return sock;
		}
	}
	sockaddr_in me{};
	int len = sizeof(me);
	hostnet::GetSockName(sock.s, (sockaddr *)&me, &len);
	*localIp = me.sin_addr.s_addr;
	// The login packet is the first thing the server expects.
	const char *p = (const char *)&sh.login;
	size_t left = sizeof(sh.login);
	start = (hostsys::MonotonicUs() / 1000);
	while (left) {
		int n = hostnet::Send(sock.s, p, (int)left, 0);
		if (n > 0) {
			p += n;
			left -= (size_t)n;
			continue;
		}
		if (hostnet::LastSocketError() != hostnet::Error_EWOULDBLOCK || (hostsys::MonotonicUs() / 1000) - start > kConnectTimeoutMs || !LinkWait(sh, sock.ev, 20)) {
			*why = "cannot send the login";
			hostnet::CloseSocket(sock);
			return sock;
		}
	}
	return sock;
}

void LinkThread(std::shared_ptr<LinkShared> sh) {
	hostnet::Socket sock;
	bool connected = false;
	std::string rx;
	u64 lastPing = 0;
	u32 gen = sh->gen;
	auto lost = [&](const char *why) {
		LOGW(LOG, "Lost the ad hoc server connection (%s)", why);
		hostnet::CloseSocket(sock);
		connected = false;
		rx.clear();
		Post(*sh, [gen] { OnLinkLost(gen); });
	};
	while (!sh->stop) {
		bool wantLogin;
		{
			std::lock_guard<std::mutex> l(sh->mu);
			wantLogin = sh->loginRequested;
			sh->loginRequested = false;
		}
		if (!connected && wantLogin) {
			u32 localIp = 0;
			std::string why;
			sock = LinkConnect(*sh, &localIp, &why);
			if (sh->stop) break;
			connected = sock.Valid();
			if (connected) {
				LOGI(LOG, "Connected to the ad hoc server %s:%u from %s", sh->host.c_str(), sh->port, hostnet::Ipv4ToString(localIp).c_str());
				lastPing = (hostsys::MonotonicUs() / 1000);
			} else {
				LOGW(LOG, "Ad hoc server %s:%u: %s", sh->host.c_str(), sh->port, why.c_str());
			}
			bool ok = connected;
			Post(*sh, [gen, ok, localIp] { OnLinkLogin(gen, ok, localIp); });
		}
		int waitMs = 100;
		if (connected) {
			u64 now = (hostsys::MonotonicUs() / 1000);
			if (now - lastPing >= kPingIntervalMs) {
				u8 ping = OPCODE_PING;
				if (hostnet::Send(sock.s, (const char *)&ping, 1, 0) == 1) lastPing = now;
				else if (hostnet::LastSocketError() != hostnet::Error_EWOULDBLOCK) {
					lost("ping failed");
					continue;
				}
			}
			// Requests, in order.
			bool broken = false;
			for (;;) {
				Outgoing out;
				{
					std::lock_guard<std::mutex> l(sh->mu);
					if (sh->queue.empty()) break;
					out = sh->queue.front();
				}
				int n = hostnet::Send(sock.s, out.bytes.data(), (int)out.bytes.size(), 0);
				int e = n < 0 ? hostnet::LastSocketError() : 0;
				if (n < 0 && e == hostnet::Error_EWOULDBLOCK) break;
				{
					std::lock_guard<std::mutex> l(sh->mu);
					if (!sh->queue.empty() && sh->queue.front().reqId == out.reqId && sh->queue.front().bytes == out.bytes) {
						if (n > 0 && (size_t)n < out.bytes.size()) {
							sh->queue.front().bytes.erase(0, (size_t)n);  // the rest goes next time
							break;
						}
						sh->queue.pop_front();
					}
				}
				u32 reqId = out.reqId;
				if (reqId) Post(*sh, [gen, reqId] { OnLinkSent(gen, reqId); });
				if (n < 0) {
					broken = true;
					break;
				}
			}
			if (broken) {
				lost("send failed");
				continue;
			}
			// Server packets.
			char buf[2048];
			for (;;) {
				int n = recv(sock.s, buf, sizeof(buf), 0);
				if (n > 0) {
					rx.append(buf, (size_t)n);
					continue;
				}
				if (n < 0 && hostnet::LastSocketError() == hostnet::Error_EWOULDBLOCK) break;
				broken = true;
				break;
			}
			while (!rx.empty()) {
				size_t need = S2CPacketSize((u8)rx[0]);
				if (need == 0) {
					LOGW(LOG, "Ad hoc server sent unknown opcode %u; discarding %zu bytes", (u8)rx[0], rx.size());
					rx.clear();
					break;
				}
				if (rx.size() < need) break;
				std::string pk = rx.substr(0, need);
				rx.erase(0, need);
				if ((u8)pk[0] == OPCODE_PING) continue;
				Post(*sh, [gen, pk] { OnLinkPacket(gen, pk); });
			}
			if (broken) {
				lost("closed by the server");
				continue;
			}
			u64 sincePing = (hostsys::MonotonicUs() / 1000) - lastPing;
			waitMs = (int)std::min<u64>(100, sincePing >= kPingIntervalMs ? 1 : kPingIntervalMs - sincePing);
		}
		LinkWait(*sh, connected ? sock.ev : hostnet::InvalidEvent, waitMs);
	}
	if (connected) {
		// Last requests (a DISCONNECT from sceNetAdhocctlTerm), best effort.
		std::lock_guard<std::mutex> l(sh->mu);
		for (auto &out : sh->queue) hostnet::Send(sock.s, out.bytes.data(), (int)out.bytes.size(), 0);
		sh->queue.clear();
		shutdown(sock.s, hostnet::ShutBoth);
	}
	hostnet::CloseSocket(sock);
	hostnet::SignalEvent(sh->done);
}

void LinkSend(std::string bytes, u32 reqId) {
	if (!g_link) return;
	{
		std::lock_guard<std::mutex> l(g_link->mu);
		g_link->queue.push_back(Outgoing{std::move(bytes), reqId});
	}
	hostnet::SignalEvent(g_link->wake);
}

// Drops a request that hasn't been written yet (its caller gave up).
void LinkCancel(u32 reqId) {
	if (!g_link || !reqId) return;
	std::lock_guard<std::mutex> l(g_link->mu);
	auto &q = g_link->queue;
	q.erase(std::remove_if(q.begin(), q.end(), [reqId](const Outgoing &o) { return o.reqId == reqId; }), q.end());
}

// PPSSPP's isAdhocctlNeedLogin: (re)connect if the connection isn't up.
void LinkRequestLogin() {
	if (!g_link) return;
	{
		std::lock_guard<std::mutex> l(g_link->mu);
		g_link->loginRequested = true;
	}
	hostnet::SignalEvent(g_link->wake);
}

void ParseServer(const std::string &server, std::string *host, u16 *port) {
	*host = server;
	*port = kServerPort;
	size_t colon = server.rfind(':');
	if (colon != std::string::npos && colon + 1 < server.size() &&
	    server.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
		int p = atoi(server.c_str() + colon + 1);
		if (p > 0 && p < 65536) {
			*host = server.substr(0, colon);
			*port = (u16)p;
		}
	}
	if (host->empty()) *host = "localhost";
}

bool IsLoopbackHost(const std::string &host) {
	std::string h = host;
	for (char &c : h) c = (char)tolower((unsigned char)c);
	if (h == "localhost") return true;
	u32 a;
	return hostnet::ParseIpv4(h.c_str(), &a) && hostnet::IsLoopback(a);
}

void StartLink() {
	auto sh = std::make_shared<LinkShared>();
	sh->wake = hostnet::NewEvent();
	sh->done = hostnet::NewEvent(true);
	sh->gen = g_nextLinkGen++;
	ParseServer(net::CurrentConfig().adhocServer, &sh->host, &sh->port);
	g_localServer = IsLoopbackHost(sh->host);
	if (g_localServer) {
		// One loopback address per PSPC instance: the server and the peers tell us apart by it.
		g_localIp = htonl(0x7F000000u | (u32)std::max(1, std::min(254, net::InstanceNumber())));
		sh->bindIp = g_localIp;
	} else {
		hostnet::HostConfig hc = hostnet::QueryHostConfig();
		g_localIp = hc.valid ? hc.ip : htonl(INADDR_LOOPBACK);
	}
	sh->login.opcode = OPCODE_LOGIN;
	memcpy(sh->login.mac, net::kLocalMac, 6);
	memcpy(sh->login.name, g_parameter.nickname, kNicknameLen);
	sh->login.name[kNicknameLen - 1] = 0;
	memcpy(sh->login.game, g_productCode.data, kProductCodeLen);
	sh->loginRequested = true;
	g_link = sh;
	g_serverConnected = false;
	g_loginFailed = false;
	LOGI(LOG, "Ad hoc server %s:%u, local ad hoc address %s", sh->host.c_str(), sh->port, hostnet::Ipv4ToString(g_localIp).c_str());
	static bool atExitRegistered = false;
	if (!atExitRegistered) {
		// A game that never calls sceNetAdhocctlTerm: stop the link before the process tears down.
		std::atexit([] {
			if (auto link = g_link) {
				link->stop = true;
				hostnet::SignalEvent(link->wake);
				hostnet::WaitEvents(&link->done, 1, 500);
			}
		});
		atExitRegistered = true;
	}
	std::thread(LinkThread, sh).detach();
}

void StopLink() {
	if (!g_link) return;
	g_link->stop = true;
	hostnet::SignalEvent(g_link->wake);
	g_link.reset();  // the thread keeps its own reference and ends by itself
	g_serverConnected = false;
}

// ---------------------------------------------------------------- guest waits

bool CanWait() { return sched::Current() != nullptr && !sched::InInterrupt() && sched::DispatchEnabled(); }

void Sleep(u64 us) {
	if (CanWait()) sched::WaitUntil(kernel::WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
}

void WakeWaiters() {
	std::vector<CtlWaiter> ws = g_waiters;
	for (auto &w : ws) {
		auto *t = kernel::Get<kernel::PspThread>(w.tid);
		if (t && t->IsWaitingFor(kernel::WAITTYPE_NET, w.waitId)) sched::Wake(t, 0);
	}
}

// Sleeps the current guest thread until `done()` holds (re-checked whenever the server connection
// reports something, and every 50 ms) or `deadline` passes. Returns done().
bool WaitFor(std::function<bool()> done, u64 deadline) {
	if (!CanWait()) return done();
	SceUID waitId = (SceUID)(0x58000000u | (g_nextWaitId++ & 0x00FFFFFF));
	g_waiters.push_back(CtlWaiter{sched::CurrentId(), waitId});
	// Waiting on the network: idle time passes in real time meanwhile (fast-time mode).
	sched::HoldRealTime(true);
	bool ok;
	for (;;) {
		ok = done();
		u64 now = sched::NowUs();
		if (ok || now >= deadline) break;
		sched::WaitUntil(kernel::WAITTYPE_NET, waitId, std::min(deadline, now + 50000), false);
	}
	sched::HoldRealTime(false);
	g_waiters.erase(std::remove_if(g_waiters.begin(), g_waiters.end(), [waitId](const CtlWaiter &w) { return w.waitId == waitId; }),
	                g_waiters.end());
	return ok;
}

// PPSSPP's WaitBlockingAdhocctlSocket: sends a request to the ad hoc server (once the connection is
// up: a lost connection is made again first) and makes the caller wait until it went out. 0, or
// BUSY when it couldn't be sent within 5 s. The Netconf dialog doesn't wait (`block` false).
int ServerRequest(std::string bytes, bool block) {
	g_startTime = sched::NowUs();
	if (!g_link) StartLink();
	LinkRequestLogin();
	if (!block || !CanWait()) {
		LinkSend(std::move(bytes), 0);
		return 0;
	}
	static u32 nextReq = 1;
	u32 reqId = nextReq++;
	if (!reqId) reqId = nextReq++;
	g_sentRequests[reqId] = false;
	LinkSend(std::move(bytes), reqId);
	g_requestsInFlight++;
	u32 gen = g_ctlGen;
	u64 start = sched::NowUs();
	bool sent = WaitFor([&] { return gen != g_ctlGen || (g_sentRequests[reqId] && sched::NowUs() >= start + kDefaultDelayUs); },
	                    start + kDefaultTimeoutUs + 500);
	g_requestsInFlight--;
	g_sentRequests.erase(reqId);
	if (!sent) {
		LinkCancel(reqId);
		LOGW(LOG, "Ad hoc server request %u not sent within %llu ms: BUSY", reqId, (unsigned long long)(kDefaultTimeoutUs / 1000));
		return (int)SCE_NET_ADHOCCTL_ERROR_BUSY;
	}
	return 0;
}

// ---------------------------------------------------------------- events (SceNetAdhocctl thread)

void SetState(int state) {
	if (state != g_state) LOGI(LOG, "sceNetAdhocctl: state %d -> %d", g_state, state);
	g_state = state;
}

// One event: the state change after PPSSPP's delay, then every handler (event, error, arg).
void RunEvent(u32 flags, u32 error, u32 gen) {
	auto stale = [gen] { return gen != g_ctlGen || !g_inited; };
	if ((flags == EVENT_CONNECT || flags == EVENT_GAME) && g_connType == CONNTYPE_JOIN) {
		// Joining: the game expects to see the group's creator among the peers already.
		while (ActivePeerCount() == 0 && sched::NowUs() - g_startTime <= kDefaultTimeoutUs) {
			Sleep(kDefaultDelayUs);
			if (stale()) return;
		}
	}
	int newState = -1;
	u64 delay = kEventPollDelayUs;
	switch (flags) {
	case EVENT_CONNECT:
		newState = net::ADHOCCTL_STATE_CONNECTED;
		if (g_connType == CONNTYPE_CREATE) delay = kEventDelayUs;
		else if (g_connType == CONNTYPE_CONNECT) delay = kEventDelayUs / 2;
		break;
	case EVENT_SCAN: newState = net::ADHOCCTL_STATE_DISCONNECTED; break;
	case EVENT_DISCONNECT:
		newState = net::ADHOCCTL_STATE_DISCONNECTED;
		delay = kDefaultDelayUs;  // Tekken 5 expects a disconnect to be done within a frame or so
		break;
	case EVENT_GAME:
		newState = net::ADHOCCTL_STATE_GAMEMODE;
		delay = kEventDelayUs;
		if (g_connType == CONNTYPE_JOIN) delay += kExtraDelayUs * 3;
		for (size_t i = 0; i < g_gmMacs.size(); i++) LOGI(LOG, "Game mode member #%zu: %s", i, MacStr(g_gmMacs[i].data()).c_str());
		break;
	case EVENT_DISCOVER: newState = net::ADHOCCTL_STATE_DISCOVER; break;
	case EVENT_WOL_INTERRUPT: newState = net::ADHOCCTL_STATE_WOL; break;
	case EVENT_ERROR: delay = kDefaultDelayUs * 3; break;
	}
	Sleep(delay);
	if (stale()) return;
	if (newState >= 0) SetState(newState);
	if (g_handlers.empty()) g_busy = false;
	std::map<int, Handler> handlers = g_handlers;
	for (auto &h : handlers) {
		LOGD(LOG, "Adhocctl handler %d: event %u, error %08x", h.first, flags, error);
		net::HandlerThread::CallHandler(h.second.entry, {flags, error, h.second.arg});
		g_busy = false;
		if (stale()) return;
	}
}

void Notify(u32 flags, u32 error) {
	LOGI(LOG, "sceNetAdhocctl: event %u (error %08x)", flags, error);
	u32 gen = g_ctlGen;
	if (g_thread.Running()) {
		g_thread.Post([flags, error, gen] { RunEvent(flags, error, gen); });
		return;
	}
	// No handler thread (it couldn't start): just the state change.
	if (flags == EVENT_CONNECT) SetState(net::ADHOCCTL_STATE_CONNECTED);
	else if (flags == EVENT_SCAN || flags == EVENT_DISCONNECT) SetState(net::ADHOCCTL_STATE_DISCONNECTED);
	else if (flags == EVENT_GAME) SetState(net::ADHOCCTL_STATE_GAMEMODE);
	g_busy = false;
}

// ---------------------------------------------------------------- server messages (emulation thread)

void TimeoutAllPeers() {
	for (Peer &p : g_peers) p.lastRecv = 0;
}

bool HasMac(const std::vector<std::array<u8, 6>> &v, const u8 *mac) {
	for (auto &m : v)
		if (MacMatch(m.data(), mac)) return true;
	return false;
}

void OnLinkLogin(u32 gen, bool ok, u32 localIp) {
	if (!g_link || gen != g_link->gen) return;
	if (ok) {
		g_serverConnected = true;
		g_loginFailed = false;
		if (!g_localServer && localIp) g_localIp = localIp;
		// Not in a group on the server (any more).
		SetState(net::ADHOCCTL_STATE_DISCONNECTED);
		g_gmEntered = false;
		g_busy = false;
	} else {
		g_serverConnected = false;
		g_loginFailed = true;
	}
	WakeWaiters();
}

void OnLinkLost(u32 gen) {
	if (!g_link || gen != g_link->gen) return;
	g_serverConnected = false;
	// Without the server nobody's departure would be noticed.
	TimeoutAllPeers();
	WakeWaiters();
}

void OnLinkSent(u32 gen, u32 reqId) {
	if (!g_link || gen != g_link->gen) return;
	auto it = g_sentRequests.find(reqId);
	if (it != g_sentRequests.end()) it->second = true;
	WakeWaiters();
}

void AddPeer(const ConnectS2C &p) {
	Peer *peer = FindPeer(p.mac);
	if (!peer) {
		g_peers.insert(g_peers.begin(), Peer{});
		peer = &g_peers.front();
	}
	memcpy(peer->nickname, p.name, kNicknameLen);
	peer->nickname[kNicknameLen - 1] = 0;
	memcpy(peer->mac, p.mac, 6);
	peer->ip = p.ip;
	peer->lastRecv = sched::NowUs();
}

void OnLinkPacket(u32 gen, const std::string &pk) {
	if (!g_link || gen != g_link->gen || pk.empty()) return;
	switch ((u8)pk[0]) {
	case OPCODE_CONNECT_BSSID: {
		ConnectBssidS2C p;
		memcpy(&p, pk.data(), sizeof(p));
		LOGI(LOG, "Ad hoc server: group joined, BSSID %s", MacStr(p.mac).c_str());
		// The group's founder (like an access point's BSSID), not necessarily us.
		memcpy(g_parameter.bssid, p.mac, 6);
		if (g_mode == CTLMODE_GAMEMODE) {
			if (!HasMac(g_gmMacs, net::kLocalMac)) {
				std::array<u8, 6> me;
				memcpy(me.data(), net::kLocalMac, 6);
				g_gmMacs.push_back(me);
				if (g_gmEntered && g_gmMacs.size() >= g_gmRequired.size()) Notify(EVENT_GAME, 0);
			}
		} else {
			Notify(EVENT_CONNECT, 0);
		}
		break;
	}
	case OPCODE_CONNECT: {
		ConnectS2C p;
		memcpy(&p, pk.data(), sizeof(p));
		p.name[kNicknameLen - 1] = 0;
		LOGI(LOG, "Ad hoc server: peer %s (%s) at %s", (const char *)p.name, MacStr(p.mac).c_str(), hostnet::Ipv4ToString(p.ip).c_str());
		AddPeer(p);
		if (g_mode == CTLMODE_GAMEMODE && !HasMac(g_gmMacs, p.mac)) {
			std::array<u8, 6> m;
			memcpy(m.data(), p.mac, 6);
			// Host first; after ourselves, newcomers go to the back (the same order on every member).
			if (HasMac(g_gmMacs, net::kLocalMac) || g_gmMacs.empty()) g_gmMacs.push_back(m);
			else g_gmMacs.insert(g_gmMacs.begin() + 1, m);
			if (g_gmEntered && !g_gmRequired.empty() && g_gmMacs.size() == g_gmRequired.size()) Notify(EVENT_GAME, 0);
		}
		break;
	}
	case OPCODE_DISCONNECT: {
		DisconnectS2C p;
		memcpy(&p, pk.data(), sizeof(p));
		LOGI(LOG, "Ad hoc server: peer at %s left", hostnet::Ipv4ToString(p.ip).c_str());
		for (Peer &peer : g_peers) {
			if (peer.ip != p.ip) continue;
			if (g_mode == CTLMODE_GAMEMODE) GameModePeerLeft(peer.mac);
			// Kept, timed out: games moving to another group still talk about it.
			peer.lastRecv = 0;
			break;
		}
		break;
	}
	case OPCODE_SCAN: {
		ScanS2C p;
		memcpy(&p, pk.data(), sizeof(p));
		Network n{};
		memcpy(n.group, p.group, kGroupNameLen);
		memcpy(n.bssid, p.mac, 6);
		n.channel = g_parameter.channel;
		n.mode = std::max((int)CTLMODE_NORMAL, g_mode);
		g_newNetworks.insert(g_newNetworks.begin(), n);
		break;
	}
	case OPCODE_SCAN_COMPLETE:
		g_networks = g_newNetworks;
		g_newNetworks.clear();
		LOGI(LOG, "Ad hoc server: scan found %zu group(s)", g_networks.size());
		Notify(EVENT_SCAN, 0);
		break;
	case OPCODE_CHAT: {
		ChatS2C p;
		memcpy(&p, pk.data(), sizeof(p));
		p.message[kChatLen - 1] = 0;
		p.name[kNicknameLen - 1] = 0;
		LOGI(LOG, "Ad hoc chat: %.8s: %s", (const char *)p.name, p.message);
		break;
	}
	}
}

// ---------------------------------------------------------------- the library

void ClearGameMode() {
	GameModeDeleteBuffers();
	g_gmMacs.clear();
	g_gmRequired.clear();
}

void CancelGameModeTimeout() {
	if (g_gmTimeoutEvent) sched::CancelEvent(g_gmTimeoutEvent);
	g_gmTimeoutEvent = 0;
}

int CtlDisconnect() {
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	// Tekken 6 needs about a frame here before its next adhocctl call when leaving its lobby.
	sched::ChargeUs(16667);
	if (g_busy && g_requestsInFlight > 0) return (int)SCE_NET_ADHOCCTL_ERROR_BUSY;
	if (g_state != net::ADHOCCTL_STATE_DISCONNECTED) {
		g_busy = true;
		memset(g_parameter.group, 0, sizeof(g_parameter.group));
		if (g_serverConnected) {
			u8 op = OPCODE_DISCONNECT;
			LinkSend(std::string((const char *)&op, 1), 0);
		} else {
			SetState(net::ADHOCCTL_STATE_DISCONNECTED);  // nobody to tell
		}
	}
	TimeoutAllPeers();
	g_mode = CTLMODE_NONE;
	// Handlers hear about it even when nothing was connected (God Eater waits for it).
	Notify(EVENT_DISCONNECT, 0);
	return 0;
}

int CtlExitGameMode() {
	GameModeCloseSocket();
	ClearGameMode();
	CancelGameModeTimeout();
	g_mode = CTLMODE_NONE;
	g_gmEntered = false;
	return CtlDisconnect();
}

// The checks and the CONNECT request shared by Create / Connect / Join / the game mode ones.
int CtlCreate(const u8 *groupName, bool block) {
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (groupName && !ValidGroupName(groupName)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	if (g_state == net::ADHOCCTL_STATE_CONNECTED || g_state == net::ADHOCCTL_STATE_GAMEMODE) {
		// Already in a group: success, and the handlers get an error.
		Notify(EVENT_ERROR, SCE_NET_ADHOCCTL_ERROR_ALREADY_CONNECTED);
		sched::ChargeUs(500);
		return 0;
	}
	if (g_state != net::ADHOCCTL_STATE_DISCONNECTED || g_busy) return (int)SCE_NET_ADHOCCTL_ERROR_BUSY;
	g_busy = true;
	g_loginFailed = false;
	memset(g_parameter.group, 0, sizeof(g_parameter.group));
	if (groupName) {
		for (int i = 0; i < kGroupNameLen && groupName[i]; i++) g_parameter.group[i] = groupName[i];
	}
	ConnectC2S p{};
	p.opcode = OPCODE_CONNECT;
	memcpy(p.group, g_parameter.group, kGroupNameLen);
	return ServerRequest(std::string((const char *)&p, sizeof(p)), block);
}

std::string GroupStr(const u8 *g) {
	char b[kGroupNameLen + 1] = {};
	if (g) memcpy(b, g, kGroupNameLen);
	return b;
}

// A guest group name argument: 8 bytes, not necessarily NUL-terminated (null pointer allowed).
bool ReadGroupName(u32 addr, u8 *out) {
	memset(out, 0, kGroupNameLen);
	if (!addr) return false;
	for (u32 i = 0; i < kGroupNameLen; i++) {
		if (!mem::IsValidAddress(addr + i)) break;
		out[i] = GRead8(addr + i);
		if (!out[i]) break;
	}
	return true;
}

void GameModeTimeoutCheck(u32 gen) {
	g_gmTimeoutEvent = 0;
	if (gen != g_ctlGen || !g_inited) return;
	// Not everybody joined in time.
	if (g_busy && g_state == net::ADHOCCTL_STATE_DISCONNECTED && g_mode == CTLMODE_GAMEMODE && g_gmEntered) {
		g_gmEntered = false;
		Notify(EVENT_ERROR, SCE_NET_ADHOC_ERROR_TIMEOUT);
	}
}

void ScheduleGameModeTimeout() {
	CancelGameModeTimeout();
	u32 gen = g_ctlGen;
	g_gmTimeoutEvent = sched::ScheduleEvent(sched::NowUs() + g_gmEnterTimeoutUs, [gen] { GameModeTimeoutCheck(gen); });
}

// ---- HLE

int sceNetAdhocctlInit(int stackSize, int prio, u32 productAddr) {
	if (!net::Enabled()) return offline::CtlInit(stackSize, prio, productAddr);
	LOGI(LOG, "sceNetAdhocctlInit(%d, %d, %08x)", stackSize, prio, productAddr);
	if (g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_ALREADY_INITIALIZED;
	if (mem::IsValidRange(productAddr, sizeof(AdhocId))) mem::ReadBytes(&g_productCode, productAddr, sizeof(AdhocId));
	memset(&g_parameter, 0, sizeof(g_parameter));
	strncpy((char *)g_parameter.nickname, net::kNickname, kNicknameLen - 1);
	g_parameter.channel = kDefaultChannel;
	g_inited = true;
	g_loginFailed = false;
	g_state = net::ADHOCCTL_STATE_DISCONNECTED;
	g_thread.Start("SceNetAdhocctl", prio, stackSize);
	if (!g_holdingRealTime) {
		// The server connection is a host socket: time-outs keep their meaning in fast-time mode.
		net::HostSocketOpened();
		g_holdingRealTime = true;
	}
	StopLink();
	StartLink();
	// GTA VCS needs the login done before it creates or joins a group.
	u32 gen = g_ctlGen;
	u64 start = sched::NowUs();
	WaitFor([&] { return gen != g_ctlGen || ((g_serverConnected || g_loginFailed) && sched::NowUs() >= start + kDefaultDelayUs); },
	        start + kDefaultTimeoutUs + 500);
	return 0;
}

int sceNetAdhocctlTerm() {
	if (!net::Enabled()) return offline::CtlTerm();
	CtlShutdown();
	sched::ChargeUs(kDefaultDelayUs);
	return 0;
}

int sceNetAdhocctlGetState(u32 stateAddr) {
	if (!net::Enabled()) return offline::CtlGetState(stateAddr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(stateAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	GWrite32(stateAddr, (u32)g_state);
	return 0;
}

int sceNetAdhocctlAddHandler(u32 handler, u32 arg) {
	if (!net::Enabled()) return offline::CtlAddHandler(handler, arg);
	int id = 0;
	while (g_handlers.count(id)) id++;
	for (auto &h : g_handlers)
		if (h.second.entry == handler) return 0;  // the same handler again: success, nothing added (PPSSPP)
	if (!mem::IsValidAddress(handler)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	if ((int)g_handlers.size() >= kMaxHandlers) return (int)SCE_NET_ADHOCCTL_ERROR_TOO_MANY_HANDLERS;
	g_handlers[id] = Handler{handler, arg};
	LOGI(LOG, "sceNetAdhocctlAddHandler(%08x, %08x) -> %d", handler, arg, id);
	return id;
}

int sceNetAdhocctlDelHandler(u32 id) {
	if (!net::Enabled()) return offline::CtlDelHandler(id);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	g_handlers.erase((int)id);  // an unknown id is no error
	return 0;
}

int sceNetAdhocctlCreate(u32 groupAddr) {
	if (!net::Enabled()) return offline::CtlNeedsRadio("sceNetAdhocctlCreate");
	u8 group[kGroupNameLen];
	bool have = ReadGroupName(groupAddr, group);
	LOGI(LOG, "sceNetAdhocctlCreate(%s)", GroupStr(group).c_str());
	g_mode = CTLMODE_NORMAL;
	g_connType = CONNTYPE_CREATE;
	return CtlCreate(have ? group : nullptr, true);
}

int sceNetAdhocctlConnect(u32 groupAddr) {
	if (!net::Enabled()) return offline::CtlNeedsRadio("sceNetAdhocctlConnect");
	u8 group[kGroupNameLen];
	bool have = ReadGroupName(groupAddr, group);
	LOGI(LOG, "sceNetAdhocctlConnect(%s)", GroupStr(group).c_str());
	g_mode = CTLMODE_NORMAL;
	g_connType = CONNTYPE_CONNECT;
	return CtlCreate(have ? group : nullptr, true);
}

int CtlJoin(u32 scanInfoAddr, bool block) {
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(scanInfoAddr, sizeof(ScanInfoEmu))) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	ScanInfoEmu si;
	mem::ReadBytes(&si, scanInfoAddr, sizeof(si));
	u8 group[kGroupNameLen] = {};
	for (int i = 0; i < kGroupNameLen && si.group[i]; i++) group[i] = si.group[i];
	LOGI(LOG, "sceNetAdhocctlJoin(%s)", GroupStr(group).c_str());
	g_mode = CTLMODE_NORMAL;
	g_connType = CONNTYPE_JOIN;
	return CtlCreate(group, block);
}

int sceNetAdhocctlJoin(u32 scanInfoAddr) {
	if (!net::Enabled()) return offline::CtlNeedsRadio("sceNetAdhocctlJoin");
	return CtlJoin(scanInfoAddr, true);
}

int sceNetAdhocctlScan() {
	if (!net::Enabled()) return offline::CtlNeedsRadio("sceNetAdhocctlScan");
	LOGI(LOG, "sceNetAdhocctlScan()");
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (g_state == net::ADHOCCTL_STATE_CONNECTED || g_state == net::ADHOCCTL_STATE_GAMEMODE) {
		// Valhalla Knights 2 needs the handler call.
		Notify(EVENT_ERROR, SCE_NET_ADHOCCTL_ERROR_ALREADY_CONNECTED);
		sched::ChargeUs(500);
		return 0;
	}
	// Only from the disconnected state: the server would drop us otherwise.
	if (g_state != net::ADHOCCTL_STATE_DISCONNECTED || g_busy) return (int)SCE_NET_ADHOCCTL_ERROR_BUSY;
	g_busy = true;
	g_loginFailed = false;
	SetState(net::ADHOCCTL_STATE_SCANNING);
	g_mode = CTLMODE_NORMAL;
	g_networks.clear();
	u8 op = OPCODE_SCAN;
	return ServerRequest(std::string((const char *)&op, 1), true);
}

int sceNetAdhocctlDisconnect() {
	if (!net::Enabled()) return offline::CtlDisconnect();
	LOGI(LOG, "sceNetAdhocctlDisconnect() group %s", GroupStr(g_parameter.group).c_str());
	return CtlDisconnect();
}

int sceNetAdhocctlGetParameter(u32 paramAddr) {
	if (!net::Enabled()) return offline::CtlGetParameter(paramAddr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(paramAddr, sizeof(Parameter))) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	g_parameter.nickname[kNicknameLen - 1] = 0;
	mem::WriteBytes(paramAddr, &g_parameter, sizeof(g_parameter));
	return 0;
}

int sceNetAdhocctlGetAdhocId(u32 addr) {
	if (!net::Enabled()) return offline::CtlGetAdhocId(addr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(addr, sizeof(AdhocId))) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	mem::WriteBytes(addr, &g_productCode, sizeof(g_productCode));
	return 0;
}

int sceNetAdhocctlGetScanInfo(u32 sizeAddr, u32 bufAddr) {
	if (!net::Enabled()) return offline::CtlEmptyList(sizeAddr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	if (g_state == net::ADHOCCTL_STATE_CONNECTED || g_state == net::ADHOCCTL_STATE_GAMEMODE) {
		GWrite32(sizeAddr, 0);  // in a group: no scan results
	} else if (!bufAddr || !mem::IsValidAddress(bufAddr)) {
		GWrite32(sizeAddr, (u32)(g_networks.size() * sizeof(ScanInfoEmu)));
	} else {
		s32 room = (s32)GRead32(sizeAddr);
		if (room > 0 && mem::IsValidRange(bufAddr, (u32)room)) mem::Memset(bufAddr, 0, (u32)room);
		int want = room > 0 ? room / (int)sizeof(ScanInfoEmu) : 0;
		int n = 0;
		for (const Network &net : g_networks) {
			if (n >= want) break;
			u32 e = bufAddr + (u32)n * sizeof(ScanInfoEmu);
			if (!mem::IsValidRange(e, sizeof(ScanInfoEmu))) break;
			ScanInfoEmu si{};
			si.channel = net.channel;
			memcpy(si.group, net.group, kGroupNameLen);
			memcpy(si.bssid, net.bssid, 6);
			si.mode = net.mode;
			mem::WriteBytes(e, &si, sizeof(si));
			if (n > 0) GWrite32(e - sizeof(ScanInfoEmu), e);
			n++;
		}
		GWrite32(sizeAddr, (u32)(n * sizeof(ScanInfoEmu)));
	}
	sched::ChargeUs(200);
	return 0;
}

void FillPeerInfo(PeerInfoEmu *pi, const u8 *nickname, const u8 *mac, u64 lastRecv) {
	memset(pi, 0, sizeof(*pi));
	memcpy(pi->nickname, nickname, kNicknameLen);
	pi->nickname[kNicknameLen - 1] = 0;
	memcpy(pi->mac, mac, 6);
	pi->flags = 0x0400;
	pi->lastRecv = lastRecv;
}

u64 FreshLastRecv(u64 t) {
	u64 now = sched::NowUs();
	u64 floor = now > kLastRecvDeltaUs ? now - kLastRecvDeltaUs : 0;
	return std::max(t, floor);
}

// Writes a linked list of peer infos to (bufAddr, *sizeAddr) and returns how many were written.
template <typename Pick>
void WritePeerList(u32 sizeAddr, u32 bufAddr, int count, Pick pick) {
	if (!bufAddr || !mem::IsValidAddress(bufAddr)) {
		GWrite32(sizeAddr, (u32)(count * sizeof(PeerInfoEmu)));
		return;
	}
	s32 room = (s32)GRead32(sizeAddr);
	if (room > 0 && mem::IsValidRange(bufAddr, (u32)room)) mem::Memset(bufAddr, 0, (u32)room);
	int want = room > 0 ? room / (int)sizeof(PeerInfoEmu) : 0;
	int n = 0;
	PeerInfoEmu pi;
	for (int i = 0; n < want && pick(i, &pi); i++) {
		u32 e = bufAddr + (u32)n * sizeof(PeerInfoEmu);
		if (!mem::IsValidRange(e, sizeof(PeerInfoEmu))) break;
		mem::WriteBytes(e, &pi, sizeof(pi));
		if (n > 0) GWrite32(e - sizeof(PeerInfoEmu), e);
		n++;
	}
	GWrite32(sizeAddr, (u32)(n * sizeof(PeerInfoEmu)));
}

int sceNetAdhocctlGetPeerList(u32 sizeAddr, u32 bufAddr) {
	if (!net::Enabled()) return offline::CtlEmptyList(sizeAddr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	std::vector<Peer *> active;
	for (Peer &p : g_peers)
		if (p.lastRecv != 0) active.push_back(&p);
	WritePeerList(sizeAddr, bufAddr, (int)active.size(), [&](int i, PeerInfoEmu *pi) {
		if (i >= (int)active.size()) return false;
		Peer *p = active[i];
		p->lastRecv = FreshLastRecv(p->lastRecv);
		FillPeerInfo(pi, p->nickname, p->mac, p->lastRecv);
		return true;
	});
	Sleep(100);
	return 0;
}

int sceNetAdhocctlGetPeerInfo(u32 macAddr, int size, u32 infoAddr) {
	if (!net::Enabled()) return offline::CtlGetPeerInfo(macAddr, size, infoAddr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (size < (int)sizeof(PeerInfoEmu) || !mem::IsValidRange(infoAddr, sizeof(PeerInfoEmu)) || !mem::IsValidRange(macAddr, 6))
		return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	PeerInfoEmu pi;
	int r = (int)SCE_NET_ADHOC_ERROR_NO_ENTRY;
	if (IsLocalMac(mac)) {
		u64 now = sched::NowUs();
		FillPeerInfo(&pi, g_parameter.nickname, mac, now > kLastRecvDeltaUs ? now - kLastRecvDeltaUs : 0);
		r = 0;
	} else if (Peer *p = FindPeer(mac)) {
		if (p->lastRecv != 0) {
			p->lastRecv = FreshLastRecv(p->lastRecv);
			FillPeerInfo(&pi, p->nickname, mac, p->lastRecv);
			r = 0;
		}
	}
	if (r == 0) mem::WriteBytes(infoAddr, &pi, sizeof(pi));
	sched::ChargeUs(50);
	return r;
}

int sceNetAdhocctlGetNameByAddr(u32 macAddr, u32 nameAddr) {
	if (!net::Enabled()) return offline::CtlGetNameByAddr(macAddr, nameAddr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6) || !mem::IsValidRange(nameAddr, kNicknameLen)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	if (IsLocalMac(mac)) {
		mem::WriteBytes(nameAddr, g_parameter.nickname, kNicknameLen);
		return 0;
	}
	for (Peer &p : g_peers) {
		if (p.lastRecv != 0 && MacMatch(p.mac, mac)) {
			mem::WriteBytes(nameAddr, p.nickname, kNicknameLen);
			return 0;
		}
	}
	return (int)SCE_NET_ADHOC_ERROR_NO_ENTRY;
}

int sceNetAdhocctlGetAddrByName(u32 nickAddr, u32 sizeAddr, u32 bufAddr) {
	if (!net::Enabled()) return offline::CtlGetAddrByName(nickAddr, sizeAddr);
	if (!nickAddr || !mem::IsValidAddress(nickAddr) || !mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	char nick[kNicknameLen] = {};
	for (u32 i = 0; i + 1 < kNicknameLen && mem::IsValidAddress(nickAddr + i); i++) {
		nick[i] = (char)GRead8(nickAddr + i);
		if (!nick[i]) break;
	}
	struct Match {
		const u8 *nickname;
		const u8 *mac;
		Peer *peer;
	};
	std::vector<Match> found;
	if (!strncmp((const char *)g_parameter.nickname, nick, kNicknameLen)) found.push_back(Match{g_parameter.nickname, net::kLocalMac, nullptr});
	for (Peer &p : g_peers)
		if (p.lastRecv != 0 && !strncmp((const char *)p.nickname, nick, kNicknameLen)) found.push_back(Match{p.nickname, p.mac, &p});
	WritePeerList(sizeAddr, bufAddr, (int)found.size(), [&](int i, PeerInfoEmu *pi) {
		if (i >= (int)found.size()) return false;
		u64 t;
		if (found[i].peer) t = found[i].peer->lastRecv = FreshLastRecv(found[i].peer->lastRecv);
		else t = FreshLastRecv(0);
		FillPeerInfo(pi, found[i].nickname, found[i].mac, t);
		return true;
	});
	Sleep(100);
	return 0;
}

int CtlCreateEnterGameMode(u32 groupAddr, int gameType, int numMembers, u32 membersAddr, u32 timeout) {
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidAddress(membersAddr)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	if (gameType < 1 || gameType > 3 || numMembers < 2 || numMembers > 16 || (gameType == 1 && numMembers > 4))
		return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	if (!mem::IsValidRange(membersAddr, (u32)numMembers * 6)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	ClearGameMode();
	for (int i = 0; i < numMembers; i++) {
		std::array<u8, 6> m;
		mem::ReadBytes(m.data(), membersAddr + (u32)i * 6, 6);
		g_gmRequired.push_back(m);
	}
	std::array<u8, 6> me;
	memcpy(me.data(), net::kLocalMac, 6);
	g_gmMacs.push_back(me);  // the host first
	g_mode = CTLMODE_GAMEMODE;
	g_connType = CONNTYPE_CREATE;
	g_gmEntered = true;
	g_gmEnterTimeoutUs = timeout;
	u8 group[kGroupNameLen];
	bool have = ReadGroupName(groupAddr, group);
	int r = CtlCreate(have ? group : nullptr, true);
	if (r == 0) ScheduleGameModeTimeout();
	return r;
}

int sceNetAdhocctlCreateEnterGameMode(u32 groupAddr, int gameType, int numMembers, u32 membersAddr, u32 timeout, int flag) {
	if (!net::Enabled()) return offline::CtlNeedsRadio("sceNetAdhocctlCreateEnterGameMode");
	LOGI(LOG, "sceNetAdhocctlCreateEnterGameMode(%08x, %d, %d, %08x, %u, %d)", groupAddr, gameType, numMembers, membersAddr, timeout, flag);
	return CtlCreateEnterGameMode(groupAddr, gameType, numMembers, membersAddr, timeout);
}

// The minimum member count isn't checked (PPSSPP doesn't either).
int sceNetAdhocctlCreateEnterGameModeMin(u32 groupAddr, int gameType, int minMembers, int numMembers, u32 membersAddr, u32 timeout,
                                         int flag) {
	if (!net::Enabled()) return offline::CtlNeedsRadio("sceNetAdhocctlCreateEnterGameModeMin");
	return CtlCreateEnterGameMode(groupAddr, gameType, numMembers, membersAddr, timeout);
}

int sceNetAdhocctlJoinEnterGameMode(u32 groupAddr, u32 hostMacAddr, u32 timeout, int flag) {
	if (!net::Enabled()) return offline::CtlNeedsRadio("sceNetAdhocctlJoinEnterGameMode");
	LOGI(LOG, "sceNetAdhocctlJoinEnterGameMode(%08x, %08x, %u, %d)", groupAddr, hostMacAddr, timeout, flag);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(hostMacAddr, 6)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	ClearGameMode();
	std::array<u8, 6> host;
	mem::ReadBytes(host.data(), hostMacAddr, 6);
	g_gmMacs.push_back(host);  // the host first
	g_mode = CTLMODE_GAMEMODE;
	g_connType = CONNTYPE_JOIN;
	g_gmEntered = true;
	g_gmEnterTimeoutUs = timeout;
	u8 group[kGroupNameLen];
	bool have = ReadGroupName(groupAddr, group);
	int r = CtlCreate(have ? group : nullptr, true);
	if (r == 0) ScheduleGameModeTimeout();
	return r;
}

int sceNetAdhocctlExitGameMode() {
	if (!net::Enabled()) return offline::CtlExitGameMode();
	LOGI(LOG, "sceNetAdhocctlExitGameMode()");
	return CtlExitGameMode();
}

int sceNetAdhocctlGetGameModeInfo(u32 infoAddr) {
	if (!net::Enabled()) return offline::CtlGetGameModeInfo(infoAddr);
	if (!g_inited) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(infoAddr, sizeof(GameModeInfo))) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	GameModeInfo gi{};
	gi.num = (s32)g_gmMacs.size();
	for (size_t i = 0; i < g_gmMacs.size() && i < 16; i++) memcpy(gi.members[i], g_gmMacs[i].data(), 6);
	mem::WriteBytes(infoAddr, &gi, sizeof(gi));
	sched::ChargeUs(100);
	return 0;
}

}  // namespace

// ---------------------------------------------------------------- shared with the other ad hoc files

bool MacMatch(const u8 *a, const u8 *b) { return memcmp(a + 1, b + 1, 5) == 0; }
bool IsLocalMac(const u8 *mac) { return MacMatch(mac, net::kLocalMac); }
bool IsZeroMac(const u8 *mac) {
	static const u8 z[6] = {};
	return memcmp(mac, z, 6) == 0;
}
bool IsBroadcastMac(const u8 *mac) {
	static const u8 b[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	return memcmp(mac, b, 6) == 0;
}
std::string MacStr(const u8 *m) {
	char b[18];
	snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	return b;
}

std::vector<Peer> &Peers() { return g_peers; }

Peer *FindPeer(const u8 *mac) {
	for (Peer &p : g_peers)
		if (MacMatch(p.mac, mac)) return &p;
	return nullptr;
}

bool ResolveIp(u32 ip, u8 *mac) {
	if (ip == LocalIp()) {
		memcpy(mac, net::kLocalMac, 6);
		return true;
	}
	for (Peer &p : g_peers) {
		if (p.ip == ip) {
			memcpy(mac, p.mac, 6);
			return true;
		}
	}
	return false;
}

bool ResolveMac(const u8 *mac, u32 *ip) {
	if (IsLocalMac(mac)) {
		*ip = LocalIp();
		return true;
	}
	for (Peer &p : g_peers) {
		if (MacMatch(p.mac, mac)) {
			*ip = p.ip;
			return true;
		}
	}
	return false;
}

// A player the server said left stays gone (PPSSPP revives it here, so a datagram that arrives after
// the server's DISCONNECT keeps a departed player in the peer list for good).
void TouchPeer(const u8 *mac) {
	Peer *p = FindPeer(mac);
	if (p && p->lastRecv != 0) p->lastRecv = sched::NowUs();
}

int ActivePeerCount(bool excludeTimedOut) {
	int n = 0;
	for (Peer &p : g_peers)
		if (!excludeTimedOut || p.lastRecv != 0) n++;
	return n;
}

u32 LocalIp() {
	if (g_localIp) return g_localIp;
	// Before sceNetAdhocctlInit: what it is going to be.
	std::string host;
	u16 port;
	ParseServer(net::CurrentConfig().adhocServer, &host, &port);
	if (IsLoopbackHost(host)) return htonl(0x7F000000u | (u32)std::max(1, std::min(254, net::InstanceNumber())));
	hostnet::HostConfig hc = hostnet::QueryHostConfig();
	g_localIp = hc.valid ? hc.ip : htonl(INADDR_LOOPBACK);
	return g_localIp;
}

bool CtlInited() { return g_inited; }
int CtlCurrentMode() { return g_mode; }
int CtlState() { return g_state; }
std::vector<std::array<u8, 6>> &GameModeMacs() { return g_gmMacs; }
bool GameModeEntered() { return g_gmEntered; }

void CtlShutdown() {
	if (!g_inited) return;
	if (g_state != net::ADHOCCTL_STATE_DISCONNECTED) {
		if (g_gmEntered) CtlExitGameMode();
		else CtlDisconnect();
	}
	StopLink();
	GameModeDeleteMaster();
	ClearGameMode();
	CancelGameModeTimeout();
	g_peers.clear();
	g_networks.clear();
	g_newNetworks.clear();
	g_handlers.clear();
	g_thread.Stop();
	g_ctlGen++;
	WakeWaiters();
	g_mode = CTLMODE_NONE;
	g_busy = false;
	g_inited = false;
	g_state = net::ADHOCCTL_STATE_DISCONNECTED;
	if (g_holdingRealTime) {
		net::HostSocketClosed();
		g_holdingRealTime = false;
	}
}

void RegisterAdhocctlLibrary() {
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
}

// sceNetAdhoc's table lists sceNetAdhocctlConnect too (PPSSPP does the same).
int AdhocctlConnectFromAdhocLib(u32 groupAddr) { return sceNetAdhocctlConnect(groupAddr); }

}  // namespace net::adhoc

// ---------------------------------------------------------------- net_hle.h (the Netconf dialog)

namespace net {

int AdhocctlGetState() {
	if (!net::Enabled()) return ADHOCCTL_STATE_DISCONNECTED;
	return adhoc::CtlState();
}

int AdhocctlCreate(u32 groupNameAddr) {
	if (!net::Enabled()) return adhoc::offline::CtlNeedsRadio("sceNetAdhocctlCreate");
	u8 group[adhocproto::kGroupNameLen];
	bool have = adhoc::ReadGroupName(groupNameAddr, group);
	adhoc::g_mode = adhoc::CTLMODE_NORMAL;
	adhoc::g_connType = adhoc::CONNTYPE_CREATE;
	return adhoc::CtlCreate(have ? group : nullptr, false);
}

int AdhocctlConnect(u32 groupNameAddr) {
	if (!net::Enabled()) return adhoc::offline::CtlNeedsRadio("sceNetAdhocctlConnect");
	u8 group[adhocproto::kGroupNameLen];
	bool have = adhoc::ReadGroupName(groupNameAddr, group);
	adhoc::g_mode = adhoc::CTLMODE_NORMAL;
	adhoc::g_connType = adhoc::CONNTYPE_CONNECT;
	return adhoc::CtlCreate(have ? group : nullptr, false);
}

int AdhocctlJoin(u32 scanInfoAddr) {
	if (!net::Enabled()) return adhoc::offline::CtlNeedsRadio("sceNetAdhocctlJoin");
	return adhoc::CtlJoin(scanInfoAddr, false);
}

int AdhocctlDisconnect() {
	if (!net::Enabled()) return adhoc::offline::CtlDisconnect();
	return adhoc::CtlDisconnect();
}

}  // namespace net
