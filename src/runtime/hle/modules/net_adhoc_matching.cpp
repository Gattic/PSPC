// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceNetAdhocMatching: finding partners inside an ad hoc group, on top of a PDP socket per context.
// A host (or P2P peer) broadcasts HELLO with the game's "hello option"; a client answers with JOIN;
// the host ACCEPTs (or CANCELs), children hear about each other (BIRTH / DEATH), PING keeps members
// alive, BULK carries data, BYE says goodbye. The game hears about all of it through its handler
// (id, event, mac, optlen, opt), called on the context's event thread.
//
// Each started context runs two guest threads like the firmware's library: the input thread
// (priority / stack from sceNetAdhocMatchingStart) sends HELLO / PING on time, checks time-outs,
// sends the queued packets and handles what arrives, sleeping on the socket's readiness event in
// between; the event thread (net::HandlerThread) calls the handler for each event in order, 3 ms
// apart, with the MAC and option data in a buffer of user memory.
//
// Packet formats (little-endian lengths), peer states, event order, the time-out formula and the
// quirks games rely on are ported from PPSSPP (Core/HLE/sceNetAdhocMatching.cpp, proAdhoc.cpp),
// Copyright (c) 2013- PPSSPP Project, GPL-2.0-or-later, itself based on Coldbird's aemu. PPSSPP's
// event and input lists are stacks (newest first); here they are queues and the few places that
// queue two items in a row do it in the order PPSSPP's stacks deliver them.
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_adhoc_internal.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/thread.h"

namespace net::adhoc {

namespace {

constexpr const char *LOG = "ADHOC";

enum MatchingMode : int { MODE_PARENT = 1, MODE_CHILD = 2, MODE_P2P = 3 };

enum MatchingEvent : int {
	EVENT_HELLO = 1,
	EVENT_REQUEST = 2,
	EVENT_LEAVE = 3,
	EVENT_DENY = 4,
	EVENT_CANCEL = 5,
	EVENT_ACCEPT = 6,
	EVENT_ESTABLISHED = 7,
	EVENT_TIMEOUT = 8,
	EVENT_ERROR = 9,
	EVENT_BYE = 10,
	EVENT_DATA = 11,
	EVENT_DATA_ACK = 12,
	EVENT_DATA_TIMEOUT = 13,
};

enum PeerState : int {
	PEER_SELF = 0,  // ourselves, in a child's list after the accept
	PEER_OFFER = 1,
	PEER_PARENT = 2,
	PEER_CHILD = 3,
	PEER_P2P = 4,
	PEER_INCOMING_REQUEST = 5,
	PEER_OUTGOING_REQUEST = 6,
	PEER_CANCEL_IN_PROGRESS = 7,
};

enum Packet : u8 {
	PACKET_PING = 0,
	PACKET_HELLO = 1,
	PACKET_JOIN = 2,
	PACKET_ACCEPT = 3,
	PACKET_CANCEL = 4,
	PACKET_BULK = 5,
	PACKET_BULK_ABORT = 6,
	PACKET_BIRTH = 7,
	PACKET_DEATH = 8,
	PACKET_BYE = 9,
};

constexpr u64 kDefaultKeepAliveUs = 2000000;  // PSP_ADHOCCTL_PING_TIMEOUT
constexpr u64 kEventSpacingUs = 3000;         // between two handler calls
constexpr u64 kStartDelayUs = 30000;          // adhocMatchingEventDelay
constexpr u64 kLastRecvDeltaUs = 10000;
constexpr s32 ADHOC_F_ALERTRECV = 0x0020;

using Mac = std::array<u8, 6>;

struct Member {
	Mac mac{};
	int state = PEER_SELF;
	bool sending = false;
	u64 lastping = 0;
};

struct InputMessage {
	u8 opcode;
	Mac mac;
	std::vector<u8> opt;
};

struct Context {
	int id = 0;
	int mode = 0;
	int maxpeers = 0;
	u16 port = 0;
	int rxbuflen = 0;
	u32 helloInt = 0;
	u32 keepaliveInt = 0;
	u32 resendInt = 0;
	int resendCounter = 0;
	int keepaliveCounter = 0;
	u64 timeout = 0;
	u32 handler = 0;
	std::vector<u8> hello;
	u32 helloAddr = 0;
	std::vector<Member> peers;  // newest first
	std::vector<std::pair<Mac, u16>> peerPorts;
	Mac mac{};
	int socket = -1;
	bool running = false;
	std::vector<u8> rxbuf;
	std::deque<InputMessage> input;
	net::HandlerThread events;
	bool inputRunning = false;
	u32 inputGen = 0;  // which start the input thread belongs to
	u64 abortKey = 0;
	u64 lastHello = 0, lastPing = 0;
};

bool g_inited = false;
u32 g_poolSize = 0;
std::vector<std::shared_ptr<Context>> g_contexts;  // newest first
u64 g_nextAbortKey = 1;

bool CanWait() { return sched::Current() != nullptr && !sched::InInterrupt() && sched::DispatchEnabled(); }
void Sleep(u64 us) {
	if (CanWait()) sched::WaitUntil(kernel::WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
}

Mac ToMac(const u8 *p) {
	Mac m;
	memcpy(m.data(), p, 6);
	return m;
}

void PutLE32(std::vector<u8> &v, size_t at, s32 x) { memcpy(v.data() + at, &x, 4); }
s32 GetLE32(const u8 *p) {
	s32 x;
	memcpy(&x, p, 4);
	return x;
}

std::shared_ptr<Context> FindContext(int id) {
	for (auto &c : g_contexts)
		if (c->id == id) return c;
	return nullptr;
}

int FreeMatchingId() {
	int maxId = 0;
	for (auto &c : g_contexts) maxId = std::max(maxId, c->id);
	for (int i = 1; i < maxId; i++)
		if (!FindContext(i)) return i;
	return maxId + 1;
}

// ---- members

Member *FindMember(Context *c, const u8 *mac) {
	for (Member &m : c->peers)
		if (MacMatch(m.mac.data(), mac)) return &m;
	return nullptr;
}
Member *FindByState(Context *c, int state, bool excludeTimedOut = false) {
	for (Member &m : c->peers)
		if (m.state == state && (!excludeTimedOut || m.lastping != 0)) return &m;
	return nullptr;
}
Member *FindParent(Context *c) { return FindByState(c, PEER_PARENT); }
Member *FindP2P(Context *c, bool excludeTimedOut = false) { return FindByState(c, PEER_P2P, excludeTimedOut); }
Member *FindOutgoingRequest(Context *c) { return FindByState(c, PEER_OUTGOING_REQUEST); }

int CountChildren(Context *c, bool excludeTimedOut = false) {
	int n = 0;
	for (Member &m : c->peers)
		if (m.state == PEER_CHILD && (!excludeTimedOut || m.lastping != 0)) n++;
	return n;
}

int CountConnectedPeers(Context *c, bool excludeTimedOut = false) {
	if (c->mode == MODE_PARENT) return CountChildren(c, excludeTimedOut) + 1;
	if (c->mode == MODE_CHILD) {
		int n = 1;
		// Siblings + the parent (Ys vs. Sora no Kiseki shows the right player count this way).
		if (FindParent(c)) n += CountChildren(c, excludeTimedOut) + 1;
		return n;
	}
	return FindP2P(c, excludeTimedOut) ? 2 : 1;
}

void DeleteMember(Context *c, const u8 *mac) {
	auto &p = c->peers;
	for (auto it = p.begin(); it != p.end(); ++it) {
		if (MacMatch(it->mac.data(), mac)) {
			LOGI(LOG, "Matching %d: member %s removed", c->id, MacStr(mac).c_str());
			p.erase(it);
			return;
		}
	}
}

Member *AddMember(Context *c, const u8 *mac, int state) {
	if (Member *m = FindMember(c, mac)) {
		m->state = state;
		m->sending = false;
		m->lastping = sched::NowUs();
		return m;
	}
	Member m;
	m.mac = ToMac(mac);
	m.state = state;
	m.lastping = sched::NowUs();
	c->peers.insert(c->peers.begin(), m);
	return &c->peers.front();
}

u16 PeerPort(Context *c, const u8 *mac) {
	for (auto &p : c->peerPorts)
		if (MacMatch(p.first.data(), mac)) return p.second;
	return c->port;
}
void SetPeerPort(Context *c, const u8 *mac, u16 port) {
	for (auto &p : c->peerPorts)
		if (MacMatch(p.first.data(), mac)) {
			p.second = port;
			return;
		}
	c->peerPorts.push_back({ToMac(mac), port});
}

// ---- events (to the event thread) and messages (to the input thread)

void CallEvent(std::weak_ptr<Context> wc, int event, Mac mac, std::vector<u8> opt) {
	auto c = wc.lock();
	if (!c || !c->running) return;
	// No HELLO in the middle of joining: Super Pocket Tennis would try to join again.
	Member *peer = FindMember(c.get(), mac.data());
	if (event == EVENT_HELLO && peer &&
	    (peer->state == PEER_OUTGOING_REQUEST || peer->state == PEER_INCOMING_REQUEST || peer->state == PEER_CANCEL_IN_PROGRESS)) {
		LOGD(LOG, "Matching %d: HELLO from %s dropped while joining", c->id, MacStr(mac.data()).c_str());
		return;
	}
	u32 size = (u32)opt.size() + 8;
	u32 buf = kernel::g_userMem.Alloc(size, true, "SceNetAdhocMatching event");
	if (!buf) {
		LOGW(LOG, "Matching %d: no memory for event %d", c->id, event);
		return;
	}
	mem::Memset(buf, 0, size);
	mem::WriteBytes(buf, mac.data(), 6);
	if (!opt.empty()) mem::WriteBytes(buf + 8, opt.data(), (u32)opt.size());
	LOGI(LOG, "Matching %d: event %d from %s (%zu bytes)", c->id, event, MacStr(mac.data()).c_str(), opt.size());
	int id = c->id;
	u32 handler = c->handler;
	c.reset();  // the handler may delete the context
	net::HandlerThread::CallHandler(handler, {(u32)id, (u32)event, buf, (u32)opt.size(), buf + 8});
	kernel::g_userMem.Free(buf);
	Sleep(kEventSpacingUs);
}

void SpawnLocalEvent(Context *c, int event, const u8 *mac, int optlen, const void *opt) {
	std::vector<u8> data;
	if (optlen > 0 && opt) data.assign((const u8 *)opt, (const u8 *)opt + optlen);
	std::weak_ptr<Context> wc;
	for (auto &x : g_contexts)
		if (x.get() == c) wc = x;
	Mac m = ToMac(mac);
	c->events.Post([wc, event, m, data] { CallEvent(wc, event, m, data); });
}

void QueueMessage(Context *c, u8 opcode, const u8 *mac, int optlen = 0, const void *opt = nullptr) {
	InputMessage msg;
	msg.opcode = opcode;
	msg.mac = ToMac(mac);
	if (optlen > 0 && opt) msg.opt.assign((const u8 *)opt, (const u8 *)opt + optlen);
	c->input.push_back(std::move(msg));
}

void Send(Context *c, const u8 *mac, u16 port, const std::vector<u8> &pkt) {
	PdpSendInternal(c->socket, mac, port, pkt.data(), (int)pkt.size());
}

// ---- outgoing packets

void BroadcastPing(Context *c) {
	std::vector<u8> pkt{PACKET_PING};
	for (Peer &p : Peers()) {
		if (p.lastRecv == 0) continue;  // gone
		Send(c, p.mac, PeerPort(c, p.mac), pkt);
	}
}

void BroadcastHello(Context *c) {
	std::vector<u8> pkt(5 + c->hello.size());
	pkt[0] = PACKET_HELLO;
	PutLE32(pkt, 1, (s32)c->hello.size());
	if (!c->hello.empty()) memcpy(pkt.data() + 5, c->hello.data(), c->hello.size());
	// To everybody in the group, so they know there is a room here.
	for (Peer &p : Peers()) {
		if (p.lastRecv == 0) continue;
		Send(c, p.mac, PeerPort(c, p.mac), pkt);
	}
}

void SendAcceptPacket(Context *c, const u8 *mac, const std::vector<u8> &opt) {
	Member *peer = FindMember(c, mac);
	if (!peer || (peer->state != PEER_CHILD && peer->state != PEER_P2P)) return;
	std::vector<Mac> siblings;
	if (c->mode == MODE_PARENT) {
		int n = CountConnectedPeers(c) - 2;
		for (Member &m : c->peers) {
			if ((int)siblings.size() >= n) break;
			if (&m != peer && m.state == PEER_CHILD) siblings.push_back(m.mac);
		}
	}
	std::vector<u8> pkt(9 + opt.size() + siblings.size() * 6);
	pkt[0] = PACKET_ACCEPT;
	PutLE32(pkt, 1, (s32)opt.size());
	PutLE32(pkt, 5, (s32)siblings.size());
	if (!opt.empty()) memcpy(pkt.data() + 9, opt.data(), opt.size());
	for (size_t i = 0; i < siblings.size(); i++) memcpy(pkt.data() + 9 + opt.size() + i * 6, siblings[i].data(), 6);
	Send(c, mac, PeerPort(c, mac), pkt);
	SpawnLocalEvent(c, EVENT_ESTABLISHED, mac, 0, nullptr);
}

void SendJoinPacket(Context *c, const u8 *mac, const std::vector<u8> &opt) {
	Member *peer = FindMember(c, mac);
	if (!peer || peer->state != PEER_OUTGOING_REQUEST) return;
	std::vector<u8> pkt(5 + opt.size());
	pkt[0] = PACKET_JOIN;
	PutLE32(pkt, 1, (s32)opt.size());
	if (!opt.empty()) memcpy(pkt.data() + 5, opt.data(), opt.size());
	Send(c, mac, PeerPort(c, mac), pkt);
}

void ClearPeers(Context *c) { c->peers.clear(); }

void SendCancelPacket(Context *c, const u8 *mac, const std::vector<u8> &opt) {
	std::vector<u8> pkt(5 + opt.size());
	pkt[0] = PACKET_CANCEL;
	PutLE32(pkt, 1, (s32)opt.size());
	if (!opt.empty()) memcpy(pkt.data() + 5, opt.data(), opt.size());
	Send(c, mac, PeerPort(c, mac), pkt);
	Member *peer = FindMember(c, mac);
	if (!peer) return;
	if (c->mode == MODE_CHILD) {
		ClearPeers(c);
	} else {
		// Kept for a while (it times out), so the BYE on stopping still reaches it.
		peer->lastping = sched::NowUs();
	}
}

void SendBulkDataPacket(Context *c, const u8 *mac, const std::vector<u8> &data) {
	Member *peer = FindMember(c, mac);
	if (!peer) return;
	std::vector<u8> pkt(5 + data.size());
	pkt[0] = PACKET_BULK;
	PutLE32(pkt, 1, (s32)data.size());
	if (!data.empty()) memcpy(pkt.data() + 5, data.data(), data.size());
	Send(c, mac, PeerPort(c, mac), pkt);
	peer->sending = false;
	SpawnLocalEvent(c, EVENT_DATA_ACK, mac, 0, nullptr);
}

void SendBirthPacket(Context *c, const u8 *mac) {
	Member *newborn = FindMember(c, mac);
	if (!newborn) return;
	std::vector<u8> pkt(7);
	pkt[0] = PACKET_BIRTH;
	memcpy(pkt.data() + 1, mac, 6);
	for (Member &m : c->peers) {
		if (&m == newborn || m.state != PEER_CHILD) continue;
		Send(c, m.mac.data(), PeerPort(c, m.mac.data()), pkt);
	}
}

void SendDeathPacket(Context *c, const u8 *mac) {
	Member *dead = FindMember(c, mac);
	if (!dead) return;
	std::vector<u8> pkt(7);
	memcpy(pkt.data() + 1, mac, 6);
	for (Member &m : c->peers) {
		if (&m == dead) {
			// The abandoned child hears BYE.
			std::vector<u8> bye{PACKET_BYE};
			Send(c, m.mac.data(), PeerPort(c, m.mac.data()), bye);
		} else if (m.state == PEER_CHILD) {
			pkt[0] = PACKET_DEATH;
			Send(c, m.mac.data(), PeerPort(c, m.mac.data()), pkt);
		}
	}
	DeleteMember(c, mac);
}

void SendByePacket(Context *c) {
	std::vector<u8> bye{PACKET_BYE};
	for (Member &m : c->peers) {
		if (m.state == PEER_PARENT || m.state == PEER_CHILD || m.state == PEER_P2P || m.state == PEER_CANCEL_IN_PROGRESS)
			Send(c, m.mac.data(), PeerPort(c, m.mac.data()), bye);
	}
}

void ProcessInput(Context *c) {
	while (!c->input.empty()) {
		InputMessage msg = std::move(c->input.front());
		c->input.pop_front();
		switch (msg.opcode) {
		case PACKET_ACCEPT: SendAcceptPacket(c, msg.mac.data(), msg.opt); break;
		case PACKET_JOIN: SendJoinPacket(c, msg.mac.data(), msg.opt); break;
		case PACKET_CANCEL: SendCancelPacket(c, msg.mac.data(), msg.opt); break;
		case PACKET_BULK: SendBulkDataPacket(c, msg.mac.data(), msg.opt); break;
		case PACKET_BIRTH: SendBirthPacket(c, msg.mac.data()); break;
		case PACKET_DEATH: SendDeathPacket(c, msg.mac.data()); break;
		default: break;  // BULK_ABORT: fire and forget, nothing to abort
		}
	}
}

void HandleTimeouts(Context *c) {
	u64 now = sched::NowUs();
	std::vector<Mac> macs;
	for (Member &m : c->peers) macs.push_back(m.mac);
	for (const Mac &mac : macs) {
		Member *m = FindMember(c, mac.data());
		if (!m || m->state == PEER_SELF || (s64)(now - m->lastping) <= (s64)c->timeout) continue;
		bool interesting = (c->mode == MODE_CHILD && m->state == PEER_PARENT) || (c->mode == MODE_PARENT && m->state == PEER_CHILD) ||
		                   (c->mode == MODE_P2P && (m->state == PEER_P2P || m->state == PEER_OFFER || m->state == PEER_INCOMING_REQUEST ||
		                                           m->state == PEER_OUTGOING_REQUEST || m->state == PEER_CANCEL_IN_PROGRESS));
		if (!interesting) continue;
		LOGI(LOG, "Matching %d: %s timed out", c->id, MacStr(mac.data()).c_str());
		SpawnLocalEvent(c, EVENT_TIMEOUT, mac.data(), 0, nullptr);
		QueueMessage(c, c->mode == MODE_PARENT ? PACKET_DEATH : PACKET_CANCEL, mac.data());
	}
}

// ---- incoming packets

void ActOnPing(Context *c, const u8 *sender) {
	if (Member *m = FindMember(c, sender)) m->lastping = sched::NowUs();
}

void ActOnHello(Context *c, const u8 *sender, int length) {
	if (!((c->mode == MODE_CHILD && !FindParent(c)) || (c->mode == MODE_P2P && !FindP2P(c)))) return;
	if (length < 5) return;
	int optlen = GetLE32(c->rxbuf.data() + 1);
	if (optlen < 0 || optlen > length - 5) return;
	Member *peer = FindMember(c, sender);
	if (!peer) peer = AddMember(c, sender, PEER_OFFER);
	if (peer->state != PEER_OUTGOING_REQUEST && peer->state != PEER_INCOMING_REQUEST)
		SpawnLocalEvent(c, EVENT_HELLO, sender, optlen, optlen > 0 ? c->rxbuf.data() + 5 : nullptr);
}

void ActOnJoin(Context *c, const u8 *sender, int length) {
	if (c->mode == MODE_CHILD) return;
	if ((c->mode == MODE_PARENT && CountChildren(c) < c->maxpeers - 1) || (c->mode == MODE_P2P && !FindP2P(c))) {
		if (length >= 5) {
			int optlen = GetLE32(c->rxbuf.data() + 1);
			if (optlen >= 0 && optlen <= length - 5) {
				const u8 *opt = optlen > 0 ? c->rxbuf.data() + 5 : nullptr;
				Member *peer = FindMember(c, sender);
				// A parent learns of a child only through a join request: a second one is a repeat.
				if (peer && peer->lastping != 0 && c->mode == MODE_PARENT) return;
				if (!peer) {
					AddMember(c, sender, PEER_INCOMING_REQUEST);
				} else {
					peer->state = PEER_INCOMING_REQUEST;  // P2P only
					peer->lastping = sched::NowUs();
				}
				SpawnLocalEvent(c, EVENT_REQUEST, sender, optlen, opt);
				return;
			}
		}
	}
	// No room: reject.
	SendCancelPacket(c, sender, {});
}

void ActOnAccept(Context *c, const u8 *sender, u32 length) {
	if (c->mode == MODE_PARENT) return;
	if (!((c->mode == MODE_CHILD && !FindParent(c)) || (c->mode == MODE_P2P && !FindP2P(c)))) return;
	if (length < 9) return;
	int optlen = GetLE32(c->rxbuf.data() + 1);
	int siblingCount = GetLE32(c->rxbuf.data() + 5);
	if (optlen < 0 || siblingCount < 0 || (u64)length < 9ull + (u64)optlen + 6ull * (u64)siblingCount) return;
	std::vector<u8> opt(c->rxbuf.begin() + 9, c->rxbuf.begin() + 9 + optlen);
	std::vector<Mac> siblings;
	for (int i = 0; i < siblingCount; i++) siblings.push_back(ToMac(c->rxbuf.data() + 9 + optlen + i * 6));
	Member *request = FindOutgoingRequest(c);
	if (!request) return;
	Member *peer = FindMember(c, sender);
	if (request != peer) return;  // not the answer we wait for
	peer->state = c->mode == MODE_CHILD ? PEER_PARENT : PEER_P2P;
	// Drop the peers that don't matter any more.
	auto &p = c->peers;
	p.erase(std::remove_if(p.begin(), p.end(),
	                       [](const Member &m) {
		                       return m.state != PEER_CHILD && m.state != PEER_P2P && m.state != PEER_PARENT && m.state != PEER_SELF;
	                       }),
	        p.end());
	if (c->mode == MODE_CHILD) {
		// Siblings, in the parent's order; then ourselves (state 0).
		for (int i = (int)siblings.size() - 1; i >= 0; i--) {
			if (Member *m = FindMember(c, siblings[i].data())) {
				m->state = PEER_CHILD;
				m->sending = false;
				m->lastping = sched::NowUs();
			} else {
				AddMember(c, siblings[i].data(), PEER_CHILD);
			}
		}
		AddMember(c, c->mac.data(), PEER_SELF);
	}
	SpawnLocalEvent(c, EVENT_ACCEPT, sender, optlen, opt.empty() ? nullptr : opt.data());
	SpawnLocalEvent(c, EVENT_ESTABLISHED, sender, 0, nullptr);
}

void ActOnCancel(Context *c, const u8 *sender, int length) {
	Member *peer = FindMember(c, sender);
	if (!peer || length < 5) return;
	int optlen = GetLE32(c->rxbuf.data() + 1);
	if (optlen < 0 || optlen > length - 5) return;
	std::vector<u8> opt(c->rxbuf.begin() + 5, c->rxbuf.begin() + 5 + optlen);
	const u8 *o = opt.empty() ? nullptr : opt.data();
	Member *request = FindOutgoingRequest(c);
	if (c->mode == MODE_CHILD) {
		Member *parent = FindParent(c);
		if (request == peer) {
			SpawnLocalEvent(c, EVENT_DENY, sender, optlen, o);
			peer->lastping = 0;
		} else if (parent == peer) {
			// Kicked out: every established member leaves.
			for (Member &m : c->peers)
				if (m.state == PEER_CHILD || m.state == PEER_PARENT) SpawnLocalEvent(c, EVENT_LEAVE, m.mac.data(), optlen, o);
			ClearPeers(c);
		}
	} else if (c->mode == MODE_PARENT) {
		if (peer->state == PEER_INCOMING_REQUEST) {
			SpawnLocalEvent(c, EVENT_CANCEL, sender, optlen, o);
			peer->lastping = 0;
		} else if (peer->state == PEER_CHILD) {
			SpawnLocalEvent(c, EVENT_LEAVE, sender, optlen, o);
			peer->lastping = 0;
		}
	} else {
		Member *p2p = FindP2P(c);
		if (request == peer) {
			// Kept, timed out (Crazy Taxi's inviter waits for the time-out).
			SpawnLocalEvent(c, EVENT_DENY, sender, optlen, o);
			peer->lastping = 0;
		} else if (p2p == peer) {
			SpawnLocalEvent(c, EVENT_LEAVE, sender, optlen, o);
			peer->lastping = 0;
		} else if (peer->state == PEER_INCOMING_REQUEST) {
			SpawnLocalEvent(c, EVENT_CANCEL, sender, optlen, o);
			peer->lastping = 0;
		}
	}
}

void ActOnBulkData(Context *c, const u8 *sender, int length) {
	Member *peer = FindMember(c, sender);
	if (!peer) return;
	bool established = (c->mode == MODE_PARENT && peer->state == PEER_CHILD) ||
	                   (c->mode == MODE_CHILD && (peer->state == PEER_CHILD || peer->state == PEER_PARENT)) ||
	                   (c->mode == MODE_P2P && peer->state == PEER_P2P);
	if (!established || length <= 5) return;
	int datalen = GetLE32(c->rxbuf.data() + 1);
	if (datalen > 0 && datalen <= length - 5) SpawnLocalEvent(c, EVENT_DATA, sender, datalen, c->rxbuf.data() + 5);
}

void ActOnBirth(Context *c, const u8 *sender, u32 length) {
	Member *peer = FindMember(c, sender);
	if (!peer || c->mode != MODE_CHILD || peer != FindParent(c) || length < 7) return;
	Mac mac = ToMac(c->rxbuf.data() + 1);
	Member m;
	m.mac = mac;
	m.state = PEER_CHILD;
	m.lastping = sched::NowUs();
	c->peers.insert(c->peers.begin(), m);
}

void ActOnDeath(Context *c, const u8 *sender, u32 length) {
	Member *peer = FindMember(c, sender);
	if (!peer || c->mode != MODE_CHILD || peer != FindParent(c) || length < 7) return;
	Mac mac = ToMac(c->rxbuf.data() + 1);
	Member *dead = FindMember(c, mac.data());
	if (!dead || dead->state != PEER_CHILD) return;
	SpawnLocalEvent(c, EVENT_LEAVE, mac.data(), 0, nullptr);
	DeleteMember(c, mac.data());
}

void ActOnBye(Context *c, const u8 *sender) {
	Member *peer = FindMember(c, sender);
	if (!peer) return;
	int st = peer->state;
	if ((c->mode == MODE_PARENT && st == PEER_CHILD) || (c->mode == MODE_CHILD && st == PEER_CHILD) ||
	    (c->mode == MODE_P2P && (st == PEER_P2P || st == PEER_OFFER || st == PEER_INCOMING_REQUEST || st == PEER_OUTGOING_REQUEST ||
	                            st == PEER_CANCEL_IN_PROGRESS))) {
		if (c->mode != MODE_CHILD) SpawnLocalEvent(c, EVENT_BYE, sender, 0, nullptr);
		DeleteMember(c, sender);
	} else if (c->mode == MODE_CHILD && st == PEER_PARENT) {
		SpawnLocalEvent(c, EVENT_BYE, sender, 0, nullptr);
		ClearPeers(c);
	}
}

// ---- the input thread

void ReceiveAll(Context *c) {
	for (int n = 0; n < 64; n++) {
		u8 sender[6];
		u16 sport = 0;
		int len = c->rxbuflen;
		int r = PdpRecvInternal(c->socket, sender, &sport, c->rxbuf.data(), &len);
		if (r == (int)SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE) {
			// Bigger than the receive buffer: it would block the socket for good.
			std::vector<u8> big((size_t)len);
			int blen = len;
			PdpRecvInternal(c->socket, sender, &sport, big.data(), &blen);
			LOGW(LOG, "Matching %d: dropped a %d byte packet (receive buffer %d)", c->id, blen, c->rxbuflen);
			continue;
		}
		if (r != 0 || len <= 0) return;
		if (Peer *p = FindPeer(sender)) {
			u64 now = sched::NowUs();
			if (p->lastRecv != 0) p->lastRecv = std::max(p->lastRecv, now > kLastRecvDeltaUs ? now - kLastRecvDeltaUs : 0);
		}
		// Ports may be re-mapped by a router: answer where it came from.
		SetPeerPort(c, sender, sport);
		switch (c->rxbuf[0]) {
		case PACKET_PING: ActOnPing(c, sender); break;
		case PACKET_HELLO: ActOnHello(c, sender, len); break;
		case PACKET_JOIN: ActOnJoin(c, sender, len); break;
		case PACKET_ACCEPT: ActOnAccept(c, sender, (u32)len); break;
		case PACKET_CANCEL: ActOnCancel(c, sender, len); break;
		case PACKET_BULK: ActOnBulkData(c, sender, len); break;
		case PACKET_BIRTH: ActOnBirth(c, sender, (u32)len); break;
		case PACKET_DEATH: ActOnDeath(c, sender, (u32)len); break;
		case PACKET_BYE: ActOnBye(c, sender); break;
		default: break;
		}
	}
}

void InputStep(Context *c) {
	u64 now = sched::NowUs();
	if ((c->mode == MODE_PARENT && CountChildren(c) < c->maxpeers - 1) || (c->mode == MODE_P2P && !FindP2P(c))) {
		if (c->helloInt > 0 && (s64)(now - c->lastHello) >= (s64)c->helloInt) {
			BroadcastHello(c);
			c->lastHello = now;
		}
	}
	if (c->keepaliveInt > 0) {
		if ((s64)(now - c->lastPing) >= (s64)c->keepaliveInt) {
			HandleTimeouts(c);
			BroadcastPing(c);
			c->lastPing = now;
		}
	} else {
		HandleTimeouts(c);
	}
	ProcessInput(c);
	ReceiveAll(c);
}

void InputThread(std::weak_ptr<Context> wc, u32 gen) {
	for (;;) {
		auto c = wc.lock();
		if (!c || !c->inputRunning || c->inputGen != gen) return;
		InputStep(c.get());
		if (!c->inputRunning || c->inputGen != gen) return;
		WSAEVENT ev = SocketEvent(c->socket);
		u64 key = c->abortKey;
		c.reset();
		net::WaitHostEvents(&ev, ev != WSA_INVALID_EVENT ? 1 : 0, 0, key, 10000);
	}
}

// ---- the library

void StopContext(Context *c) {
	if (!c->running) return;
	// Ys vs. Sora no Kiseki: receiving on the socket now reports an alert.
	SetSocketAlertInternal(c->socket, ADHOC_F_ALERTRECV);
	c->inputRunning = false;
	net::AbortWaits(c->abortKey);
	// What the input thread does on its way out: the last messages, then goodbye.
	ProcessInput(c);
	SendByePacket(c);
	ClearPeers(c);
	c->events.Stop();
	PdpDeleteInternal(c->socket);
	c->socket = -1;
	ClearPeers(c);
	c->running = false;
}

void DeleteContext(int id) {
	auto c = FindContext(id);
	if (!c) return;
	if (c->running) StopContext(c.get());
	g_contexts.erase(std::remove(g_contexts.begin(), g_contexts.end(), c), g_contexts.end());
}

int sceNetAdhocMatchingInit(u32 poolSize) {
	if (!net::Enabled()) return offline::MatchingInit(poolSize);
	if (g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_INITIALIZED;
	g_poolSize = poolSize;
	g_inited = true;
	return 0;
}

int sceNetAdhocMatchingTerm() {
	if (!net::Enabled()) return offline::MatchingTerm();
	MatchingShutdown();
	return 0;
}

int sceNetAdhocMatchingCreate(int mode, int maxnum, int port, int rxbuflen, int helloInt, int keepaliveInt, int initCount, int rexmtInt,
                              u32 handler) {
	if (!net::Enabled())
		return offline::MatchingCreate(mode, maxnum, port, rxbuflen, (u32)helloInt, (u32)keepaliveInt, (u32)initCount, (u32)rexmtInt, handler);
	LOGI(LOG, "sceNetAdhocMatchingCreate(mode %d, max %d, port %d, rxbuf %d, hello %d, keepalive %d, count %d, rexmt %d, handler %08x)",
	     mode, maxnum, port, rxbuflen, helloInt, keepaliveInt, initCount, rexmtInt, handler);
	if (!g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (maxnum <= 1 || maxnum > 16) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_MAXNUM;
	if (rxbuflen < 1) return (int)SCE_NET_ADHOC_MATCHING_ERROR_RXBUF_TOO_SHORT;
	if (mode < 1 || mode > 3) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	for (auto &c : g_contexts)
		if (c->port == (u16)port) return (int)SCE_NET_ADHOC_MATCHING_ERROR_PORT_IN_USE;
	auto c = std::make_shared<Context>();
	c->id = FreeMatchingId();
	c->mode = mode;
	c->maxpeers = maxnum;
	c->port = (u16)port;
	c->rxbuflen = rxbuflen;
	c->rxbuf.resize((size_t)rxbuflen);
	c->resendCounter = initCount;
	c->resendInt = (u32)rexmtInt;
	c->helloInt = (u32)helloInt;
	c->keepaliveInt = keepaliveInt < 1 ? (u32)kDefaultKeepAliveUs : (u32)keepaliveInt;
	c->keepaliveCounter = initCount;
	// Time-out: (keep-alive + resend interval) x count, plus half a second for play over the internet.
	c->timeout = ((u64)(u32)keepaliveInt + (u64)(u32)rexmtInt) * (u64)(u32)initCount + 500000;
	c->handler = handler;
	c->mac = ToMac(net::kLocalMac);
	c->abortKey = (4ull << 40) | g_nextAbortKey++;
	g_contexts.insert(g_contexts.begin(), c);
	return c->id;
}

int StartMatching(int id, int evthPri, int evthStack, int inthPri, int inthStack, int optLen, u32 optAddr) {
	auto c = FindContext(id);
	if (!c) return 0;  // GTA VCS needs success here
	if (optLen > 0 && mem::IsValidRange(optAddr, (u32)optLen)) {
		c->hello.assign(GPtr(optAddr), GPtr(optAddr) + optLen);
		c->helloAddr = optAddr;
	}
	int sock = PdpCreateInternal(c->mac.data(), c->port, c->rxbuflen, 0);
	if (sock < 1) {
		LOGW(LOG, "sceNetAdhocMatchingStart(%d): no socket (%08x)", id, (u32)sock);
		return (int)SCE_NET_ADHOC_MATCHING_ERROR_PORT_IN_USE;
	}
	c->socket = sock;
	c->lastHello = c->lastPing = 0;
	c->events.Start("SceNetAdhocMatchingEvent", evthPri, evthStack);
	c->inputRunning = true;
	u32 gen = ++c->inputGen;
	std::weak_ptr<Context> wc = c;
	SceUID th = net::StartLibraryThread("SceNetAdhocMatchingInput", inthPri, inthStack, [wc, gen] { InputThread(wc, gen); });
	if (th < 0) LOGW(LOG, "sceNetAdhocMatchingStart(%d): no input thread (%08x)", id, (u32)th);
	c->running = true;
	LOGI(LOG, "Matching %d started (mode %d, port %u, PDP socket %d)", id, c->mode, c->port, sock);
	// Time for the threads to get going before the game's next call.
	sched::ChargeUs(kStartDelayUs);
	return 0;
}

int sceNetAdhocMatchingStart(int id, int evthPri, int evthStack, int inthPri, int inthStack, u32 optLen, u32 optAddr) {
	if (!net::Enabled()) return offline::MatchingStart(id, optLen, optAddr);
	return StartMatching(id, evthPri, evthStack, inthPri, inthStack, (int)optLen, optAddr);
}

int sceNetAdhocMatchingStart2(int id, int evthPri, int evthPartition, int evthStack, int inthPri, int inthPartition, int inthStack,
                              u32 optLen, u32 optAddr) {
	if (!net::Enabled()) return offline::MatchingStart(id, optLen, optAddr);
	return StartMatching(id, evthPri, evthStack, inthPri, inthStack, (int)optLen, optAddr);
}

// Nine arguments: the ninth is passed on the stack.
u32 StackArg0(Ctx *c) { return mem::IsValidRange(c->r[29], 4) ? GRead32(c->r[29]) : 0; }
void sceNetAdhocMatchingCreate_Raw(Ctx *c) {
	c->r[2] = (u32)sceNetAdhocMatchingCreate((int)c->r[4], (int)c->r[5], (int)c->r[6], (int)c->r[7], (int)c->r[8], (int)c->r[9],
	                                          (int)c->r[10], (int)c->r[11], StackArg0(c));
}
void sceNetAdhocMatchingStart2_Raw(Ctx *c) {
	c->r[2] = (u32)sceNetAdhocMatchingStart2((int)c->r[4], (int)c->r[5], (int)c->r[6], (int)c->r[7], (int)c->r[8], (int)c->r[9],
	                                          (int)c->r[10], c->r[11], StackArg0(c));
}

int sceNetAdhocMatchingStop(int id) {
	if (!net::Enabled()) return offline::MatchingStop(id);
	LOGI(LOG, "sceNetAdhocMatchingStop(%d)", id);
	auto c = FindContext(id);
	if (c) StopContext(c.get());
	return 0;
}

int sceNetAdhocMatchingDelete(int id) {
	if (!net::Enabled()) return offline::MatchingDelete(id);
	LOGI(LOG, "sceNetAdhocMatchingDelete(%d)", id);
	DeleteContext(id);
	return 0;
}

// Common checks of the target calls; returns the context or sets *err.
std::shared_ptr<Context> TargetContext(int id, u32 macAddr, int *err) {
	*err = 0;
	if (!g_inited) {
		*err = (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
		return nullptr;
	}
	if (!mem::IsValidRange(macAddr, 6)) {
		*err = (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
		return nullptr;
	}
	auto c = FindContext(id);
	if (!c) {
		*err = (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
		return nullptr;
	}
	if (!c->running) {
		*err = (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
		return nullptr;
	}
	return c;
}

int sceNetAdhocMatchingSelectTarget(int id, u32 macAddr, int optLen, u32 optAddr) {
	if (!net::Enabled()) return offline::MatchingTarget(id, macAddr);
	int err;
	auto cp = TargetContext(id, macAddr, &err);
	if (!cp) return err;
	Context *c = cp.get();
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	LOGI(LOG, "sceNetAdhocMatchingSelectTarget(%d, %s, %d)", id, MacStr(mac).c_str(), optLen);
	Member *peer = FindMember(c, mac);
	if (!peer) return (int)SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
	if (optLen != 0 && (optLen <= 0 || optAddr == 0)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
	std::vector<u8> opt;
	if (optLen > 0 && mem::IsValidRange(optAddr, (u32)optLen)) opt.assign(GPtr(optAddr), GPtr(optAddr) + optLen);
	if (c->mode == MODE_PARENT) {
		if (peer->state == PEER_CHILD) return (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_ESTABLISHED;
		if (CountChildren(c) == c->maxpeers - 1) return (int)SCE_NET_ADHOC_MATCHING_ERROR_EXCEED_MAXNUM;
		if (peer->state == PEER_INCOMING_REQUEST) {
			peer->state = PEER_CHILD;
			// The accept goes out first, then the other children hear of their new sibling.
			QueueMessage(c, PACKET_ACCEPT, mac, (int)opt.size(), opt.data());
			QueueMessage(c, PACKET_BIRTH, mac);
			return 0;
		}
	} else if (c->mode == MODE_CHILD) {
		if (FindParent(c)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_ESTABLISHED;
		if (FindOutgoingRequest(c)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_REQUEST_IN_PROGRESS;
		if (peer->state == PEER_OFFER) {
			peer->state = PEER_OUTGOING_REQUEST;
			QueueMessage(c, PACKET_JOIN, mac, (int)opt.size(), opt.data());
			return 0;
		}
	} else {
		if (FindP2P(c)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_ESTABLISHED;
		if (FindOutgoingRequest(c)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_REQUEST_IN_PROGRESS;
		if (peer->state == PEER_OFFER) {
			peer->state = PEER_OUTGOING_REQUEST;
			QueueMessage(c, PACKET_JOIN, mac, (int)opt.size(), opt.data());
			return 0;
		}
		if (peer->state == PEER_INCOMING_REQUEST) {
			peer->state = PEER_P2P;
			QueueMessage(c, PACKET_ACCEPT, mac, (int)opt.size(), opt.data());
			return 0;
		}
	}
	return (int)SCE_NET_ADHOC_MATCHING_ERROR_TARGET_NOT_READY;
}

int CancelTarget(int id, u32 macAddr, int optLen, u32 optAddr) {
	if (!g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	bool optValid = optAddr && mem::IsValidAddress(optAddr);
	if (!mem::IsValidRange(macAddr, 6) || (optLen != 0 && (optLen <= 0 || !optValid))) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	auto cp = FindContext(id);
	if (!cp) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
	Context *c = cp.get();
	if (!c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	LOGI(LOG, "sceNetAdhocMatchingCancelTarget(%d, %s, %d)", id, MacStr(mac).c_str(), optLen);
	Member *peer = FindMember(c, mac);
	// Unknown target: success (Soul Calibur calls it again and again otherwise).
	if (!peer) return 0;
	std::vector<u8> opt;
	if (optLen > 0 && mem::IsValidRange(optAddr, (u32)optLen)) opt.assign(GPtr(optAddr), GPtr(optAddr) + optLen);
	int st = peer->state;
	if ((c->mode == MODE_CHILD && (st == PEER_PARENT || st == PEER_OUTGOING_REQUEST)) ||
	    (c->mode == MODE_PARENT && (st == PEER_CHILD || st == PEER_INCOMING_REQUEST)) ||
	    (c->mode == MODE_P2P && (st == PEER_P2P || st == PEER_INCOMING_REQUEST))) {
		bool death = c->mode == MODE_PARENT && st == PEER_CHILD && CountConnectedPeers(c) > 1;
		peer->state = PEER_CANCEL_IN_PROGRESS;
		// The cancel first, then the other children hear of the departure.
		QueueMessage(c, PACKET_CANCEL, mac, (int)opt.size(), opt.data());
		if (death) QueueMessage(c, PACKET_DEATH, mac);
		// Not deleted now: the input thread still needs it; it times out.
		peer->lastping = 0;
		sched::ChargeCycles(10000);
	}
	return 0;
}

int sceNetAdhocMatchingCancelTarget(int id, u32 macAddr) {
	if (!net::Enabled()) return offline::MatchingTarget(id, macAddr);
	return CancelTarget(id, macAddr, 0, 0);
}

int sceNetAdhocMatchingCancelTargetWithOpt(int id, u32 macAddr, int optLen, u32 optAddr) {
	if (!net::Enabled()) return offline::MatchingTarget(id, macAddr);
	return CancelTarget(id, macAddr, optLen, optAddr);
}

int sceNetAdhocMatchingGetHelloOpt(int id, u32 optLenAddr, u32 optAddr) {
	if (!net::Enabled()) return offline::MatchingGetHelloOpt(id, optLenAddr, optAddr);
	if (!mem::IsValidRange(optLenAddr, 4)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	if (auto c = FindContext(id)) {
		GWrite32(optLenAddr, (u32)c->hello.size());
		if (!c->hello.empty() && mem::IsValidRange(optAddr, (u32)c->hello.size()))
			mem::WriteBytes(optAddr, c->hello.data(), (u32)c->hello.size());
	}
	return 0;
}

int sceNetAdhocMatchingSetHelloOpt(int id, int optLen, u32 optAddr) {
	if (!net::Enabled()) return offline::MatchingSetHelloOpt(id, optLen, optAddr);
	if (!g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	auto c = FindContext(id);
	if (!c) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
	if (c->mode == MODE_CHILD) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_MODE;
	if (!c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
	if (optLen != 0 && optAddr == 0) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
	if (optLen > 0) {
		if (!mem::IsValidRange(optAddr, (u32)optLen)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
		c->hello.assign(GPtr(optAddr), GPtr(optAddr) + optLen);
		c->helloAddr = optAddr;
	} else {
		c->hello.clear();
		c->helloAddr = 0;
	}
	return 0;
}

int sceNetAdhocMatchingGetMembers(int id, u32 sizeAddr, u32 bufAddr) {
	if (!net::Enabled()) return offline::MatchingGetMembers(id, sizeAddr, bufAddr);
	if (!g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	auto cp = FindContext(id);
	if (!cp) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
	Context *c = cp.get();
	if (!c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
	constexpr u32 kEntry = 12;  // next, MAC, 2 bytes of padding
	int peercount = CountConnectedPeers(c, false);
	int available = (int)kEntry * peercount;
	if (!bufAddr || !mem::IsValidAddress(bufAddr)) {
		GWrite32(sizeAddr, (u32)available);
		Sleep(100);
		return 0;
	}
	int room = std::max(0, std::min((int)GRead32(sizeAddr), available));
	if (room > 0 && mem::IsValidRange(bufAddr, (u32)room)) mem::Memset(bufAddr, 0, (u32)room);
	int requested = room / (int)kEntry;
	std::vector<Mac> out;
	u64 now = sched::NowUs();
	auto freshen = [&](Member *m) {
		Peer *f = FindPeer(m->mac.data());
		if (m->lastping != 0 && f && f->lastRecv != 0) m->lastping = std::max(m->lastping, now > kLastRecvDeltaUs ? now - kLastRecvDeltaUs : 0);
		else m->lastping = 0;
	};
	if (requested > 0) {
		// Ourselves first, unless a child that has a parent (the parent comes first then).
		if (peercount == 1 || c->mode != MODE_CHILD) out.push_back(c->mac);
		if (requested > 1) {
			if (c->mode == MODE_P2P) {
				if (Member *p2p = FindP2P(c, false)) {
					freshen(p2p);
					out.push_back(p2p->mac);
				}
			} else {
				if (Member *parent = FindParent(c)) {
					freshen(parent);
					out.push_back(parent->mac);
				}
				// Oldest first: the last to join comes last.
				std::vector<Member *> sorted;
				for (Member &m : c->peers) {
					freshen(&m);
					sorted.insert(sorted.begin(), &m);
				}
				for (Member *m : sorted) {
					if ((int)out.size() >= requested) break;
					if (m->state == PEER_CHILD) out.push_back(m->mac);
					else if (c->mode == MODE_CHILD && m->state == PEER_SELF) out.push_back(m->mac);
				}
			}
		}
	}
	int filled = std::min((int)out.size(), requested);
	for (int i = 0; i < filled; i++) {
		u32 e = bufAddr + (u32)i * kEntry;
		GWrite32(e, i + 1 < filled ? e + kEntry : 0);
		mem::WriteBytes(e + 4, out[i].data(), 6);
	}
	GWrite32(sizeAddr, (u32)(filled * (int)kEntry));
	Sleep(100);
	return 0;
}

int sceNetAdhocMatchingSendData(int id, u32 macAddr, int dataLen, u32 dataAddr) {
	if (!net::Enabled()) return offline::MatchingTarget(id, macAddr);
	if (!g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	auto cp = FindContext(id);
	if (!cp) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
	Context *c = cp.get();
	if (!c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
	if (dataLen <= 0 || dataAddr == 0 || !mem::IsValidRange(dataAddr, (u32)dataLen)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_DATALEN;
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	Member *peer = FindMember(c, mac);
	if (!peer) return (int)SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
	if (peer->state != PEER_PARENT && peer->state != PEER_CHILD && peer->state != PEER_P2P)
		return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_ESTABLISHED;
	if (peer->sending) return (int)SCE_NET_ADHOC_MATCHING_ERROR_DATA_BUSY;
	peer->sending = true;
	std::vector<u8> data(GPtr(dataAddr), GPtr(dataAddr) + dataLen);
	// Sent right away (PPSSPP does it from the call as well).
	SendBulkDataPacket(c, peer->mac.data(), data);
	return 0;
}

int sceNetAdhocMatchingAbortSendData(int id, u32 macAddr) {
	if (!net::Enabled()) return offline::MatchingTarget(id, macAddr);
	if (!g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	auto cp = FindContext(id);
	if (!cp) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
	Context *c = cp.get();
	if (!c->running) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	Member *peer = FindMember(c, mac);
	if (!peer) return (int)SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
	if (peer->sending) {
		peer->sending = false;
		QueueMessage(c, PACKET_BULK_ABORT, mac);
	}
	return 0;
}

int sceNetAdhocMatchingGetPoolMaxAlloc() {
	if (!net::Enabled()) return offline::MatchingGetPoolMaxAlloc();
	return (int)(g_poolSize / 2);
}

int sceNetAdhocMatchingGetPoolStat(u32 statAddr) {
	if (!net::Enabled()) return offline::MatchingGetPoolStat(statAddr);
	if (!g_inited) return (int)SCE_NET_ADHOC_MATCHING_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(statAddr, 12)) return (int)SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
	// pool size, maximum used (faked: half), free
	GWrite32(statAddr, g_poolSize);
	GWrite32(statAddr + 4, g_poolSize / 2);
	GWrite32(statAddr + 8, g_poolSize - g_poolSize / 2);
	return 0;
}

}  // namespace

void MatchingShutdown() {
	if (!g_inited) return;
	std::vector<int> ids;
	for (auto &c : g_contexts) ids.push_back(c->id);
	for (int id : ids) DeleteContext(id);
	g_contexts.clear();
	g_inited = false;
}

void RegisterMatchingLibrary() {
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
}

}  // namespace net::adhoc
