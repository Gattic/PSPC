// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceNetAdhoc: the PSP's ad hoc sockets, carried over the host's network like PPSSPP does it.
//   PDP (datagrams) -> UDP, PTP (reliable streams) -> TCP, both on the ad hoc interface address
//   (net_adhocctl.cpp: 127.0.0.<instance> with a local ad hoc server) and at PSP port + 10000.
//   A peer's MAC becomes its IP through the group's member list the ad hoc server sent; data from an
//   address that isn't a member is dropped.
// Host sockets are non-blocking; a blocking call sleeps its guest thread on the socket's readiness
// event (net::WaitHostEvents) and honours the time-out (microseconds, 0 = none), socket alerts and
// deletion. Also here: polling, the socket lists, game mode's master / replica data, and
// sceNetAdhocDiscover.
//
// Argument checks, error codes and their order, structure layouts, PTP connection states and the
// game mode synchronisation are ported from PPSSPP (Core/HLE/sceNetAdhoc.cpp, proAdhoc.cpp),
// Copyright (c) 2013- PPSSPP Project, GPL-2.0-or-later, itself based on Coldbird's aemu.
// PPSSPP's relay ("aemu postoffice") mode and UPnP are not ported.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_adhoc_internal.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

namespace net::adhoc {

namespace {

constexpr const char *LOG = "ADHOC";
constexpr u32 ERROR_NET_NO_SPACE = 0x80410001;

enum SockType : int { SOCK_PDP = 1, SOCK_PTP = 2 };
enum PtpState : int { PTP_CLOSED = 0, PTP_LISTEN = 1, PTP_SYN_SENT = 2, PTP_SYN_RCVD = 3, PTP_ESTABLISHED = 4 };

constexpr s32 ADHOC_F_NONBLOCK = 0x0001;
constexpr s32 ADHOC_F_ALERTSEND = 0x0010;
constexpr s32 ADHOC_F_ALERTRECV = 0x0020;
constexpr s32 ADHOC_F_ALERTPOLL = 0x0040;
constexpr s32 ADHOC_F_ALERTCONNECT = 0x0080;
constexpr s32 ADHOC_F_ALERTACCEPT = 0x0100;
constexpr s32 ADHOC_F_ALERTFLUSH = 0x0200;
constexpr s32 ADHOC_F_ALERTALL = 0x03F0;

constexpr s32 ADHOC_EV_SEND = 0x0001;
constexpr s32 ADHOC_EV_RECV = 0x0002;
constexpr s32 ADHOC_EV_CONNECT = 0x0004;
constexpr s32 ADHOC_EV_ACCEPT = 0x0008;
constexpr s32 ADHOC_EV_INVALID = 0x0100;
constexpr s32 ADHOC_EV_ALERT = 0x0400;
constexpr s32 ADHOC_EV_DISCONNECT = 0x0800;

constexpr int PSP_ADHOC_PTP_MSS = 1444;

#pragma pack(push, 1)
struct PdpStat {
	u32 next;
	s32 id;
	u8 laddr[6];
	u16 lport;
	u32 rcvSbCc;
};
struct PtpStat {
	u32 next;
	s32 id;
	u8 laddr[6];
	u8 paddr[6];
	u16 lport;
	u16 pport;
	u32 sndSbCc;
	u32 rcvSbCc;
	s32 state;
};
struct PollSd {
	s32 id;
	s32 events;
	s32 revents;
};
struct GameModeUpdateInfo {
	u32 length;
	s32 updated;
	u64 timeStamp;
};
#pragma pack(pop)
static_assert(sizeof(PdpStat) == 20, "SceNetAdhocPdpStat");
static_assert(sizeof(PtpStat) == 36, "SceNetAdhocPtpStat");
static_assert(sizeof(PollSd) == 12, "SceNetAdhocPollSd");
static_assert(sizeof(GameModeUpdateInfo) == 16, "GameModeUpdateInfo");

struct AdhocSocket {
	int type = SOCK_PDP;
	s32 flags = 0;          // alert flags (sceNetAdhocSetSocketAlert)
	s32 alertedFlags = 0;
	s32 nonblocking = 0;    // the last call's flag
	u32 bufferSize = 0;
	s32 retryInterval = 0;  // PTP: rexmt_int (us)
	s32 retryCount = 0;
	int attemptCount = 0;   // PTP connect attempts (tells "never tried" from "connection lost")
	u64 lastAttempt = 0;
	u64 internalLastAttempt = 0;
	bool isClient = false;  // created with port 0
	hostnet::Socket host;
	u64 abortKey = 0;
	u8 laddr[6] = {};
	u8 paddr[6] = {};
	u16 lport = 0, pport = 0;
	int state = PTP_CLOSED;
};

bool g_inited = false;
std::unique_ptr<AdhocSocket> g_sockets[kMaxSockets];
std::vector<char> g_peek(65536);
u64 g_nextAbortKey = 1;

bool CanWait() { return sched::Current() != nullptr && !sched::InInterrupt() && sched::DispatchEnabled(); }
void Sleep(u64 us) {
	if (CanWait()) sched::WaitUntil(kernel::WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
}

AdhocSocket *GetSocket(int id) {
	if (id < 1 || id > kMaxSockets) return nullptr;
	return g_sockets[id - 1].get();
}

int FreeSlot() {
	for (int i = 0; i < kMaxSockets; i++)
		if (!g_sockets[i]) return i;
	return -1;
}

// PSP port -> host port: + 10000; a port that would wrap to 0 becomes 65535; 0 stays "any".
u16 HostPort(int port) {
	if (port <= 0) return 0;
	u16 p = (u16)(port + kPortOffset);
	return p == 0 ? 65535 : p;
}
u16 PspPort(u16 hostPort) { return (u16)(hostPort - kPortOffset); }

bool PdpPortInUse(u16 port) {
	for (auto &s : g_sockets)
		if (s && s->type == SOCK_PDP && s->lport == port) return true;
	return false;
}

// Listening and connecting on the same PTP port is allowed; the same listen port twice, or the same
// port to the same peer and port twice, isn't.
bool PtpPortInUse(u16 port, bool forListen, const u8 *dstMac = nullptr, u16 dstPort = 0) {
	for (auto &s : g_sockets) {
		if (!s || s->type != SOCK_PTP || s->lport != port) continue;
		if (forListen && s->state == PTP_LISTEN) return true;
		if (!forListen && s->state != PTP_LISTEN && s->pport == dstPort && dstMac && MacMatch(s->paddr, dstMac)) return true;
	}
	return false;
}

void SetBuffers(SOCKET s, u32 bufferSize) {
	int snd = std::max<int>((int)bufferSize * 5, 1024);
	int rcv = std::max<int>((int)bufferSize * 10, 128);
	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&snd, sizeof(snd));
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof(rcv));
}

// A host socket on the ad hoc interface at `hostPort` (0 = any). Returns the bound port, or 0.
u16 OpenBound(int type, u16 hostPort, u32 bufferSize, hostnet::Socket *out, int *wsaError) {
	*out = hostnet::OpenSocket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP, wsaError);
	if (!out->Valid()) return 0;
	SetBuffers(out->s, bufferSize);
	if (type == SOCK_STREAM) {
		BOOL on = TRUE;
		setsockopt(out->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
		// A PTP port may be listened on and connected from at once.
		setsockopt(out->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
	}
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = LocalIp();
	a.sin_port = htons(hostPort);
	if (bind(out->s, (const sockaddr *)&a, sizeof(a)) != 0) {
		*wsaError = WSAGetLastError();
		LOGW(LOG, "Cannot bind %s:%u (%d)", hostnet::Ipv4ToString(a.sin_addr.s_addr).c_str(), hostPort, *wsaError);
		hostnet::CloseSocket(*out);
		return 0;
	}
	int len = sizeof(a);
	getsockname(out->s, (sockaddr *)&a, &len);
	return ntohs(a.sin_port);
}

int AddSocket(std::unique_ptr<AdhocSocket> sock) {
	int slot = FreeSlot();
	if (slot < 0) return -1;
	sock->abortKey = (3ull << 40) | g_nextAbortKey++;
	net::HostSocketOpened();
	g_sockets[slot] = std::move(sock);
	return slot + 1;
}

void DeleteSocket(int id) {
	AdhocSocket *s = GetSocket(id);
	if (!s) return;
	net::AbortWaits(s->abortKey);
	if (s->host.Valid()) {
		shutdown(s->host.s, SD_RECEIVE);
		hostnet::CloseSocket(s->host);
	}
	net::HostSocketClosed();
	g_sockets[id - 1].reset();
}

// Sleeps until the socket(s) may be ready, `deadline` (0 = none) passes or the socket is aborted.
void WaitSocket(AdhocSocket *s, u64 deadline, u64 recheckUs = 100000) {
	if (!CanWait()) return;
	net::WaitHostEvents(&s->host.ev, 1, deadline, s->abortKey, recheckUs);
}

u64 Deadline(u32 timeoutUs) { return timeoutUs ? sched::NowUs() + timeoutUs : 0; }
bool Expired(u64 deadline) { return deadline && sched::NowUs() >= deadline; }

void WriteMac(u32 addr, const u8 *mac) {
	if (mem::IsValidRange(addr, 6)) mem::WriteBytes(addr, mac, 6);
}

// ---------------------------------------------------------------- PDP

// One datagram, without waiting. 0 = got it; WOULD_BLOCK; NOT_ENOUGH_SPACE (*len = its size, the
// datagram stays queued); TIMEOUT for an unexpected socket error (PPSSPP's choice).
int PdpTryRecv(AdhocSocket *s, u8 *mac, u16 *port, void *buf, int *len) {
	for (int tries = 0; tries < 16; tries++) {
		sockaddr_in from{};
		int fromLen = sizeof(from);
		int n = recvfrom(s->host.s, g_peek.data(), (int)g_peek.size(), MSG_PEEK, (sockaddr *)&from, &fromLen);
		if (n < 0) {
			int e = WSAGetLastError();
			if (e == WSAEWOULDBLOCK || e == WSAECONNRESET) return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
			LOGD(LOG, "PDP recv error %d", e);
			return (int)SCE_NET_ADHOC_ERROR_TIMEOUT;
		}
		u8 sender[6];
		if (!ResolveIp(from.sin_addr.s_addr, sender)) {
			// Not from a member of the group: drop it.
			fromLen = sizeof(from);
			recvfrom(s->host.s, g_peek.data(), (int)g_peek.size(), 0, (sockaddr *)&from, &fromLen);
			LOGD(LOG, "PDP: dropped %d bytes from unknown %s:%u", n, hostnet::Ipv4ToString(from.sin_addr.s_addr).c_str(), ntohs(from.sin_port));
			continue;
		}
		if (n > *len) {
			if (*len > 0) memcpy(buf, g_peek.data(), (size_t)*len);
			*len = n;
			memcpy(mac, sender, 6);
			*port = PspPort(ntohs(from.sin_port));
			TouchPeer(sender);
			return (int)SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE;
		}
		fromLen = sizeof(from);
		n = recvfrom(s->host.s, (char *)buf, std::max(0, *len), 0, (sockaddr *)&from, &fromLen);
		if (n < 0) return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
		*len = n;
		memcpy(mac, sender, 6);
		*port = PspPort(ntohs(from.sin_port));
		TouchPeer(sender);
		return 0;
	}
	return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
}

struct PdpTarget {
	u32 ip;
	u16 port;  // host port
};

int PdpCreate(u32 macAddr, int port, int bufferSize, u32 flag) {
	if (!net::NetInited()) return (int)SCE_LWMUTEX_ERROR_NO_SUCH_LWMUTEX;
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(macAddr, 6) || bufferSize <= 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	if (PdpPortInUse((u16)port)) return (int)SCE_NET_ADHOC_ERROR_PORT_IN_USE;
	// The source MAC is always ours (DBZ Shin Budokai 2 passes one from a save state).
	WriteMac(macAddr, net::kLocalMac);
	if (CtlCurrentMode() == CTLMODE_NONE) return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
	auto sock = std::make_unique<AdhocSocket>();
	int err = 0;
	u16 bound = OpenBound(SOCK_DGRAM, HostPort(port), (u32)bufferSize, &sock->host, &err);
	if (!bound) {
		if (!sock->host.Valid() && err != WSAEADDRINUSE && err != WSAEACCES && err != WSAEADDRNOTAVAIL) return (int)ERROR_NET_NO_SPACE;
		return (int)SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL;
	}
	sock->type = SOCK_PDP;
	sock->nonblocking = (s32)flag;
	sock->bufferSize = (u32)bufferSize;
	sock->isClient = port == 0;
	memcpy(sock->laddr, net::kLocalMac, 6);
	sock->lport = PspPort(bound);
	hostnet::Socket host = sock->host;
	u16 lport = sock->lport;
	int id = AddSocket(std::move(sock));
	if (id < 0) {
		hostnet::CloseSocket(host);
		return (int)ERROR_NET_NO_SPACE;
	}
	LOGI(LOG, "sceNetAdhocPdpCreate(port %d, buffer %d) -> %d (%s:%u)", port, bufferSize, id, hostnet::Ipv4ToString(LocalIp()).c_str(),
	     bound);
	(void)lport;
	return id;
}

int PdpSendTo(AdhocSocket *s, const PdpTarget &t, const void *data, int len, int *wsaError) {
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = t.ip;
	to.sin_port = htons(t.port);
	int n = sendto(s->host.s, (const char *)data, len, 0, (const sockaddr *)&to, sizeof(to));
	*wsaError = n < 0 ? WSAGetLastError() : 0;
	return n;
}

int sceNetAdhocPdpSend(int id, u32 macAddr, u32 port, u32 dataAddr, int len, u32 timeout, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	u16 dport = (u16)port;
	if (dport == 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_PORT;
	if (len < 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_DATALEN;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PDP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->nonblocking = flag;
	if (!dataAddr || (len > 0 && !mem::IsValidRange(dataAddr, (u32)len))) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	if (!mem::IsValidRange(macAddr, 6)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	if (IsZeroMac(mac)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
	if (s->flags & ADHOC_F_ALERTSEND) {
		s->alertedFlags |= ADHOC_F_ALERTSEND;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	const u8 *data = len ? GPtr(dataAddr) : nullptr;
	std::vector<PdpTarget> targets;
	bool broadcast = IsBroadcastMac(mac);
	if (!broadcast) {
		u32 ip;
		if (!ResolveMac(mac, &ip)) {
			LOGD(LOG, "sceNetAdhocPdpSend(%d) to unknown %s: faking success", id, MacStr(mac).c_str());
			return 0;
		}
		targets.push_back(PdpTarget{ip, HostPort(dport)});
	} else {
		for (Peer &p : Peers())
			if (p.lastRecv != 0) targets.push_back(PdpTarget{p.ip, HostPort(dport)});
	}
	u64 deadline = Deadline(timeout);
	for (;;) {
		bool retry = false;
		for (auto it = targets.begin(); it != targets.end();) {
			int e = 0;
			int n = PdpSendTo(s, *it, data, len, &e);
			if (n >= 0 || e != WSAEWOULDBLOCK) {
				if (n < 0) LOGD(LOG, "PDP send error %d", e);
				it = targets.erase(it);
			} else {
				retry = true;
				++it;
			}
		}
		if (!retry) break;
		if (flag) return broadcast ? 0 : (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
		if (Expired(deadline)) return (int)SCE_NET_ADHOC_ERROR_TIMEOUT;
		WaitSocket(s, deadline, 10000);
		s = GetSocket(id);
		if (!s) return (int)SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
		if (s->flags & ADHOC_F_ALERTSEND) {
			s->alertedFlags |= ADHOC_F_ALERTSEND;
			return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
		}
	}
	sched::ChargeUs(50);
	return 0;  // MotorStorm resends unless this is 0
}

int sceNetAdhocPdpRecv(int id, u32 macAddr, u32 portAddr, u32 bufAddr, u32 lenAddr, u32 timeout, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PDP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->nonblocking = flag;
	if (!mem::IsValidRange(macAddr, 6) || !mem::IsValidRange(portAddr, 2) || !mem::IsValidRange(lenAddr, 4) || !mem::IsValidAddress(bufAddr))
		return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	u64 deadline = Deadline(timeout);
	for (;;) {
		if (s->flags & ADHOC_F_ALERTRECV) {
			s->alertedFlags |= ADHOC_F_ALERTRECV;
			return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
		}
		int len = (int)GRead32(lenAddr);
		if (len > 0 && !mem::IsValidRange(bufAddr, (u32)len)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
		u8 mac[6];
		u16 port = 0;
		int r = PdpTryRecv(s, mac, &port, GPtr(bufAddr), &len);
		if (r == 0 || r == (int)SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE) {
			GWrite32(lenAddr, (u32)len);
			WriteMac(macAddr, mac);
			GWrite16(portAddr, port);
			if (r == 0) sched::ChargeUs(50);
			return r;  // 0, not the length (KH BBS)
		}
		if (r != (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK) return r;
		if (flag) return r;
		if (Expired(deadline)) return (int)SCE_NET_ADHOC_ERROR_TIMEOUT;
		WaitSocket(s, deadline);
		s = GetSocket(id);
		if (!s) return (int)SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
	}
}

int sceNetAdhocPdpDelete(int id, int unknown) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (id < 1 || id > kMaxSockets) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PDP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	LOGI(LOG, "sceNetAdhocPdpDelete(%d)", id);
	DeleteSocket(id);
	return 0;
}

// ---------------------------------------------------------------- PTP

// Replaces a PTP socket's host socket after a failed connect (bound to the same port again).
bool RecreatePtp(AdhocSocket *s) {
	hostnet::CloseSocket(s->host, true);
	int err = 0;
	u16 bound = OpenBound(SOCK_STREAM, HostPort(s->lport), s->bufferSize, &s->host, &err);
	if (!bound) {
		bound = OpenBound(SOCK_STREAM, 0, s->bufferSize, &s->host, &err);
		if (!bound) return false;
	}
	s->lport = PspPort(bound);
	return true;
}

enum ConnectProgress { CONNECT_DONE, CONNECT_PENDING };

// One step of a PTP connection: starts it (CLOSED), or checks how it is going (SYN_SENT). A refused
// or failed attempt gets a fresh socket and starts over on the next step, so a peer that starts
// listening later is still reached (PPSSPP's non-Windows behaviour; Windows reports the refusal late).
ConnectProgress ConnectStep(AdhocSocket *s) {
	if (s->state == PTP_ESTABLISHED) return CONNECT_DONE;
	if (s->state == PTP_CLOSED) {
		u32 ip;
		if (!ResolveMac(s->paddr, &ip)) return CONNECT_PENDING;
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = ip;
		to.sin_port = htons(HostPort(s->pport));
		int r = connect(s->host.s, (const sockaddr *)&to, sizeof(to));
		int e = r == 0 ? 0 : WSAGetLastError();
		s->attemptCount++;
		s->lastAttempt = s->internalLastAttempt = sched::NowUs();
		if (r == 0 || e == WSAEISCONN) {
			s->state = PTP_ESTABLISHED;
			return CONNECT_DONE;
		}
		if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY || e == WSAEINVAL) {
			s->state = PTP_SYN_SENT;
			return CONNECT_PENDING;
		}
		LOGD(LOG, "PTP connect to %s:%u failed (%d)", hostnet::Ipv4ToString(ip).c_str(), HostPort(s->pport), e);
		RecreatePtp(s);
		return CONNECT_PENDING;
	}
	if (s->state == PTP_SYN_SENT) {
		hostnet::Readiness r;
		if (!hostnet::CheckReady(s->host.s, &r)) return CONNECT_PENDING;
		if (r.write) {
			sockaddr_in peer{};
			int len = sizeof(peer);
			if (getpeername(s->host.s, (sockaddr *)&peer, &len) == 0) {
				s->state = PTP_ESTABLISHED;
				LOGI(LOG, "PTP connection established to %s:%u", hostnet::Ipv4ToString(peer.sin_addr.s_addr).c_str(), ntohs(peer.sin_port));
				return CONNECT_DONE;
			}
		}
		if (r.except) {
			int e = hostnet::PendingError(s->host.s);
			LOGD(LOG, "PTP connect failed (%d): trying again", e);
			if (sched::NowUs() - s->internalLastAttempt > 16666) {
				RecreatePtp(s);
				s->state = PTP_CLOSED;
				s->internalLastAttempt = sched::NowUs();
			}
		}
	}
	return CONNECT_PENDING;
}

int PtpConnect(int id, u32 timeout, int flag) {
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PTP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->nonblocking = flag;
	if (s->flags & ADHOC_F_ALERTCONNECT) {
		s->alertedFlags |= ADHOC_F_ALERTCONNECT;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	// Phantasy Star Portable 2 connects again when it already is.
	if (s->state == PTP_ESTABLISHED) return 0;
	if (s->state != PTP_CLOSED && s->state != PTP_SYN_SENT) return (int)SCE_NET_ADHOC_ERROR_NOT_OPENED;
	sched::ChargeUs(50);
	u32 ip;
	if (!ResolveMac(s->paddr, &ip)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
	if (ConnectStep(s) == CONNECT_DONE) return 0;
	if (flag) return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
	u64 deadline = Deadline(timeout);
	for (;;) {
		if (Expired(deadline)) return s->nonblocking ? (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK : (int)SCE_NET_ADHOC_ERROR_TIMEOUT;
		WaitSocket(s, deadline, 20000);
		s = GetSocket(id);
		if (!s) return (int)SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
		if (s->flags & ADHOC_F_ALERTCONNECT) {
			s->alertedFlags |= ADHOC_F_ALERTCONNECT;
			return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
		}
		if (ConnectStep(s) == CONNECT_DONE) return 0;
	}
}

int sceNetAdhocPtpOpen(u32 srcMacAddr, int sport, u32 dstMacAddr, int dport, int bufsize, int rexmtInt, int rexmtCnt, int flag) {
	if (!net::Enabled()) return offline::PtpOpen(srcMacAddr, dstMacAddr);
	LOGI(LOG, "sceNetAdhocPtpOpen(port %d -> %d, buffer %d, rexmt %d x %d, flag %d)", sport, dport, bufsize, rexmtInt, rexmtCnt, flag);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	bool srcValid = mem::IsValidRange(srcMacAddr, 6);
	if (srcValid) WriteMac(srcMacAddr, net::kLocalMac);
	u8 dst[6] = {};
	bool dstValid = mem::IsValidRange(dstMacAddr, 6);
	if (dstValid) mem::ReadBytes(dst, dstMacAddr, 6);
	if (CtlCurrentMode() == CTLMODE_NONE || !srcValid || !dstValid || IsBroadcastMac(dst) || IsZeroMac(dst))
		return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
	// Dissidia 012 opens the port again without closing it after a failed connect.
	if (PtpPortInUse((u16)sport, false, dst, (u16)dport)) return (int)SCE_NET_ADHOC_ERROR_PORT_IN_USE;
	if (bufsize <= 0 || rexmtInt <= 0 || rexmtCnt <= 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	auto sock = std::make_unique<AdhocSocket>();
	int err = 0;
	u16 bound = OpenBound(SOCK_STREAM, HostPort(sport), (u32)bufsize, &sock->host, &err);
	if (!bound) return (int)SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL;
	sock->type = SOCK_PTP;
	sock->retryInterval = rexmtInt;
	sock->retryCount = rexmtCnt;
	sock->nonblocking = flag;
	sock->bufferSize = (u32)bufsize;
	sock->isClient = sport == 0;
	memcpy(sock->laddr, net::kLocalMac, 6);
	memcpy(sock->paddr, dst, 6);
	sock->lport = PspPort(bound);
	sock->pport = (u16)dport;
	sock->state = PTP_CLOSED;
	hostnet::Socket host = sock->host;
	int id = AddSocket(std::move(sock));
	if (id < 0) {
		hostnet::CloseSocket(host);
		return (int)SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL;
	}
	// The connection starts right away (The Warriors sends without connecting first).
	PtpConnect(id, (u32)rexmtInt, 1);
	return id;
}

int sceNetAdhocPtpConnect(int id, int timeout, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	LOGI(LOG, "sceNetAdhocPtpConnect(%d, %d, %d)", id, timeout, flag);
	return PtpConnect(id, (u32)timeout, flag);
}

int sceNetAdhocPtpListen(u32 srcMacAddr, int sport, int bufsize, int rexmtInt, int rexmtCnt, int backlog, int flag) {
	if (!net::Enabled()) return offline::PtpListen(srcMacAddr);
	LOGI(LOG, "sceNetAdhocPtpListen(port %d, buffer %d, rexmt %d x %d, backlog %d, flag %d)", sport, bufsize, rexmtInt, rexmtCnt, backlog, flag);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	bool srcValid = mem::IsValidRange(srcMacAddr, 6);
	if (srcValid) WriteMac(srcMacAddr, net::kLocalMac);
	if (CtlCurrentMode() == CTLMODE_NONE || !srcValid) return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
	if (PtpPortInUse((u16)sport, true)) return (int)SCE_NET_ADHOC_ERROR_PORT_IN_USE;
	if (bufsize <= 0 || rexmtInt <= 0 || rexmtCnt <= 0 || backlog <= 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	auto sock = std::make_unique<AdhocSocket>();
	int err = 0;
	u16 bound = OpenBound(SOCK_STREAM, HostPort(sport), (u32)bufsize, &sock->host, &err);
	if (!bound) return sock->host.Valid() || err == WSAEADDRINUSE || err == WSAEACCES ? (int)SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL
	                                                                                   : (int)SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL;
	if (listen(sock->host.s, backlog) != 0) {
		LOGW(LOG, "listen failed (%d)", WSAGetLastError());
		hostnet::CloseSocket(sock->host);
		return (int)SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL;
	}
	sock->type = SOCK_PTP;
	sock->retryInterval = rexmtInt;
	sock->retryCount = rexmtCnt;
	sock->nonblocking = flag;
	sock->bufferSize = (u32)bufsize;
	sock->isClient = sport == 0;
	memcpy(sock->laddr, net::kLocalMac, 6);
	sock->lport = PspPort(bound);
	sock->state = PTP_LISTEN;
	hostnet::Socket host = sock->host;
	int id = AddSocket(std::move(sock));
	if (id < 0) {
		hostnet::CloseSocket(host);
		return (int)SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL;
	}
	LOGI(LOG, "PTP %d listens on %s:%u", id, hostnet::Ipv4ToString(LocalIp()).c_str(), bound);
	return id;
}

// An accepted connection becomes a socket of its own, if it comes from a member of the group.
int AcceptOne(int listenId, AdhocSocket *ls, u32 macAddr, u32 portAddr, bool *gotOne) {
	*gotOne = false;
	sockaddr_in peer{};
	int len = sizeof(peer);
	SOCKET ns = accept(ls->host.s, (sockaddr *)&peer, &len);
	if (ns == INVALID_SOCKET) return 0;
	*gotOne = true;
	u8 mac[6];
	if (!ResolveIp(peer.sin_addr.s_addr, mac)) {
		LOGW(LOG, "PTP %d: connection from unknown %s:%u refused", listenId, hostnet::Ipv4ToString(peer.sin_addr.s_addr).c_str(),
		     ntohs(peer.sin_port));
		closesocket(ns);
		return 0;
	}
	auto sock = std::make_unique<AdhocSocket>();
	int err = 0;
	if (!hostnet::AdoptSocket(ns, &sock->host, &err)) {
		closesocket(ns);
		return 0;
	}
	BOOL on = TRUE;
	setsockopt(ns, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
	SetBuffers(ns, ls->bufferSize);
	sockaddr_in local{};
	len = sizeof(local);
	getsockname(ns, (sockaddr *)&local, &len);
	sock->type = SOCK_PTP;
	sock->nonblocking = ls->nonblocking;
	sock->attemptCount = 1;  // a closed state now means "disconnected", not "not connected yet"
	sock->retryInterval = ls->retryInterval;
	sock->retryCount = ls->retryCount;
	sock->isClient = true;
	sock->bufferSize = ls->bufferSize;
	memcpy(sock->laddr, net::kLocalMac, 6);
	sock->lport = PspPort(ntohs(local.sin_port));
	memcpy(sock->paddr, mac, 6);
	sock->pport = PspPort(ntohs(peer.sin_port));
	sock->state = PTP_ESTABLISHED;
	u16 pport = sock->pport;
	hostnet::Socket host = sock->host;
	int id = AddSocket(std::move(sock));
	if (id < 0) {
		hostnet::CloseSocket(host);
		return 0;
	}
	WriteMac(macAddr, mac);
	if (mem::IsValidRange(portAddr, 2)) GWrite16(portAddr, pport);
	LOGI(LOG, "PTP %d accepted %s port %u -> socket %d", listenId, MacStr(mac).c_str(), pport, id);
	return id;
}

int sceNetAdhocPtpAccept(int id, u32 macAddr, u32 portAddr, int timeout, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PTP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->nonblocking = flag;
	if (s->flags & ADHOC_F_ALERTACCEPT) {
		s->alertedFlags |= ADHOC_F_ALERTACCEPT;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	if (s->state != PTP_LISTEN) return (int)SCE_NET_ADHOC_ERROR_NOT_LISTENED;
	sched::ChargeUs(50);
	u64 deadline = Deadline((u32)timeout);
	for (;;) {
		bool got;
		int nid = AcceptOne(id, s, macAddr, portAddr, &got);
		if (nid > 0) return nid;
		if (got) continue;  // refused a stranger; look for the next one
		if (flag) return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
		if (Expired(deadline)) return (int)SCE_NET_ADHOC_ERROR_TIMEOUT;
		WaitSocket(s, deadline);
		s = GetSocket(id);
		if (!s) return (int)SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
		if (s->flags & ADHOC_F_ALERTACCEPT) {
			s->alertedFlags |= ADHOC_F_ALERTACCEPT;
			return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
		}
	}
}

// Inside a send / receive loop a closed state means "connecting again" (ConnectStep after a refusal).
bool ConnectPending(AdhocSocket *s, int e) {
	return (s->state == PTP_SYN_SENT || s->state == PTP_CLOSED) &&
	       (e == WSAENOTCONN || e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY || e == WSAEINVAL);
}

int sceNetAdhocPtpSend(int id, u32 dataAddr, u32 sizeAddr, int timeout, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PTP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->nonblocking = flag;
	if (s->state != PTP_ESTABLISHED && s->state != PTP_SYN_SENT) return (int)SCE_NET_ADHOC_ERROR_NOT_CONNECTED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	int len = (int)GRead32(sizeAddr);
	if (len <= 0 || !mem::IsValidRange(dataAddr, (u32)len)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	if (s->flags & ADHOC_F_ALERTSEND) {
		s->alertedFlags |= ADHOC_F_ALERTSEND;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	u64 deadline = Deadline((u32)timeout);
	for (;;) {
		if (s->state == PTP_SYN_SENT || s->state == PTP_CLOSED) ConnectStep(s);
		int n = send(s->host.s, (const char *)GPtr(dataAddr), len, 0);
		if (n > 0) {
			sched::ChargeUs(50);
			GWrite32(sizeAddr, (u32)n);
			if (s->state == PTP_SYN_SENT) s->state = PTP_ESTABLISHED;
			return 0;
		}
		int e = n < 0 ? WSAGetLastError() : 0;
		if (!(n < 0 && (e == WSAEWOULDBLOCK || ConnectPending(s, e)))) {
			LOGD(LOG, "PTP %d send error %d: disconnected", id, e);
			s->state = PTP_CLOSED;
			return (int)SCE_NET_ADHOC_ERROR_DISCONNECTED;
		}
		if (flag) return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
		if (Expired(deadline)) return (int)SCE_NET_ADHOC_ERROR_TIMEOUT;
		WaitSocket(s, deadline, 20000);
		s = GetSocket(id);
		if (!s) return (int)SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
		if (s->flags & ADHOC_F_ALERTSEND) {
			s->alertedFlags |= ADHOC_F_ALERTSEND;
			return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
		}
	}
}

int sceNetAdhocPtpRecv(int id, u32 dataAddr, u32 sizeAddr, int timeout, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4) || !mem::IsValidAddress(dataAddr)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	int room = (int)GRead32(sizeAddr);
	if (room <= 0 || !mem::IsValidRange(dataAddr, (u32)room)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PTP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->nonblocking = flag;
	if (s->state != PTP_ESTABLISHED && s->state != PTP_SYN_SENT) return (int)SCE_NET_ADHOC_ERROR_NOT_CONNECTED;
	if (s->flags & ADHOC_F_ALERTRECV) {
		s->alertedFlags |= ADHOC_F_ALERTRECV;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	u64 deadline = Deadline((u32)timeout);
	for (;;) {
		if (s->state == PTP_SYN_SENT || s->state == PTP_CLOSED) ConnectStep(s);
		int n = recv(s->host.s, (char *)GPtr(dataAddr), room, 0);
		if (n > 0) {
			GWrite32(sizeAddr, (u32)n);
			TouchPeer(s->paddr);
			if (s->state == PTP_SYN_SENT) s->state = PTP_ESTABLISHED;
			sched::ChargeUs(50);
			return 0;
		}
		int e = n < 0 ? WSAGetLastError() : 0;
		if (!(n < 0 && (e == WSAEWOULDBLOCK || ConnectPending(s, e)))) {
			// End of stream or a broken connection.
			LOGD(LOG, "PTP %d recv: %s (%d)", id, n == 0 ? "closed by the peer" : "error", e);
			s->state = PTP_CLOSED;
			return (int)SCE_NET_ADHOC_ERROR_DISCONNECTED;
		}
		if (flag) return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
		if (Expired(deadline)) return (int)SCE_NET_ADHOC_ERROR_TIMEOUT;
		WaitSocket(s, deadline, 20000);
		s = GetSocket(id);
		if (!s) return (int)SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
		if (s->flags & ADHOC_F_ALERTRECV) {
			s->alertedFlags |= ADHOC_F_ALERTRECV;
			return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
		}
	}
}

// Sockets send at once (no Nagle), so there is never anything to flush.
int sceNetAdhocPtpFlush(int id, int timeout, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PTP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->nonblocking = flag;
	if (s->flags & ADHOC_F_ALERTFLUSH) {
		s->alertedFlags |= ADHOC_F_ALERTFLUSH;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	if (s->state == PTP_ESTABLISHED) sched::ChargeUs(50);
	return 0;
}

int sceNetAdhocPtpClose(int id, int unknown) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (id < 1 || id > kMaxSockets) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PTP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	LOGI(LOG, "sceNetAdhocPtpClose(%d)", id);
	DeleteSocket(id);
	return 0;
}

// ---------------------------------------------------------------- alerts, polling, lists

int sceNetAdhocSetSocketAlert(int id, int flag) {
	if (!net::Enabled()) return offline::SocketCall(id);
	int r = SetSocketAlertInternal(id, flag);
	Sleep(1000);
	return r;
}

int sceNetAdhocGetSocketAlert(int id, u32 flagAddr) {
	if (!net::Enabled()) return offline::SocketCall(id);
	if (!mem::IsValidRange(flagAddr, 4)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	AdhocSocket *s = GetSocket(id);
	if (!s) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	GWrite32(flagAddr, (u32)s->flags);
	return 0;
}

// One look at the sockets (PPSSPP's PollAdhocSocket). Returns how many have events, or ALERTED.
int PollOnce(std::vector<PollSd> &sds) {
	int affected = 0;
	for (auto &sd : sds) sd.revents = 0;
	u64 now = sched::NowUs();
	for (auto &sd : sds) {
		AdhocSocket *s = GetSocket(sd.id);
		if (!s) {
			sd.revents |= ADHOC_EV_INVALID;
		} else {
			hostnet::Readiness r;
			hostnet::CheckReady(s->host.s, &r);
			if ((sd.events & ADHOC_EV_RECV) && r.read) sd.revents |= ADHOC_EV_RECV;
			if ((sd.events & ADHOC_EV_SEND) && r.write) sd.revents |= ADHOC_EV_SEND;
			if (s->alertedFlags) sd.revents |= ADHOC_EV_ALERT;
			sd.revents &= sd.events;
			if (s->type == SOCK_PTP) {
				if ((sd.events & ADHOC_EV_ACCEPT) && s->state == PTP_LISTEN) {
					if (r.read) sd.revents |= ADHOC_EV_ACCEPT;
				} else if ((sd.events & ADHOC_EV_CONNECT) &&
				           ((s->state == PTP_CLOSED && s->attemptCount == 0) || (s->state == PTP_SYN_SENT && now - s->lastAttempt > 1000))) {
					// Fate Unlimited Codes and Carnage Heart EXA retry a failed connect when told so.
					sd.revents |= ADHOC_EV_CONNECT;
				}
				if (s->state == PTP_CLOSED && s->attemptCount > 0) sd.revents |= ADHOC_EV_DISCONNECT;
			}
			if (s->flags & ADHOC_F_ALERTPOLL) {
				s->alertedFlags |= ADHOC_F_ALERTPOLL;
				return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
			}
		}
		if (sd.revents) affected++;
	}
	return affected;
}

int sceNetAdhocPollSocket(u32 sdsAddr, int count, int timeout, int nonblock) {
	if (!net::Enabled()) return offline::PollSocket(sdsAddr, count);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (count <= 0 || !mem::IsValidRange(sdsAddr, (u32)count * sizeof(PollSd))) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	std::vector<PollSd> sds((size_t)count);
	mem::ReadBytes(sds.data(), sdsAddr, (u32)(count * sizeof(PollSd)));
	for (auto &sd : sds)
		if (!GetSocket(sd.id)) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	if (count > 64) count = 64, sds.resize(64);  // FD_SETSIZE
	auto writeBack = [&] { mem::WriteBytes(sdsAddr, sds.data(), (u32)(sds.size() * sizeof(PollSd))); };
	if (nonblock) {
		int r = PollOnce(sds);
		writeBack();
		if (r >= 0) {
			sched::ChargeUs(50);
			return r;
		}
		return (int)SCE_NET_ADHOC_ERROR_EXCEPTION_EVENT;
	}
	// Blocking: until an event or the time-out (<= 0: none).
	u64 deadline = timeout > 0 ? sched::NowUs() + (u64)timeout : 0;
	for (;;) {
		int r = PollOnce(sds);
		writeBack();
		if (r > 0) return r;
		if (Expired(deadline)) return r < 0 ? (int)SCE_NET_ADHOC_ERROR_EXCEPTION_EVENT : r;
		std::vector<WSAEVENT> evs;
		bool wantConnect = false;
		for (auto &sd : sds) {
			if (AdhocSocket *s = GetSocket(sd.id)) {
				evs.push_back(s->host.ev);
				if ((sd.events & ADHOC_EV_CONNECT) && s->type == SOCK_PTP) wantConnect = true;
			}
		}
		if (!CanWait()) return r < 0 ? (int)SCE_NET_ADHOC_ERROR_EXCEPTION_EVENT : r;
		if (net::WaitHostEvents(evs.data(), evs.size(), deadline, 0, wantConnect ? 2000 : 50000) == net::HOSTWAIT_ABORTED)
			return (int)SCE_NET_ADHOC_ERROR_EXCEPTION_EVENT;
	}
}

u32 RecvQueued(AdhocSocket *s) {
	u32 n = hostnet::BytesAvailable(s->host.s);
	if (s->type == SOCK_PTP) n = std::min(n, s->bufferSize);
	if (n == 0) {
		// FIONREAD can read 0 while there is data; peek.
		int r = s->type == SOCK_PTP ? recv(s->host.s, g_peek.data(), (int)std::min<u32>((u32)g_peek.size(), s->bufferSize), MSG_PEEK)
		                            : recvfrom(s->host.s, g_peek.data(), (int)std::min<u32>((u32)g_peek.size(), s->bufferSize), MSG_PEEK, nullptr, nullptr);
		if (r > 0) n = (u32)r;
	}
	return n;
}

int sceNetAdhocGetPdpStat(u32 sizeAddr, u32 bufAddr) {
	if (!net::Enabled()) return offline::GetSocketStat(sizeAddr);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	int count = 0;
	for (auto &s : g_sockets)
		if (s && s->type == SOCK_PDP) count++;
	if (!bufAddr || !mem::IsValidAddress(bufAddr)) {
		GWrite32(sizeAddr, (u32)(count * sizeof(PdpStat)));
		return 0;
	}
	int want = std::min(count, (int)GRead32(sizeAddr) / (int)sizeof(PdpStat));
	int n = 0;
	for (int j = 0; j < kMaxSockets && n < want; j++) {
		AdhocSocket *s = g_sockets[j].get();
		if (!s || s->type != SOCK_PDP) continue;
		u32 e = bufAddr + (u32)n * sizeof(PdpStat);
		if (!mem::IsValidRange(e, sizeof(PdpStat))) break;
		PdpStat st{};
		st.id = j + 1;
		memcpy(st.laddr, s->laddr, 6);
		st.lport = s->lport;
		st.rcvSbCc = RecvQueued(s);  // Obscure The Aftermath checks this before receiving
		mem::WriteBytes(e, &st, sizeof(st));
		if (n > 0) GWrite32(e - sizeof(PdpStat), e);
		n++;
	}
	GWrite32(sizeAddr, (u32)(n * sizeof(PdpStat)));
	sched::ChargeUs(50);
	return 0;
}

int sceNetAdhocGetPtpStat(u32 sizeAddr, u32 bufAddr) {
	if (!net::Enabled()) return offline::GetSocketStat(sizeAddr);
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(sizeAddr, 4)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	int count = 0;
	for (auto &s : g_sockets)
		if (s && s->type == SOCK_PTP) count++;
	if (!bufAddr || !mem::IsValidAddress(bufAddr)) {
		GWrite32(sizeAddr, (u32)(count * sizeof(PtpStat)));
		return 0;
	}
	int want = std::min(count, (int)GRead32(sizeAddr) / (int)sizeof(PtpStat));
	int n = 0;
	u64 now = sched::NowUs();
	for (int j = 0; j < kMaxSockets && n < want; j++) {
		AdhocSocket *s = g_sockets[j].get();
		if (!s || s->type != SOCK_PTP) continue;
		// GvG Next Plus watches the state here: a connection in progress turns established about two
		// frames after the attempt (not sooner: Bleach Heat the Soul 7).
		if ((s->state == PTP_SYN_SENT || s->state == PTP_SYN_RCVD) && now - s->lastAttempt > 33333) ConnectStep(s);
		u32 e = bufAddr + (u32)n * sizeof(PtpStat);
		if (!mem::IsValidRange(e, sizeof(PtpStat))) break;
		PtpStat st{};
		st.id = j + 1;
		memcpy(st.laddr, s->laddr, 6);
		memcpy(st.paddr, s->paddr, 6);
		st.lport = s->lport;
		st.pport = s->pport;
		st.rcvSbCc = s->state == PTP_LISTEN ? 0 : RecvQueued(s);
		st.state = s->state;
		mem::WriteBytes(e, &st, sizeof(st));
		if (n > 0) GWrite32(e - sizeof(PtpStat), e);
		n++;
	}
	GWrite32(sizeAddr, (u32)(n * sizeof(PtpStat)));
	sched::ChargeUs(50);
	return 0;
}

// ---------------------------------------------------------------- game mode data
// Game mode keeps a "master" block of game data per player and a "replica" of every other player's.
// A scheduler event (every 500 us while active) sends the master to every replica holder over a PDP
// socket on port 31000 and copies what arrives into the replicas. Creating the master / the last
// replica blocks the caller until everybody's first data arrived (PPSSPP's __GameModeNotify).

constexpr u16 ADHOC_GAMEMODE_PORT = 31000;
constexpr u64 GAMEMODE_UPDATE_INTERVAL_US = 500;
constexpr u64 GAMEMODE_INIT_DELAY_US = 10000;
constexpr u64 GAMEMODE_SYNC_TIMEOUT_US = 250000;
constexpr SceUID GAMEMODE_WAITID = 0x2001;

struct GameModeArea {
	int id = 0;
	int size = 0;
	u32 addr = 0;
	u64 updateTimestamp = 0;
	bool dataUpdated = false;
	bool dataSent = false;
	u8 mac[6] = {};
	std::vector<u8> data;
};

GameModeArea g_master;
std::vector<GameModeArea> g_replicas;
std::vector<std::pair<std::array<u8, 6>, u16>> g_gmPeerPorts;
int g_gmSocket = -1;
int g_gmBuffSize = 0;
std::vector<u8> g_gmBuffer;
u64 g_gmEvent = 0;
SceUID g_gmWaiter = 0;     // the thread blocked until the first data is in (or the master is gone)
u32 g_gmWaitValue = 0;     // what that call returns

u16 GmPeerPort(const u8 *mac) {
	for (auto &p : g_gmPeerPorts)
		if (MacMatch(p.first.data(), mac)) return p.second;
	return ADHOC_GAMEMODE_PORT;
}
void SetGmPeerPort(const u8 *mac, u16 port) {
	for (auto &p : g_gmPeerPorts)
		if (MacMatch(p.first.data(), mac)) {
			p.second = port;
			return;
		}
	std::array<u8, 6> m;
	memcpy(m.data(), mac, 6);
	g_gmPeerPorts.push_back({m, port});
}

bool GameModeActive() { return GameModeEntered() && !g_master.data.empty(); }

void ResumeGmWaiter() {
	if (!g_gmWaiter) return;
	auto *t = kernel::Get<kernel::PspThread>(g_gmWaiter);
	if (t && t->IsWaitingFor(kernel::WAITTYPE_NET, GAMEMODE_WAITID)) sched::Wake(t, (s32)g_gmWaitValue);
	g_gmWaiter = 0;
}

// Blocks the current thread until the scheduler resumes it; returns what it passes.
int BlockOnGameMode(u32 waitValue) {
	if (!CanWait()) return (int)waitValue;
	g_gmWaiter = sched::CurrentId();
	g_gmWaitValue = waitValue;
	sched::HoldRealTime(true);
	s32 r = sched::Wait(kernel::WAITTYPE_NET, GAMEMODE_WAITID, 0, false);
	sched::HoldRealTime(false);
	return r;
}

void GameModeTick();

void ScheduleGameMode(u64 delayUs) {
	g_gmEvent = sched::ScheduleEvent(sched::NowUs() + delayUs, [] {
		g_gmEvent = 0;
		GameModeTick();
	});
}

void GameModeTick() {
	if (!GameModeActive()) {
		LOGI(LOG, "Game mode scheduler ended");
		ResumeGmWaiter();
		return;
	}
	auto &macs = GameModeMacs();
	if (!macs.empty() && g_replicas.size() == macs.size() - 1) {
		if (g_gmSocket < 0 && !IsZeroMac(g_master.mac)) {
			g_gmBuffer.resize((size_t)g_gmBuffSize);
			g_gmSocket = PdpCreateInternal(g_master.mac, ADHOC_GAMEMODE_PORT, g_gmBuffSize, 0);
			if (g_gmSocket < 0) {
				LOGW(LOG, "Game mode: cannot create the socket (%08x)", (u32)g_gmSocket);
				g_gmWaitValue = (u32)g_gmSocket;
				ResumeGmWaiter();
				return;
			}
			LOGI(LOG, "Game mode synchronizer started (socket %d, %d bytes)", g_gmSocket, g_gmBuffSize);
		}
		if (g_gmSocket < 0 || !GetSocket(g_gmSocket)) {
			if (g_gmSocket >= 0) {
				LOGW(LOG, "Game mode: the socket was deleted");
				g_gmSocket = -1;
				ResumeGmWaiter();
				return;
			}
			ScheduleGameMode(GAMEMODE_UPDATE_INTERVAL_US);
			return;
		}
		if (g_master.dataUpdated) {
			size_t sent = 0;
			for (auto &gma : g_replicas) {
				if (!gma.dataSent && PdpWritable(g_gmSocket)) {
					int r = PdpSendInternal(g_gmSocket, gma.mac, GmPeerPort(gma.mac), g_master.data.data(), g_master.size);
					if (r != (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK) {
						gma.dataSent = true;
						sent++;
					}
				} else if (gma.dataSent) {
					sent++;
				}
			}
			if (sent == g_replicas.size()) g_master.dataUpdated = false;
		} else if (g_gmWaiter) {
			// The initial sync: everybody's first data (or departure).
			size_t recvd = 0;
			for (auto &gma : g_replicas) {
				if (gma.dataUpdated || gma.updateTimestamp == 0) {
					recvd++;
					// They're listening now: send ours once more, in case the first one came too early.
					if (gma.dataUpdated) PdpSendInternal(g_gmSocket, gma.mac, GmPeerPort(gma.mac), g_master.data.data(), g_master.size);
				}
			}
			u64 now = sched::NowUs();
			if (recvd == g_replicas.size()) {
				ResumeGmWaiter();
			} else if (now - g_master.updateTimestamp > GAMEMODE_SYNC_TIMEOUT_US) {
				for (auto &gma : g_replicas)
					if (!gma.dataUpdated && gma.updateTimestamp != 0) gma.dataSent = false;
				g_master.updateTimestamp = now;
				g_master.dataUpdated = true;
			}
		}
		if (PdpReadable(g_gmSocket)) {
			u8 sender[6];
			u16 port = ADHOC_GAMEMODE_PORT;
			int len = g_gmBuffSize;
			if (PdpRecvInternal(g_gmSocket, sender, &port, g_gmBuffer.data(), &len) == 0 && len > 0) {
				SetGmPeerPort(sender, port);
				for (auto &gma : g_replicas) {
					if (MacMatch(gma.mac, sender)) {
						memcpy(gma.data.data(), g_gmBuffer.data(), (size_t)std::min(gma.size, len));
						gma.dataUpdated = true;
						gma.updateTimestamp = sched::NowUs();
						break;
					}
				}
			}
		}
	}
	ScheduleGameMode(GAMEMODE_UPDATE_INTERVAL_US);
}

int CheckGameMode() {
	if (!CtlInited()) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
	if (CtlCurrentMode() != CTLMODE_GAMEMODE) return (int)SCE_NET_ADHOC_ERROR_NOT_IN_GAMEMODE;
	if (!GameModeEntered()) return (int)SCE_NET_ADHOCCTL_ERROR_NOT_ENTER_GAMEMODE;
	return 0;
}

int sceNetAdhocGameModeCreateMaster(u32 dataAddr, int size) {
	if (!net::Enabled()) return offline::GameModeCreate();
	LOGI(LOG, "sceNetAdhocGameModeCreateMaster(%08x, %d)", dataAddr, size);
	if (int e = CheckGameMode()) return e;
	if (size < 0 || !mem::IsValidRange(dataAddr, (u32)size)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	if (!g_master.data.empty()) return (int)SCE_NET_ADHOC_ERROR_ALREADY_CREATED;
	sched::ChargeUs(1000);
	g_gmBuffSize = std::max(g_gmBuffSize, size);
	g_master = GameModeArea{};
	g_master.size = size;
	g_master.addr = dataAddr;
	g_master.updateTimestamp = sched::NowUs();
	g_master.dataUpdated = true;
	memcpy(g_master.mac, net::kLocalMac, 6);
	g_master.data.resize((size_t)std::max(size, 1));
	if (size) mem::ReadBytes(g_master.data.data(), dataAddr, (u32)size);
	if (!g_gmEvent) ScheduleGameMode(GAMEMODE_INIT_DELAY_US);
	// With every replica created too, wait for the first data of all players.
	if (!GameModeMacs().empty() && g_replicas.size() == GameModeMacs().size() - 1 && g_gmEvent) return BlockOnGameMode(0);
	return 0;
}

int sceNetAdhocGameModeCreateReplica(u32 macAddr, u32 dataAddr, int size) {
	if (!net::Enabled()) return offline::GameModeCreate();
	LOGI(LOG, "sceNetAdhocGameModeCreateReplica(%08x, %08x, %d)", macAddr, dataAddr, size);
	if (int e = CheckGameMode()) return e;
	if (!mem::IsValidRange(macAddr, 6) || size < 0 || !mem::IsValidRange(dataAddr, (u32)size)) return (int)SCE_NET_ADHOCCTL_ERROR_INVALID_ARG;
	sched::ChargeUs(1000);
	u8 mac[6];
	mem::ReadBytes(mac, macAddr, 6);
	int maxId = 0;
	for (auto &gma : g_replicas) {
		maxId = std::max(maxId, gma.id);
		if (!memcmp(gma.mac, mac, 6)) return gma.id;  // already there
	}
	g_gmBuffSize = std::max(g_gmBuffSize, size);
	GameModeArea gma;
	gma.id = maxId + 1;
	gma.size = size;
	gma.addr = dataAddr;
	gma.updateTimestamp = sched::NowUs();
	memcpy(gma.mac, mac, 6);
	gma.data.resize((size_t)std::max(size, 1));
	if (size) mem::ReadBytes(gma.data.data(), dataAddr, (u32)size);
	g_replicas.push_back(gma);
	int id = gma.id;
	if (!g_master.data.empty() && g_replicas.size() == GameModeMacs().size() - 1 && g_gmEvent) return BlockOnGameMode((u32)id);
	return id;
}

int sceNetAdhocGameModeUpdateMaster() {
	if (!net::Enabled()) return offline::GameModeOther();
	if (int e = CheckGameMode()) return e;
	if (!g_master.data.empty()) {
		if (g_master.size) mem::ReadBytes(g_master.data.data(), g_master.addr, (u32)g_master.size);
		g_master.dataUpdated = true;
		g_master.updateTimestamp = sched::NowUs();
		for (auto &gma : g_replicas) gma.dataSent = false;
	}
	sched::ChargeUs(100);
	return 0;
}

int sceNetAdhocGameModeDeleteMaster() {
	if (!net::Enabled()) return offline::GameModeOther();
	if (IsZeroMac(g_master.mac)) return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
	GameModeDeleteMaster();
	return 0;
}

int sceNetAdhocGameModeUpdateReplica(int id, u32 infoAddr) {
	if (!net::Enabled()) return offline::GameModeOther();
	if (int e = CheckGameMode()) return e;
	auto it = std::find_if(g_replicas.begin(), g_replicas.end(), [id](const GameModeArea &g) { return g.id == id; });
	if (it == g_replicas.end()) return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
	bool info = mem::IsValidRange(infoAddr, sizeof(GameModeUpdateInfo));  // optional (Bomberman Panic Bomber)
	if (it->dataUpdated) {
		if (it->size) mem::WriteBytes(it->addr, it->data.data(), (u32)it->size);
		it->dataUpdated = false;
		if (info) {
			GameModeUpdateInfo ui{};
			ui.length = sizeof(GameModeUpdateInfo);
			ui.updated = 1;
			u64 now = sched::NowUs();
			ui.timeStamp = std::max(it->updateTimestamp, now > 10000 ? now - 10000 : 0);
			mem::WriteBytes(infoAddr, &ui, sizeof(ui));
		}
	} else if (info) {
		GWrite32(infoAddr + 4, 0);
	}
	sched::ChargeUs(100);
	return 0;
}

int sceNetAdhocGameModeDeleteReplica(int id) {
	if (!net::Enabled()) return offline::GameModeOther();
	auto it = std::find_if(g_replicas.begin(), g_replicas.end(), [id](const GameModeArea &g) { return g.id == id; });
	if (it == g_replicas.end()) return (int)SCE_NET_ADHOC_ERROR_NOT_CREATED;
	auto &pp = g_gmPeerPorts;
	u8 mac[6];
	memcpy(mac, it->mac, 6);
	pp.erase(std::remove_if(pp.begin(), pp.end(), [&](const std::pair<std::array<u8, 6>, u16> &p) { return MacMatch(p.first.data(), mac); }),
	         pp.end());
	g_replicas.erase(it);
	return 0;
}

// ---------------------------------------------------------------- the library

int sceNetAdhocInit() {
	if (!net::Enabled()) return offline::AdhocInit();
	if (g_inited) return (int)SCE_NET_ADHOC_ERROR_ALREADY_INITIALIZED;
	g_inited = true;
	// The firmware deletes game mode's master data here.
	GameModeDeleteMaster();
	GameModeDeleteBuffers();
	LOGI(LOG, "sceNetAdhocInit: ad hoc address %s, port offset %u", hostnet::Ipv4ToString(LocalIp()).c_str(), kPortOffset);
	return 0;
}

int sceNetAdhocTerm() {
	if (!net::Enabled()) return offline::AdhocTerm();
	// Also takes sceNetAdhocctl down (PPSSPP does the same).
	CtlShutdown();
	if (g_inited) {
		GameModeDeleteBuffers();
		AdhocShutdownSockets();
		g_inited = false;
	}
	sched::ChargeUs(10000);
	return 0;
}

int sceNetAdhocPdpCreate(u32 macAddr, int port, int bufferSize, u32 flag) {
	if (!net::Enabled()) return offline::PdpCreate(macAddr, port, bufferSize, flag);
	return PdpCreate(macAddr, port, bufferSize, flag);
}

// ---------------------------------------------------------------- sceNetAdhocDiscover
// The discovery runs and ends at once without finding anyone (as before ad hoc play existed).

enum { DISCOVER_STATUS_NONE = 0, DISCOVER_STATUS_IN_PROGRESS = 1, DISCOVER_STATUS_COMPLETED = 2 };
enum { DISCOVER_RESULT_NO_PEER_FOUND = 0, DISCOVER_RESULT_CANCELED = 1 };
// SceNetAdhocDiscoverParam: u32 unknown1; char groupName[8]; u32 unknown2; u32 result.
constexpr u32 DISCOVER_PARAM_RESULT_OFFSET = 16;

int g_discoverStatus = DISCOVER_STATUS_NONE;
u32 g_discoverParam = 0;
bool g_discoverStopping = false;

void SetDiscoverResult(int result) {
	if (mem::IsValidRange(g_discoverParam + DISCOVER_PARAM_RESULT_OFFSET, 4)) GWrite32(g_discoverParam + DISCOVER_PARAM_RESULT_OFFSET, (u32)result);
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

// ---------------------------------------------------------------- shared with the other ad hoc files

bool AdhocInited() { return g_inited; }

int PdpCreateInternal(const u8 *mac, int port, int bufferSize, u32 flag) {
	if (!g_inited) return (int)SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
	if (bufferSize <= 0) return (int)SCE_NET_ADHOC_ERROR_INVALID_ARG;
	if (PdpPortInUse((u16)port)) return (int)SCE_NET_ADHOC_ERROR_PORT_IN_USE;
	if (CtlCurrentMode() == CTLMODE_NONE || !IsLocalMac(mac)) return (int)SCE_NET_ADHOC_ERROR_INVALID_ADDR;
	auto sock = std::make_unique<AdhocSocket>();
	int err = 0;
	u16 bound = OpenBound(SOCK_DGRAM, HostPort(port), (u32)bufferSize, &sock->host, &err);
	if (!bound) return (int)SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL;
	sock->type = SOCK_PDP;
	sock->nonblocking = (s32)flag;
	sock->bufferSize = (u32)bufferSize;
	sock->isClient = port == 0;
	memcpy(sock->laddr, net::kLocalMac, 6);
	sock->lport = PspPort(bound);
	hostnet::Socket host = sock->host;
	int id = AddSocket(std::move(sock));
	if (id < 0) {
		hostnet::CloseSocket(host);
		return (int)ERROR_NET_NO_SPACE;
	}
	return id;
}

int PdpSendInternal(int id, const u8 *mac, u16 port, const void *data, int len) {
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PDP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	if (s->flags & ADHOC_F_ALERTSEND) {
		s->alertedFlags |= ADHOC_F_ALERTSEND;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	if (IsBroadcastMac(mac)) {
		for (Peer &p : Peers()) {
			if (p.lastRecv == 0) continue;
			int e;
			PdpSendTo(s, PdpTarget{p.ip, HostPort(port)}, data, len, &e);
		}
		return 0;
	}
	u32 ip;
	if (!ResolveMac(mac, &ip)) return 0;  // unknown peer: faked success, like the HLE call
	int e = 0;
	int n = PdpSendTo(s, PdpTarget{ip, HostPort(port)}, data, len, &e);
	if (n < 0 && e == WSAEWOULDBLOCK) return (int)SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
	return 0;
}

int PdpRecvInternal(int id, u8 *mac, u16 *port, void *buf, int *len) {
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PDP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	if (s->flags & ADHOC_F_ALERTRECV) {
		s->alertedFlags |= ADHOC_F_ALERTRECV;
		return (int)SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
	}
	return PdpTryRecv(s, mac, port, buf, len);
}

int PdpDeleteInternal(int id) {
	AdhocSocket *s = GetSocket(id);
	if (!s || s->type != SOCK_PDP) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	DeleteSocket(id);
	return 0;
}

bool PdpWritable(int id) {
	AdhocSocket *s = GetSocket(id);
	hostnet::Readiness r;
	return s && hostnet::CheckReady(s->host.s, &r) && r.write;
}

bool PdpReadable(int id) {
	AdhocSocket *s = GetSocket(id);
	hostnet::Readiness r;
	return s && hostnet::CheckReady(s->host.s, &r) && r.read;
}

WSAEVENT SocketEvent(int id) {
	AdhocSocket *s = GetSocket(id);
	return s ? s->host.ev : WSA_INVALID_EVENT;
}

int SetSocketAlertInternal(int id, s32 flags) {
	AdhocSocket *s = GetSocket(id);
	if (!s) return (int)SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
	s->flags = flags & ADHOC_F_ALERTALL;
	s->alertedFlags = 0;
	// Blocked calls look at the flags again.
	net::AbortWaits(s->abortKey);
	return 0;
}

void AdhocShutdownSockets() {
	for (int id = 1; id <= kMaxSockets; id++)
		if (g_sockets[id - 1]) DeleteSocket(id);
	g_gmSocket = -1;
}

void GameModeDeleteBuffers() {
	g_master = GameModeArea{};
	g_replicas.clear();
	g_gmBuffer.clear();
	g_gmBuffSize = 0;
}

void GameModeDeleteMaster() {
	// The scheduler notices on its next tick and resumes the caller then.
	bool running = g_gmEvent != 0;
	u8 mac[6];
	memcpy(mac, g_master.mac, 6);
	g_master.data.clear();
	auto &pp = g_gmPeerPorts;
	pp.erase(std::remove_if(pp.begin(), pp.end(), [&](const std::pair<std::array<u8, 6>, u16> &p) { return MacMatch(p.first.data(), mac); }),
	         pp.end());
	g_master = GameModeArea{};
	if (g_replicas.empty() && g_gmSocket >= 0) {
		PdpDeleteInternal(g_gmSocket);
		g_gmSocket = -1;
	}
	if (running) BlockOnGameMode(0);
}

void GameModeCloseSocket() {
	if (g_gmSocket >= 0) PdpDeleteInternal(g_gmSocket);
	g_gmSocket = -1;
	g_gmPeerPorts.clear();
}

void GameModePeerLeft(const u8 *mac) {
	for (auto &gma : g_replicas)
		if (MacMatch(gma.mac, mac)) {
			gma.updateTimestamp = 0;
			break;
		}
}

}  // namespace net::adhoc

namespace net {

void AdhocShutdown() {
	if (!net::Enabled()) {
		adhoc::offline::MatchingTerm();
		adhoc::offline::CtlTerm();
		adhoc::offline::AdhocTerm();
	} else {
		adhoc::MatchingShutdown();
		adhoc::CtlShutdown();
		adhoc::GameModeDeleteBuffers();
		adhoc::AdhocShutdownSockets();
		adhoc::g_inited = false;
	}
	adhoc::sceNetAdhocDiscoverTerm();
}

void RegisterAdhocLibraries() {
	using namespace adhoc;
	static const hle::HleFunction sceNetAdhoc[] = {
		HLEFN(0xE1D621D7, sceNetAdhocInit),
		HLEFN(0xA62C6F57, sceNetAdhocTerm),
		HLEFN(0x0AD043ED, AdhocctlConnectFromAdhocLib),
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

	RegisterAdhocctlLibrary();
	RegisterMatchingLibrary();

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
