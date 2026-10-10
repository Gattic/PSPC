// SPDX-License-Identifier: GPL-2.0-or-later
//
// See host_net.h. The errno and socket-option tables follow PPSSPP's Core/HLE/NetInetConstants.cpp
// (Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later), mapped from Winsock's error codes.
#include "net/host_net.h"

#include <iphlpapi.h>
#include <mstcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "common/log.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace hostnet {

namespace {
constexpr const char *LOG = "HOSTNET";
}

bool Startup() {
	static std::once_flag once;
	static bool ok = false;
	std::call_once(once, [] {
		WSADATA wsa;
		ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
		if (!ok) LOGE(LOG, "WSAStartup failed");
	});
	return ok;
}

// ---------------------------------------------------------------- PSP constants

int PspErrnoFromWsa(int e, bool forConnect) {
	switch (e) {
	case 0: return 0;
	case WSAEINTR: return PSP_EINTR;
	case WSAEBADF: return PSP_EBADF;
	case WSAEACCES: return PSP_EACCES;
	case WSAEFAULT: return PSP_EFAULT;
	case WSAEINVAL: return PSP_EINVAL;
	case WSAEMFILE: return PSP_EMFILE;
	case WSAEWOULDBLOCK: return forConnect ? PSP_EINPROGRESS : PSP_EAGAIN;
	case WSAEINPROGRESS: return PSP_EINPROGRESS;
	case WSAEALREADY: return PSP_EALREADY;
	case WSAENOTSOCK: return PSP_ENOTSOCK;
	case WSAEDESTADDRREQ: return PSP_EDESTADDRREQ;
	case WSAEMSGSIZE: return PSP_EMSGSIZE;
	case WSAEPROTOTYPE: return PSP_EPROTOTYPE;
	case WSAENOPROTOOPT: return PSP_ENOPROTOOPT;
	case WSAEPROTONOSUPPORT: return PSP_EPROTONOSUPPORT;
	case WSAESOCKTNOSUPPORT: return PSP_ESOCKTNOSUPPORT;
	case WSAEOPNOTSUPP: return PSP_EOPNOTSUPP;
	case WSAEPFNOSUPPORT: return PSP_EPFNOSUPPORT;
	case WSAEAFNOSUPPORT: return PSP_EAFNOSUPPORT;
	case WSAEADDRINUSE: return PSP_EADDRINUSE;
	case WSAEADDRNOTAVAIL: return PSP_EADDRNOTAVAIL;
	case WSAENETDOWN: return PSP_ENETDOWN;
	case WSAENETUNREACH: return PSP_ENETUNREACH;
	case WSAENETRESET: return PSP_ENETRESET;
	case WSAECONNABORTED: return PSP_ECONNABORTED;
	case WSAECONNRESET: return PSP_ECONNRESET;
	case WSAENOBUFS: return PSP_ENOBUFS;
	case WSAEISCONN: return PSP_EISCONN;
	case WSAENOTCONN: return PSP_ENOTCONN;
	case WSAESHUTDOWN: return PSP_ESHUTDOWN;
	case WSAETIMEDOUT: return PSP_ETIMEDOUT;
	case WSAECONNREFUSED: return PSP_ECONNREFUSED;
	case WSAEHOSTDOWN: return PSP_EHOSTDOWN;
	case WSAEHOSTUNREACH: return PSP_EHOSTUNREACH;
	case WSA_NOT_ENOUGH_MEMORY: return PSP_ENOMEM;
	default: return PSP_EINVAL;
	}
}

const char *PspErrnoName(int e) {
	switch (e) {
	case 0: return "0";
	case PSP_EINTR: return "EINTR";
	case PSP_EBADF: return "EBADF";
	case PSP_EAGAIN: return "EAGAIN";
	case PSP_ENOMEM: return "ENOMEM";
	case PSP_EACCES: return "EACCES";
	case PSP_EFAULT: return "EFAULT";
	case PSP_EINVAL: return "EINVAL";
	case PSP_EMFILE: return "EMFILE";
	case PSP_ENOSPC: return "ENOSPC";
	case PSP_EPIPE: return "EPIPE";
	case PSP_EOPNOTSUPP: return "EOPNOTSUPP";
	case PSP_EPFNOSUPPORT: return "EPFNOSUPPORT";
	case PSP_ECONNRESET: return "ECONNRESET";
	case PSP_ENOBUFS: return "ENOBUFS";
	case PSP_EAFNOSUPPORT: return "EAFNOSUPPORT";
	case PSP_EPROTOTYPE: return "EPROTOTYPE";
	case PSP_ENOTSOCK: return "ENOTSOCK";
	case PSP_ENOPROTOOPT: return "ENOPROTOOPT";
	case PSP_ESHUTDOWN: return "ESHUTDOWN";
	case PSP_ECONNREFUSED: return "ECONNREFUSED";
	case PSP_EADDRINUSE: return "EADDRINUSE";
	case PSP_ECONNABORTED: return "ECONNABORTED";
	case PSP_ENETUNREACH: return "ENETUNREACH";
	case PSP_ENETDOWN: return "ENETDOWN";
	case PSP_ETIMEDOUT: return "ETIMEDOUT";
	case PSP_EHOSTDOWN: return "EHOSTDOWN";
	case PSP_EHOSTUNREACH: return "EHOSTUNREACH";
	case PSP_EINPROGRESS: return "EINPROGRESS";
	case PSP_EALREADY: return "EALREADY";
	case PSP_EDESTADDRREQ: return "EDESTADDRREQ";
	case PSP_EMSGSIZE: return "EMSGSIZE";
	case PSP_EPROTONOSUPPORT: return "EPROTONOSUPPORT";
	case PSP_ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT";
	case PSP_EADDRNOTAVAIL: return "EADDRNOTAVAIL";
	case PSP_ENETRESET: return "ENETRESET";
	case PSP_EISCONN: return "EISCONN";
	case PSP_ENOTCONN: return "ENOTCONN";
	default: return "E?";
	}
}

sockaddr_in ToHost(const PspSockaddrIn &a) {
	sockaddr_in h{};
	h.sin_family = AF_INET;  // the PSP stack only does AF_INET; sin_family is checked by the caller
	h.sin_port = a.port;
	h.sin_addr.s_addr = a.addr;
	return h;
}

PspSockaddrIn ToPsp(const sockaddr_in &a) {
	PspSockaddrIn p{};
	p.len = sizeof(PspSockaddrIn);
	p.family = PSP_AF_INET;
	p.port = a.sin_port;
	p.addr = a.sin_addr.s_addr;
	return p;
}

bool MapSockopt(int level, int name, int *hostLevel, int *hostName) {
	if (level == PSP_SOL_SOCKET) {
		*hostLevel = SOL_SOCKET;
		switch (name) {
		case PSP_SO_DEBUG: *hostName = SO_DEBUG; return true;
		case PSP_SO_ACCEPTCONN: *hostName = SO_ACCEPTCONN; return true;
		case PSP_SO_REUSEADDR: *hostName = SO_REUSEADDR; return true;
		case PSP_SO_KEEPALIVE: *hostName = SO_KEEPALIVE; return true;
		case PSP_SO_DONTROUTE: *hostName = SO_DONTROUTE; return true;
		case PSP_SO_BROADCAST: *hostName = SO_BROADCAST; return true;
		case PSP_SO_LINGER: *hostName = SO_LINGER; return true;
		case PSP_SO_OOBINLINE: *hostName = SO_OOBINLINE; return true;
		case PSP_SO_SNDBUF: *hostName = SO_SNDBUF; return true;
		case PSP_SO_RCVBUF: *hostName = SO_RCVBUF; return true;
		case PSP_SO_SNDLOWAT: *hostName = SO_SNDLOWAT; return true;
		case PSP_SO_RCVLOWAT: *hostName = SO_RCVLOWAT; return true;
		case PSP_SO_SNDTIMEO: *hostName = SO_SNDTIMEO; return true;
		case PSP_SO_RCVTIMEO: *hostName = SO_RCVTIMEO; return true;
		case PSP_SO_ERROR: *hostName = SO_ERROR; return true;
		case PSP_SO_TYPE: *hostName = SO_TYPE; return true;
		default: return false;
		}
	}
	if (level == PSP_IPPROTO_TCP) {
		*hostLevel = IPPROTO_TCP;
		switch (name) {
		case PSP_TCP_NODELAY: *hostName = TCP_NODELAY; return true;
		case PSP_TCP_MAXSEG: *hostName = TCP_MAXSEG; return true;
		default: return false;
		}
	}
	if (level == PSP_IPPROTO_IP) {
		*hostLevel = IPPROTO_IP;
		switch (name) {
		case PSP_IP_OPTIONS: *hostName = IP_OPTIONS; return true;
		case PSP_IP_HDRINCL: *hostName = IP_HDRINCL; return true;
		case PSP_IP_TOS: *hostName = IP_TOS; return true;
		case PSP_IP_TTL: *hostName = IP_TTL; return true;
		case PSP_IP_MULTICAST_IF: *hostName = IP_MULTICAST_IF; return true;
		case PSP_IP_MULTICAST_TTL: *hostName = IP_MULTICAST_TTL; return true;
		case PSP_IP_MULTICAST_LOOP: *hostName = IP_MULTICAST_LOOP; return true;
		case PSP_IP_ADD_MEMBERSHIP: *hostName = IP_ADD_MEMBERSHIP; return true;
		case PSP_IP_DROP_MEMBERSHIP: *hostName = IP_DROP_MEMBERSHIP; return true;
		default: return false;
		}
	}
	return false;
}

int MapMsgFlags(int f) {
	int h = 0;
	if (f & PSP_MSG_OOB) h |= MSG_OOB;
	if (f & PSP_MSG_PEEK) h |= MSG_PEEK;
	if (f & PSP_MSG_DONTROUTE) h |= MSG_DONTROUTE;
	return h;
}

// ---------------------------------------------------------------- sockets

namespace {
constexpr long kAllEvents = FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE;

bool AttachEvent(SOCKET s, WSAEVENT *ev, int *wsaError) {
	WSAEVENT e = WSACreateEvent();
	if (e == WSA_INVALID_EVENT) {
		if (wsaError) *wsaError = WSAGetLastError();
		return false;
	}
	// WSAEventSelect also makes the socket non-blocking, for good.
	if (WSAEventSelect(s, e, kAllEvents) != 0) {
		if (wsaError) *wsaError = WSAGetLastError();
		WSACloseEvent(e);
		return false;
	}
	*ev = e;
	return true;
}
}  // namespace

Socket OpenSocket(int af, int type, int protocol, int *wsaError) {
	Socket out;
	if (!Startup()) {
		if (wsaError) *wsaError = WSANOTINITIALISED;
		return out;
	}
	SOCKET s = socket(af, type, protocol);
	if (s == INVALID_SOCKET) {
		if (wsaError) *wsaError = WSAGetLastError();
		return out;
	}
	if (type == SOCK_DGRAM) {
		// No WSAECONNRESET on recvfrom after an ICMP port unreachable (BSD doesn't report it).
		BOOL off = FALSE;
		DWORD bytes = 0;
		WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
	}
	if (!AttachEvent(s, &out.ev, wsaError)) {
		closesocket(s);
		return out;
	}
	out.s = s;
	return out;
}

bool AdoptSocket(SOCKET s, Socket *out, int *wsaError) {
	*out = Socket{};
	if (!AttachEvent(s, &out->ev, wsaError)) return false;
	out->s = s;
	return true;
}

void CloseSocket(Socket &sock, bool reset) {
	if (sock.s != INVALID_SOCKET) {
		if (reset) {
			linger l{1, 0};
			setsockopt(sock.s, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof(l));
		}
		closesocket(sock.s);
		sock.s = INVALID_SOCKET;
	}
	if (sock.ev != WSA_INVALID_EVENT) {
		ReleaseEvent(sock.ev);
		sock.ev = WSA_INVALID_EVENT;
	}
}

bool CheckReady(SOCKET s, Readiness *r) {
	*r = Readiness{};
	fd_set rd, wr, ex;
	FD_ZERO(&rd);
	FD_ZERO(&wr);
	FD_ZERO(&ex);
	FD_SET(s, &rd);
	FD_SET(s, &wr);
	FD_SET(s, &ex);
	timeval tv{0, 0};
	int n = select(0, &rd, &wr, &ex, &tv);
	if (n == SOCKET_ERROR) return false;
	r->read = FD_ISSET(s, &rd) != 0;
	r->write = FD_ISSET(s, &wr) != 0;
	r->except = FD_ISSET(s, &ex) != 0;
	return true;
}

int PendingError(SOCKET s) {
	int err = 0;
	int len = sizeof(err);
	if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) return WSAGetLastError();
	return err;
}

u32 BytesAvailable(SOCKET s) {
	u_long n = 0;
	if (ioctlsocket(s, FIONREAD, &n) != 0) return 0;
	return (u32)n;
}

// ---------------------------------------------------------------- poller

namespace {

struct WatchEntry {
	std::vector<WSAEVENT> events;
	std::function<void()> fired;
};

struct Poller {
	std::mutex mu;
	std::map<WatchId, WatchEntry> watches;
	std::vector<WSAEVENT> toClose;
	WatchId nextId = 1;
	HANDLE wake = nullptr;
	std::thread thread;
	bool running = false;
	bool stop = false;

	void EnsureRunning() {
		// Called with mu held.
		if (running) return;
		if (!wake) wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		stop = false;
		running = true;
		thread = std::thread([this] { Loop(); });
	}

	void Loop() {
		size_t rotate = 0;
		for (;;) {
			std::vector<HANDLE> handles;
			DWORD timeout = INFINITE;
			{
				std::unique_lock<std::mutex> lock(mu);
				if (stop) break;
				for (WSAEVENT e : toClose) WSACloseEvent(e);
				toClose.clear();
				std::vector<HANDLE> all;
				for (auto &w : watches)
					for (WSAEVENT e : w.second.events)
						if (std::find(all.begin(), all.end(), e) == all.end()) all.push_back(e);
				handles.push_back(wake);
				if (all.size() <= MAXIMUM_WAIT_OBJECTS - 1) {
					handles.insert(handles.end(), all.begin(), all.end());
				} else {
					// More events than one wait can hold: take turns, and don't wait long on any batch.
					for (size_t i = 0; i < MAXIMUM_WAIT_OBJECTS - 1; i++) handles.push_back(all[(rotate + i) % all.size()]);
					rotate += MAXIMUM_WAIT_OBJECTS - 1;
					timeout = 5;
				}
			}
			DWORD r = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, timeout);
			std::vector<std::function<void()>> fire;
			{
				std::unique_lock<std::mutex> lock(mu);
				if (stop) break;
				if (r == WAIT_FAILED) {
					// A handle went bad under us; rebuild after a short pause rather than spin.
					lock.unlock();
					Sleep(1);
					continue;
				}
				std::vector<WSAEVENT> signalled;
				for (size_t i = 1; i < handles.size(); i++) {
					// toClose events may be in `handles`; they haven't been closed yet (only this thread
					// closes them), so testing them is safe.
					if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) signalled.push_back(handles[i]);
				}
				for (WSAEVENT e : signalled) WSAResetEvent(e);
				if (!signalled.empty()) {
					for (auto it = watches.begin(); it != watches.end();) {
						bool hit = false;
						for (WSAEVENT e : it->second.events)
							if (std::find(signalled.begin(), signalled.end(), e) != signalled.end()) hit = true;
						if (hit) {
							fire.push_back(std::move(it->second.fired));
							it = watches.erase(it);
						} else {
							++it;
						}
					}
				}
			}
			for (auto &f : fire)
				if (f) f();
		}
		std::lock_guard<std::mutex> lock(mu);
		for (WSAEVENT e : toClose) WSACloseEvent(e);
		toClose.clear();
		running = false;
	}
};

Poller &ThePoller() {
	static Poller *p = new Poller();  // never destroyed: the thread may outlive static destructors
	return *p;
}

}  // namespace

WatchId Watch(const WSAEVENT *events, size_t count, std::function<void()> fired) {
	Poller &p = ThePoller();
	std::lock_guard<std::mutex> lock(p.mu);
	WatchId id = p.nextId++;
	if (count == 0) return id;
	WatchEntry w;
	w.events.assign(events, events + count);
	w.fired = std::move(fired);
	p.watches.emplace(id, std::move(w));
	p.EnsureRunning();
	SetEvent(p.wake);
	return id;
}

void Unwatch(WatchId id) {
	Poller &p = ThePoller();
	std::lock_guard<std::mutex> lock(p.mu);
	if (p.watches.erase(id) && p.wake) SetEvent(p.wake);
}

void ReleaseEvent(WSAEVENT ev) {
	if (ev == WSA_INVALID_EVENT) return;
	Poller &p = ThePoller();
	std::lock_guard<std::mutex> lock(p.mu);
	bool used = false;
	for (auto it = p.watches.begin(); it != p.watches.end();) {
		auto &evs = it->second.events;
		if (std::find(evs.begin(), evs.end(), ev) != evs.end()) {
			used = true;
			it = p.watches.erase(it);
		} else {
			++it;
		}
	}
	if (p.running) {
		// The poller may be waiting on it right now: let it close the event once it isn't.
		p.toClose.push_back(ev);
		SetEvent(p.wake);
	} else {
		(void)used;
		WSACloseEvent(ev);
	}
}

void StopPoller() {
	Poller &p = ThePoller();
	std::thread t;
	{
		std::lock_guard<std::mutex> lock(p.mu);
		if (!p.running) return;
		p.stop = true;
		SetEvent(p.wake);
		t = std::move(p.thread);
	}
	if (t.joinable()) t.join();
}

// ---------------------------------------------------------------- the host's configuration

std::string Ipv4ToString(u32 a) {
	char buf[16];
	snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
	return buf;
}

bool ParseIpv4(const char *s, u32 *out) {
	if (!s) return false;
	u32 parts[4];
	const char *p = s;
	for (int n = 0; n < 4; n++) {
		if (*p < '0' || *p > '9') return false;
		u32 v = 0;
		int digits = 0;
		while (*p >= '0' && *p <= '9') {
			v = v * 10 + (u32)(*p++ - '0');
			if (++digits > 3 || v > 255) return false;
		}
		parts[n] = v;
		if (n < 3) {
			if (*p != '.') return false;
			p++;
		}
	}
	if (*p) return false;
	*out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
	return true;
}

HostConfig QueryHostConfig() {
	HostConfig cfg;
	// Loopback fallback: a host without a network still "connects", but only reaches itself.
	cfg.ip = htonl(INADDR_LOOPBACK);
	cfg.netmask = htonl(0xFF000000);
	cfg.adapter = "loopback";
	if (!Startup()) return cfg;

	// The interface of the route to a public address. Only the routing table is consulted.
	DWORD bestIf = 0;
	sockaddr_in probe{};
	probe.sin_family = AF_INET;
	probe.sin_addr.s_addr = htonl(0x08080808);
	bool haveBest = GetBestInterfaceEx((sockaddr *)&probe, &bestIf) == NO_ERROR;

	ULONG size = 16 * 1024;
	std::vector<u8> buf;
	ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
	ULONG rc = ERROR_BUFFER_OVERFLOW;
	for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
		buf.resize(size);
		rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	}
	if (rc != NO_ERROR) return cfg;

	const IP_ADAPTER_ADDRESSES *chosen = nullptr;
	for (auto *a = (const IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		if (!a->FirstUnicastAddress) continue;
		if (haveBest && a->IfIndex == bestIf) {
			chosen = a;
			break;
		}
		if (!chosen && a->FirstGatewayAddress) chosen = a;
	}
	if (!chosen) return cfg;

	for (auto *u = chosen->FirstUnicastAddress; u; u = u->Next) {
		if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
		u32 ip = ((const sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
		bool apipa = (ip & 0xFFFF) == 0xFEA9;  // 169.254.x.x
		if (cfg.valid && apipa) continue;
		cfg.ip = ip;
		u32 prefix = u->OnLinkPrefixLength;
		cfg.netmask = prefix == 0 ? 0 : htonl(prefix >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix));
		cfg.valid = true;
		if (!apipa) break;
	}
	if (!cfg.valid) {
		cfg.ip = htonl(INADDR_LOOPBACK);
		cfg.netmask = htonl(0xFF000000);
		return cfg;
	}
	for (auto *g = chosen->FirstGatewayAddress; g; g = g->Next) {
		if (g->Address.lpSockaddr->sa_family == AF_INET) {
			cfg.gateway = ((const sockaddr_in *)g->Address.lpSockaddr)->sin_addr.s_addr;
			break;
		}
	}
	int n = 0;
	for (auto *d = chosen->FirstDnsServerAddress; d && n < 2; d = d->Next) {
		if (d->Address.lpSockaddr->sa_family == AF_INET) cfg.dns[n++] = ((const sockaddr_in *)d->Address.lpSockaddr)->sin_addr.s_addr;
	}
	// An adapter with IPv6-only DNS: the gateway usually answers DNS too.
	if (n == 0) cfg.dns[0] = cfg.gateway;
	int len = WideCharToMultiByte(CP_UTF8, 0, chosen->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
	if (len > 1) {
		cfg.adapter.assign((size_t)len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, chosen->FriendlyName, -1, cfg.adapter.data(), len, nullptr, nullptr);
	}
	return cfg;
}

// ---------------------------------------------------------------- name resolution

int ResolveIpv4(const std::string &host, u32 *out) {
	if (!Startup()) return WSANOTINITIALISED;
	addrinfo hints{};
	hints.ai_family = AF_INET;
	addrinfo *res = nullptr;
	int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
	if (rc != 0) return rc;
	rc = WSAHOST_NOT_FOUND;
	for (addrinfo *p = res; p; p = p->ai_next) {
		if (p->ai_family == AF_INET && p->ai_addr) {
			*out = ((const sockaddr_in *)p->ai_addr)->sin_addr.s_addr;
			rc = 0;
			break;
		}
	}
	freeaddrinfo(res);
	return rc;
}

int ReverseLookup(u32 addr, std::string *name) {
	if (!Startup()) return WSANOTINITIALISED;
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = addr;
	char host[NI_MAXHOST];
	int rc = getnameinfo((const sockaddr *)&sa, sizeof(sa), host, sizeof(host), nullptr, 0, NI_NAMEREQD);
	if (rc != 0) return rc;
	*name = host;
	return 0;
}

}  // namespace hostnet
