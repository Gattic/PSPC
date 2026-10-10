// SPDX-License-Identifier: GPL-2.0-or-later
//
// The selftest harness (see net_selftest.h) and the infrastructure scenarios: "inet" (sceNetApctl,
// sceNetResolver, sceNetInet over loopback) and "http" (sceHttp against a URL given as argument).
#include "runtime/hle/modules/net_selftest.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <utility>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/thread.h"
#include "runtime/kernel/threads.h"

namespace net {
// Defined with the ad hoc libraries (net_adhoc*.cpp).
void RegisterAdhocSelftests();
}  // namespace net

namespace net::selftest {

namespace {

struct Entry {
	std::string help;
	Scenario fn;
};

std::map<std::string, Entry> &Registry() {
	static std::map<std::string, Entry> r;
	return r;
}

// Native callbacks behind guest addresses: a fixed set of trampolines.
constexpr int kMaxCallbacks = 64;
std::function<void(Ctx *)> g_callbacks[kMaxCallbacks];
int g_numCallbacks = 0;

template <int N>
void Trampoline(Ctx *c) {
	if (g_callbacks[N]) g_callbacks[N](c);
}

template <int... I>
constexpr std::array<HostFn, sizeof...(I)> MakeTrampolines(std::integer_sequence<int, I...>) {
	return {&Trampoline<I>...};
}
const auto kTrampolines = MakeTrampolines(std::make_integer_sequence<int, kMaxCallbacks>{});

void RegisterInetScenarios();

void RegisterAll() {
	static bool done = false;
	if (done) return;
	done = true;
	RegisterInetScenarios();
	net::RegisterAdhocSelftests();
}

std::vector<std::string> Split(const std::string &s, char sep) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t e = s.find(sep, pos);
		out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
		if (e == std::string::npos) break;
		pos = e + 1;
	}
	return out;
}

}  // namespace

void Register(const char *name, const char *help, Scenario fn) { Registry()[name] = Entry{help, std::move(fn)}; }

void Print(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0) return;
	logging::GuestOutput(buf, (size_t)std::min<int>(n, (int)sizeof(buf) - 1));
}

bool Checker::Check(bool ok, const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Print("%s %s\n", ok ? "[ok]" : "[FAIL]", buf);
	if (!ok) failures++;
	return ok;
}

u32 Call(const char *lib, u32 nid, std::initializer_list<u32> args) {
	kernel::PspThread *t = sched::Current();
	u32 stub = hle::MakeGuestStub(lib, nid);
	if (!t || !stub) {
		Print("[FAIL] no native %s %08x\n", lib, nid);
		return 0x80020001;
	}
	Ctx *c = &t->ctx;
	u32 sp = c->r[29];
	std::vector<u32> regs;
	size_t i = 0;
	if (args.size() > 8) c->r[29] = (sp - 64) & ~15u;
	for (u32 a : args) {
		if (i < 8) regs.push_back(a);
		else GWrite32(c->r[29] + 4 * (u32)(i - 8), a);
		i++;
	}
	u32 r;
	switch (regs.size()) {
	case 0: r = cpu::CallGuest(c, stub, {}); break;
	case 1: r = cpu::CallGuest(c, stub, {regs[0]}); break;
	case 2: r = cpu::CallGuest(c, stub, {regs[0], regs[1]}); break;
	case 3: r = cpu::CallGuest(c, stub, {regs[0], regs[1], regs[2]}); break;
	case 4: r = cpu::CallGuest(c, stub, {regs[0], regs[1], regs[2], regs[3]}); break;
	case 5: r = cpu::CallGuest(c, stub, {regs[0], regs[1], regs[2], regs[3], regs[4]}); break;
	case 6: r = cpu::CallGuest(c, stub, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]}); break;
	case 7: r = cpu::CallGuest(c, stub, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6]}); break;
	default: r = cpu::CallGuest(c, stub, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7]}); break;
	}
	c->r[29] = sp;
	return r;
}

u32 Alloc(u32 size) {
	u32 a = kernel::g_userMem.Alloc(AlignUp(std::max<u32>(size, 4), 16), false, "net selftest");
	if (a) mem::Memset(a, 0, size);
	return a;
}

u32 Str(const std::string &s) {
	u32 a = Alloc((u32)s.size() + 1);
	if (a) mem::WriteBytes(a, s.c_str(), (u32)s.size() + 1);
	return a;
}

std::string ReadStr(u32 addr, u32 maxLen) { return mem::ReadString(addr, maxLen); }

u32 Callback(std::function<void(Ctx *)> fn) {
	if (g_numCallbacks >= kMaxCallbacks) return 0;
	int n = g_numCallbacks++;
	g_callbacks[n] = std::move(fn);
	u32 addr = kernel::AllocKernelMemory(8, "net selftest callback");
	GWrite32(addr, 0x03E00008);      // jr ra (never run: the dispatch table has the native code)
	GWrite32(addr + 4, 0x00000000);  // nop
	dispatch::Register(addr, kTrampolines[n]);
	return addr;
}

void Delay(u32 us) { Call("ThreadManForUser", 0xCEADEB47, {us}); }

u64 NowUs() { return sched::NowUs(); }

int Start(const std::string &spec) {
	RegisterAll();
	size_t colon = spec.find(':');
	std::string name = spec.substr(0, colon);
	std::vector<std::string> args;
	if (colon != std::string::npos) args = Split(spec.substr(colon + 1), ',');
	auto it = Registry().find(name);
	if (it == Registry().end()) {
		fprintf(stderr, "Unknown network selftest \"%s\". Scenarios:\n", name.c_str());
		for (auto &e : Registry()) fprintf(stderr, "  %-14s %s\n", e.first.c_str(), e.second.help.c_str());
		return -1;
	}
	Scenario fn = it->second.fn;
	u32 entry = kernel::AllocKernelMemory(16, "net selftest entry");
	GWrite32(entry, 0x03E00008);      // jr ra
	GWrite32(entry + 4, 0x00001021);  // move v0, zero
	SceUID tid = kernel::CreateThread("net-selftest", entry, 0x20, 0x20000, kernel::PSP_THREAD_ATTR_USER, 0, 0, false);
	if (tid < 0) return tid;
	kernel::PspThread *t = kernel::Get<kernel::PspThread>(tid);
	t->deleteOnReturn = true;
	t->onFirstRun = [fn, args, name] {
		Print("net-selftest %s: start\n", name.c_str());
		int failures = fn(args);
		Print("net-selftest %s: %s (%d failure%s)\n", name.c_str(), failures ? "FAILED" : "passed", failures,
		      failures == 1 ? "" : "s");
		sched::RequestStop(failures ? 1 : 0);
	};
	return kernel::StartThread(t, 0, 0);
}

// ================================================================ infrastructure scenarios

namespace {

// pspnet / sceNetInet / sceNetApctl / sceNetResolver / sceHttp calls used below.
constexpr const char *INET = "sceNetInet";
constexpr int AF_INET_PSP = 2, STREAM = 1, DGRAM = 2;
constexpr u32 SOL_SOCKET_PSP = 0xFFFF, SO_RCVTIMEO_PSP = 0x1006, SO_ERROR_PSP = 0x1007, SO_NBIO_PSP = 0x1009;

int Socket(int type) { return (int)Call(INET, 0x8B7B220F, {AF_INET_PSP, (u32)type, 0}); }
int Close(int s) { return (int)Call(INET, 0x8D7284EA, {(u32)s}); }
int Errno() { return (int)Call(INET, 0xFBABE411, {}); }

// A guest struct sockaddr_in for ip:port (ip in host order, e.g. 0x7F000001).
u32 Sockaddr(u32 ip, u16 port) {
	u32 a = Alloc(16);
	GWrite8(a, 16);
	GWrite8(a + 1, AF_INET_PSP);
	GWrite16(a + 2, htons(port));
	GWrite32(a + 4, htonl(ip));
	return a;
}

u16 LocalPort(int s) {
	u32 a = Alloc(16), len = Alloc(4);
	GWrite32(len, 16);
	Call(INET, 0x162E6FD5, {(u32)s, a, len});
	return ntohs(GRead16(a + 2));
}

int SetOpt(int s, u32 level, u32 name, u32 value) {
	u32 v = Alloc(4);
	GWrite32(v, value);
	return (int)Call(INET, 0x2FE71FE7, {(u32)s, level, name, v, 4});
}

u32 GetOpt(int s, u32 level, u32 name) {
	u32 v = Alloc(4), len = Alloc(4);
	GWrite32(len, 4);
	Call(INET, 0x4A114C7C, {(u32)s, level, name, v, len});
	return GRead32(v);
}

std::string ApctlInfoStr(int code) {
	u32 buf = Alloc(128);
	Call("sceNetApctl", 0x2BEFDF23, {(u32)code, buf});
	return ReadStr(buf, 128);
}

// Brings sceNet, sceNetInet and the access point up. Returns false (after reporting) on failure.
bool NetUp(Checker &ck, std::vector<std::array<u32, 4>> *events) {
	ck.Check(Call("sceUtility", 0x1579A159, {1}) == 0, "sceUtilityLoadNetModule(COMMON)");
	ck.Check(Call("sceUtility", 0x1579A159, {3}) == 0, "sceUtilityLoadNetModule(INET)");
	ck.Check(Call("sceNet", 0x39AF39A6, {0x20000, 0x20, 0x1000, 0x20, 0x1000}) == 0, "sceNetInit");
	ck.Check(Call(INET, 0x17943399, {}) == 0, "sceNetInetInit");
	ck.Check(Call("sceNetApctl", 0xE2F91F9B, {0x1800, 0x30}) == 0, "sceNetApctlInit");
	u32 handler = Callback([events](Ctx *c) {
		// (oldState, newState, event, error, arg)
		if (events) events->push_back({c->r[4], c->r[5], c->r[6], c->r[7]});
		c->r[2] = 0;
	});
	int hid = (int)Call("sceNetApctl", 0x8ABADD51, {handler, 0x1234});
	ck.Check(hid >= 0, "sceNetApctlAddHandler -> %d", hid);
	int r = (int)Call("sceNetApctl", 0xCFB957C6, {1});
	if (!ck.Check(r == 0, "sceNetApctlConnect(1) -> %08x", (u32)r)) return false;
	u32 statePtr = Alloc(4);
	u64 start = NowUs();
	u32 state = 0, last = 99;
	while (NowUs() - start < 10000000) {
		Call("sceNetApctl", 0x5DEAC81B, {statePtr});
		state = GRead32(statePtr);
		if (state != last) Print("  apctl state %u\n", state);
		last = state;
		if (state == 4) break;
		Delay(20000);
	}
	return ck.Check(state == 4, "connected (GOT_IP)");
}

void NetDown(Checker &ck) {
	ck.Check(Call("sceNetApctl", 0x24FE91A1, {}) == 0, "sceNetApctlDisconnect");
	ck.Check(Call("sceNetApctl", 0xB3EDD0EC, {}) == 0, "sceNetApctlTerm");
	ck.Check(Call(INET, 0xA9ED66B9, {}) == 0, "sceNetInetTerm");
	ck.Check(Call("sceNet", 0x281928A9, {}) == 0, "sceNetTerm");
}

// A guest thread running `body` natively; returns its UID (started).
int StartGuestThread(const char *name, std::function<void()> body) {
	u32 entry = Callback([body](Ctx *c) {
		body();
		c->r[2] = 0;
	});
	int th = (int)Call("ThreadManForUser", 0x446D8DE6, {Str(name), entry, 0x20, 0x10000, 0, 0});
	if (th > 0) Call("ThreadManForUser", 0xF475845D, {(u32)th, 0, 0});
	return th;
}

void WaitThread(int th) {
	Call("ThreadManForUser", 0x278C0DF5, {(u32)th, 0});
	Call("ThreadManForUser", 0x9FA03CD3, {(u32)th});
}

int InetScenario(const std::vector<std::string> &) {
	Checker ck;
	std::vector<std::array<u32, 4>> events;
	if (!NetUp(ck, &events)) return ck.failures + 1;

	// The handler thread told us about every step: (old, new, event).
	Delay(50000);
	bool sawRequest = false, sawEstablished = false, sawGetIp = false;
	for (auto &e : events) {
		Print("  handler: %u -> %u, event %u, error %08x\n", e[0], e[1], e[2], e[3]);
		sawRequest |= e[0] == 0 && e[1] == 2 && e[2] == 0;
		sawEstablished |= e[0] == 2 && e[1] == 3 && e[2] == 3;
		sawGetIp |= e[0] == 3 && e[1] == 4 && e[2] == 4;
	}
	ck.Check(sawRequest && sawEstablished && sawGetIp, "apctl handler saw CONNECT_REQUEST, ESTABLISHED, GET_IP");

	std::string ip = ApctlInfoStr(8), mask = ApctlInfoStr(9), gw = ApctlInfoStr(10), dns = ApctlInfoStr(11);
	Print("  GetInfo: profile \"%s\" ssid \"%s\" ip %s mask %s gateway %s dns %s\n", ApctlInfoStr(0).c_str(),
	      ApctlInfoStr(2).c_str(), ip.c_str(), mask.c_str(), gw.c_str(), dns.c_str());
	ck.Check(ApctlInfoStr(2) == "PSPC", "GetInfo SSID is PSPC");
	ck.Check(!ip.empty() && ip != "0.0.0.0", "GetInfo IP is the host's (%s)", ip.c_str());

	// ---- sceNetResolver
	ck.Check(Call("sceNetResolver", 0xF3370E61, {}) == 0, "sceNetResolverInit");
	u32 ridPtr = Alloc(4), rbuf = Alloc(1024), addr = Alloc(4);
	ck.Check(Call("sceNetResolver", 0x244172AF, {ridPtr, rbuf, 1024}) == 0, "sceNetResolverCreate");
	u32 rid = GRead32(ridPtr);
	int r = (int)Call("sceNetResolver", 0x224C5F44, {rid, Str("localhost"), addr, 2, 3});
	ck.Check(r == 0 && GRead32(addr) == htonl(0x7F000001), "resolve localhost -> %08x (%08x)", GRead32(addr), (u32)r);
	r = (int)Call("sceNetResolver", 0x224C5F44, {rid, Str("no-such-host.invalid"), addr, 2, 1});
	ck.Check(r == (int)0x80410414, "resolve no-such-host.invalid fails with INVALID_HOST (%08x)", (u32)r);
	ck.Check(Call("sceNetResolver", 0x94523E09, {rid}) == 0, "sceNetResolverDelete");

	// ---- TCP: a server here, a client on another guest thread; every call blocks.
	int server = Socket(STREAM);
	ck.Check(server > 0, "socket(STREAM) -> %d", server);
	ck.Check(Call(INET, 0x1A33F9AE, {(u32)server, Sockaddr(0x7F000001, 0), 16}) == 0, "bind 127.0.0.1:0");
	ck.Check(Call(INET, 0xD10A1A7A, {(u32)server, 1}) == 0, "listen");
	u16 port = LocalPort(server);
	ck.Check(port != 0, "getsockname port %u", port);
	struct ClientResult {
		int connect = -2, sent = -2, received = -2;
		std::string reply;
	};
	auto client = std::make_shared<ClientResult>();
	int th = StartGuestThread("tcp-client", [client, port] {
		int s = Socket(STREAM);
		client->connect = (int)Call(INET, 0x410B34AA, {(u32)s, Sockaddr(0x7F000001, port), 16});
		u32 msg = Str("ping");
		client->sent = (int)Call(INET, 0x7AA671BC, {(u32)s, msg, 4, 0});
		u32 buf = Alloc(16);
		client->received = (int)Call(INET, 0xCDA85C99, {(u32)s, buf, 4, hostnet::PSP_MSG_WAITALL});
		client->reply = ReadStr(buf, 4);
		Close(s);
	});
	ck.Check(th > 0, "client thread started");
	u32 peerAddr = Alloc(16), peerLen = Alloc(4);
	GWrite32(peerLen, 16);
	int conn = (int)Call(INET, 0xDB094E1B, {(u32)server, peerAddr, peerLen});
	ck.Check(conn > 0, "accept (blocking) -> %d from port %u", conn, ntohs(GRead16(peerAddr + 2)));
	u32 buf = Alloc(64);
	int n = (int)Call(INET, 0xCDA85C99, {(u32)conn, buf, 4, hostnet::PSP_MSG_WAITALL});
	ck.Check(n == 4 && ReadStr(buf, 4) == "ping", "recv (blocking) \"%s\"", ReadStr(buf, 4).c_str());
	n = (int)Call(INET, 0x7AA671BC, {(u32)conn, Str("pong"), 4, 0});
	ck.Check(n == 4, "send -> %d", n);
	n = (int)Call(INET, 0xCDA85C99, {(u32)conn, buf, 64, 0});
	ck.Check(n == 0, "recv after the client closed -> %d (end of stream)", n);
	WaitThread(th);
	ck.Check(client->connect == 0 && client->sent == 4 && client->received == 4 && client->reply == "pong",
	         "client thread: connect %d, send %d, recv %d \"%s\"", client->connect, client->sent, client->received,
	         client->reply.c_str());
	Close(conn);

	// ---- Non-blocking connect to a port nobody listens on: EINPROGRESS, then select, SO_ERROR.
	Close(server);  // `port` is free now
	int nb = Socket(STREAM);
	SetOpt(nb, SOL_SOCKET_PSP, SO_NBIO_PSP, 1);
	r = (int)Call(INET, 0x410B34AA, {(u32)nb, Sockaddr(0x7F000001, port), 16});
	int e = Errno();
	ck.Check(r == -1 && e == hostnet::PSP_EINPROGRESS, "non-blocking connect -> %d, errno %d (EINPROGRESS)", r, e);
	u32 wset = Alloc(32), tv = Alloc(8);
	GWrite32(wset, 1u << nb);
	GWrite32(tv, 6);  // seconds (Windows reports a refused loopback connect after about 2 s)
	u64 t0 = NowUs();
	r = (int)Call(INET, 0x5BE8D595, {(u32)nb + 1, 0, wset, 0, tv});
	ck.Check(r == 1 && (GRead32(wset) >> nb & 1), "select(write) -> %d after %llu ms", r, (unsigned long long)((NowUs() - t0) / 1000));
	u32 soerr = GetOpt(nb, SOL_SOCKET_PSP, SO_ERROR_PSP);
	ck.Check(soerr == hostnet::PSP_ECONNREFUSED, "SO_ERROR = %u (ECONNREFUSED)", soerr);
	Close(nb);

	// ---- UDP: receive time-out, poll, datagrams and their source.
	int ua = Socket(DGRAM), ub = Socket(DGRAM);
	ck.Check(Call(INET, 0x1A33F9AE, {(u32)ua, Sockaddr(0x7F000001, 0), 16}) == 0 &&
	             Call(INET, 0x1A33F9AE, {(u32)ub, Sockaddr(0x7F000001, 0), 16}) == 0,
	         "bind two UDP sockets");
	u16 pa = LocalPort(ua), pb = LocalPort(ub);
	SetOpt(ub, SOL_SOCKET_PSP, SO_RCVTIMEO_PSP, 200000);
	u32 from = Alloc(16), fromLen = Alloc(4);
	GWrite32(fromLen, 16);
	t0 = NowUs();
	r = (int)Call(INET, 0xC91142E4, {(u32)ub, buf, 64, 0, from, fromLen});
	u64 waited = NowUs() - t0;
	e = Errno();
	ck.Check(r == -1 && e == hostnet::PSP_EAGAIN && waited >= 150000, "recvfrom times out: %d errno %d after %llu ms", r, e,
	         (unsigned long long)(waited / 1000));
	r = (int)Call(INET, 0xCDA85C99, {(u32)ub, buf, 64, 0x80 /* MSG_DONTWAIT */});
	ck.Check(r == -1 && Errno() == hostnet::PSP_EAGAIN, "recv(MSG_DONTWAIT) -> EAGAIN");
	n = (int)Call(INET, 0x05038FC7, {(u32)ua, Str("dgram"), 5, 0, Sockaddr(0x7F000001, pb), 16});
	ck.Check(n == 5, "sendto -> %d", n);
	u32 fds = Alloc(8);
	GWrite32(fds, (u32)ub);
	GWrite16(fds + 4, 0x0001);  // POLLIN
	r = (int)Call(INET, 0xFAABB1DD, {fds, 1, 2000});
	ck.Check(r == 1 && (GRead16(fds + 6) & 1), "poll(POLLIN) -> %d, revents %04x", r, GRead16(fds + 6));
	GWrite32(fromLen, 16);
	n = (int)Call(INET, 0xC91142E4, {(u32)ub, buf, 64, 0, from, fromLen});
	ck.Check(n == 5 && ReadStr(buf, 5) == "dgram" && ntohs(GRead16(from + 2)) == pa, "recvfrom \"%s\" from port %u",
	         ReadStr(buf, 5).c_str(), ntohs(GRead16(from + 2)));

	// ---- sceNetInetSocketAbort ends a blocked receive.
	auto aborted = std::make_shared<std::pair<int, int>>(-2, 0);
	SetOpt(ub, SOL_SOCKET_PSP, SO_RCVTIMEO_PSP, 0);
	th = StartGuestThread("udp-reader", [aborted, ub] {
		u32 b = Alloc(16);
		aborted->first = (int)Call(INET, 0xCDA85C99, {(u32)ub, b, 16, 0});
		aborted->second = Errno();
	});
	Delay(100000);
	ck.Check(aborted->first == -2, "reader is blocked");
	Call(INET, 0x80A21ABD, {(u32)ub});
	WaitThread(th);
	ck.Check(aborted->first == -1 && aborted->second == hostnet::PSP_EINTR, "aborted recv -> %d, errno %d (EINTR)",
	         aborted->first, aborted->second);
	Close(ua);
	Close(ub);

	events.clear();
	NetDown(ck);
	return ck.failures;
}

// ---- sceHttp

constexpr const char *HTTP = "sceHttp";

struct HttpResult {
	int send = -1, status = -1, lengthResult = -1;
	u64 length = 0;
	std::string body, headers;
};

// One request through sceHttp: (method 0 GET / 1 POST / 2 HEAD).
HttpResult HttpFetch(int tmpl, const std::string &url, int method = 0, const std::string &post = "", u32 readChunk = 4096) {
	HttpResult res;
	int conn = (int)Call(HTTP, 0xCDF8ECB9, {(u32)tmpl, Str(url), 0});
	if (conn < 0) {
		res.send = conn;
		return res;
	}
	// sceHttpCreateRequestWithURL(conn, method, url, u64 contentLength): the u64 takes $t0:$t1.
	int req = (int)Call(HTTP, 0xB509B09E, {(u32)conn, (u32)method, Str(url), 0, (u32)post.size(), 0});
	if (req < 0) {
		res.send = req;
		Call(HTTP, 0x5152773B, {(u32)conn});
		return res;
	}
	u32 data = post.empty() ? 0 : Str(post);
	res.send = (int)Call(HTTP, 0xBB70706F, {(u32)req, data, (u32)post.size()});
	if (res.send == 0) {
		u32 st = Alloc(4), len = Alloc(8);
		Call(HTTP, 0x4CC7D78F, {(u32)req, st});
		res.status = (int)GRead32(st);
		res.lengthResult = (int)Call(HTTP, 0x0282A3BD, {(u32)req, len});
		res.length = GRead32(len) | ((u64)GRead32(len + 4) << 32);
		u32 hp = Alloc(4), hs = Alloc(4);
		if (Call(HTTP, 0xDB266CCF, {(u32)req, hp, hs}) == 0) res.headers = ReadStr(GRead32(hp), GRead32(hs));
		u32 buf = Alloc(readChunk);
		for (int guard = 0; guard < 100000; guard++) {
			int n = (int)Call(HTTP, 0xEDEEB999, {(u32)req, buf, readChunk});
			if (n <= 0) break;
			std::string chunk((size_t)n, '\0');
			mem::ReadBytes(chunk.data(), buf, (u32)n);
			res.body += chunk;
		}
		kernel::g_userMem.Free(buf);
	}
	Call(HTTP, 0xA5512E01, {(u32)req});
	Call(HTTP, 0x5152773B, {(u32)conn});
	return res;
}

int HttpScenario(const std::vector<std::string> &args) {
	Checker ck;
	if (args.empty() || args[0].empty()) {
		Print("usage: --net-selftest http:<base url>[,internet]\n");
		return 1;
	}
	std::string base = args[0];
	bool internet = args.size() > 1 && args[1] == "internet";

	// Before the access point: names don't resolve (no DNS server), as in pspautotests net/http.
	ck.Check(Call("sceNet", 0x39AF39A6, {0x20000, 0x20, 0x1000, 0x20, 0x1000}) == 0, "sceNetInit");
	ck.Check(Call(HTTP, 0xAB1ABE07, {0x25800}) == 0, "sceHttpInit");
	int tmpl = (int)Call(HTTP, 0x9B1F1F36, {Str("PSPC-selftest/1.0"), 1, 0});
	ck.Check(tmpl > 0, "sceHttpCreateTemplate -> %d", tmpl);
	HttpResult early = HttpFetch(tmpl, "http://www.example.com/");
	ck.Check(early.send == (int)0x80410402, "send before connecting -> %08x (NO_DNS_SERVER)", (u32)early.send);
	ck.Check(Call(HTTP, 0xD1C8945E, {}) == 0, "sceHttpEnd");
	ck.Check(Call("sceNet", 0x281928A9, {}) == 0, "sceNetTerm");

	if (!NetUp(ck, nullptr)) return ck.failures + 1;
	for (u32 m : {4u, 5u, 6u}) Call("sceUtility", 0x1579A159, {m});
	ck.Check(Call("sceSsl", 0x957ECBE2, {0x28000}) == 0, "sceSslInit");
	ck.Check(Call(HTTP, 0xAB1ABE07, {0x25800}) == 0, "sceHttpInit");
	ck.Check(Call(HTTP, 0xE4D21302, {0, 0, 0, 0}) == 0, "sceHttpsInit");
	tmpl = (int)Call(HTTP, 0x9B1F1F36, {Str("PSPC-selftest/1.0"), 1, 0});

	const std::string hello = "Hello from the PSPC net selftest.\n";
	HttpResult r = HttpFetch(tmpl, base + "/hello.txt");
	ck.Check(r.send == 0 && r.status == 200, "GET /hello.txt -> send %08x, status %d", (u32)r.send, r.status);
	ck.Check(r.lengthResult == 0 && r.length == hello.size(), "content length %llu", (unsigned long long)r.length);
	ck.Check(r.body == hello, "body (%u bytes)", (u32)r.body.size());
	ck.Check(r.headers.rfind("HTTP/1.1 200", 0) == 0 && r.headers.find("X-Selftest: yes") != std::string::npos &&
	             r.headers.find("X-Agent-Seen: PSPC-selftest/1.0") != std::string::npos,
	         "sceHttpGetAllHeader has the status line, headers and our user agent");

	r = HttpFetch(tmpl, base + "/redirect");
	ck.Check(r.status == 200 && r.body == hello, "GET /redirect follows to /hello.txt (status %d)", r.status);
	// Redirects off on a template of its own.
	int tmpl2 = (int)Call(HTTP, 0x9B1F1F36, {Str("PSPC-selftest/1.0"), 1, 0});
	Call(HTTP, 0x1A0EBB69, {(u32)tmpl2});  // sceHttpDisableRedirect
	r = HttpFetch(tmpl2, base + "/redirect");
	ck.Check(r.status == 302, "with redirects disabled: status %d", r.status);
	Call(HTTP, 0xFCF8C055, {(u32)tmpl2});

	r = HttpFetch(tmpl, base + "/big", 0, "", 3000);
	u32 sum = 0;
	bool same = r.body.size() == 200 * 1024;
	for (size_t i = 0; same && i < r.body.size(); i++) same = (u8)r.body[i] == (u8)((i * 7) & 0xFF);
	(void)sum;
	ck.Check(r.status == 200 && same, "GET /big: %u bytes in 3000-byte reads, content intact", (u32)r.body.size());

	r = HttpFetch(tmpl, base + "/chunked");
	ck.Check(r.status == 200 && r.lengthResult == (int)0x80431071 && r.body == "chunk one, chunk two, end.",
	         "GET /chunked: no content length (%08x), body \"%s\"", (u32)r.lengthResult, r.body.c_str());

	r = HttpFetch(tmpl, base + "/post", 1, "abc");
	ck.Check(r.status == 200 && r.body == "got abc", "POST /post -> \"%s\"", r.body.c_str());

	r = HttpFetch(tmpl, base + "/hello.txt", 2);
	ck.Check(r.status == 200 && r.body.empty() && r.length == hello.size(), "HEAD /hello.txt: status %d, no body", r.status);

	r = HttpFetch(tmpl, base + "/missing");
	ck.Check(r.status == 404, "GET /missing -> status %d", r.status);

	r = HttpFetch(tmpl, "http://no-such-host.invalid/");
	ck.Check(r.send == (int)0x80410414, "unknown host -> %08x (resolver INVALID_HOST)", (u32)r.send);

	if (internet) {
		r = HttpFetch(tmpl, "http://example.com/");
		ck.Check(r.status == 200 && r.body.find("Example Domain") != std::string::npos, "GET http://example.com/ -> %d (%u bytes)",
		         r.status, (u32)r.body.size());
		r = HttpFetch(tmpl, "https://example.com/");
		ck.Check(r.status == 200 && r.body.find("Example Domain") != std::string::npos, "GET https://example.com/ -> %d (%u bytes)",
		         r.status, (u32)r.body.size());
	}

	ck.Check(Call(HTTP, 0xFCF8C055, {(u32)tmpl}) == 0, "sceHttpDeleteTemplate");
	ck.Check(Call(HTTP, 0xF9D8EB63, {}) == 0, "sceHttpsEnd");
	ck.Check(Call(HTTP, 0xD1C8945E, {}) == 0, "sceHttpEnd");
	ck.Check(Call("sceSsl", 0x191CDEFF, {}) == 0, "sceSslEnd");
	NetDown(ck);
	return ck.failures;
}

void RegisterInetScenarios() {
	Register("inet", "access point, resolver and BSD sockets over loopback", InetScenario);
	Register("http", "sceHttp against <base url> (tools/net_selftest.py serves one); arg 2 \"internet\": also example.com",
	         HttpScenario);
}

}  // namespace

}  // namespace net::selftest
