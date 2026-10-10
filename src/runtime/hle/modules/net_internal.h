// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared pieces of the network libraries (net*.cpp).
//
// With networking on (pspc.ini network=1, the default) the WLAN switch reads on: configuration 1
// "PSPC" connects to a virtual access point that is the host's own network (sceNetApctl), BSD
// sockets are Winsock sockets (sceNetInet), names resolve through the host (sceNetResolver), sceHttp
// talks HTTP/HTTPS, and ad hoc play goes through a PPSSPP-compatible ad hoc server. With network=0
// the switch reads off and the libraries fail the way the firmware does without a network.
//
// Host sockets are always non-blocking. A guest call that blocks puts its guest thread to sleep
// (sched::WaitUntil) until the poller thread reports the socket ready (WaitHostEvents below), so
// other guest threads keep running and the emulation thread never blocks.
#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>

#include "common/common.h"
#include "net/host_net.h"
#include "runtime/hle/modules/net_hle.h"
#include "runtime/kernel/kobject.h"

namespace net {

// Registration of the libraries implemented in the other net_*.cpp files (called by
// Register_NetLibraries in net.cpp).
void RegisterParseLibraries();  // net_parse.cpp: sceParseUri, sceParseHttp
void RegisterHttpLibraries();   // net_http.cpp: sceHttp, sceSsl, sceHttpStorage
void RegisterAdhocLibraries();  // net_adhoc.cpp: sceNetAdhoc, sceNetAdhocctl, sceNetAdhocMatching, sceNetAdhocDiscover
void RegisterNpLibraries();     // net_np.cpp: sceNp, sceNpAuth, sceNpService

// The console's wireless MAC address. Fixed (00:16:FE:50:53:43) for the first PSPC instance on a
// PC with a local ad hoc server; a second instance, or play through an ad hoc server elsewhere,
// gets its own (see Configure in net_host.cpp). Set before the program boots.
extern u8 kLocalMac[6];
// Player nickname reported by the network libraries.
constexpr const char *kNickname = "PSPC";

// ---------------------------------------------------------------- configuration (net_host.cpp)

const Config &CurrentConfig();
// Which PSPC process this is among those running networked on this PC (1, 2, ...). Inherited by
// LoadExec / XMB relaunches. Two instances on one PC take different loopback addresses
// (127.0.0.<instance>) for ad hoc play.
int InstanceNumber();

// State owned by net.cpp.
bool NetInited();
// Called by sceNetTerm: the ad hoc libraries shut down with the core library.
void AdhocShutdown();
// The virtual access point's lease (network byte order; zero while not connected).
struct ApctlLease {
	u32 ip = 0, netmask = 0, gateway = 0, dns1 = 0, dns2 = 0;
};
ApctlLease ApctlCurrentLease();
// True once sceNetApctl reached GOT_IP: the PSP then has a DNS server to ask.
inline bool HaveDnsServer() { return ApctlGetState() == APCTL_STATE_GOT_IP; }

// Errno values of the PSP's BSD socket layer (newlib numbering), as sceNetInetGetErrno returns them.
enum InetErrno : int {
	INET_EINTR = hostnet::PSP_EINTR,
	INET_EBADF = hostnet::PSP_EBADF,
	INET_EAGAIN = hostnet::PSP_EAGAIN,
	INET_EACCES = hostnet::PSP_EACCES,
	INET_EFAULT = hostnet::PSP_EFAULT,
	INET_EINVAL = hostnet::PSP_EINVAL,
	INET_ENOSPC = hostnet::PSP_ENOSPC,
	INET_EPIPE = hostnet::PSP_EPIPE,
	INET_EOPNOTSUPP = hostnet::PSP_EOPNOTSUPP,
	INET_EPFNOSUPPORT = hostnet::PSP_EPFNOSUPPORT,
	INET_ECONNRESET = hostnet::PSP_ECONNRESET,
	INET_ENOBUFS = hostnet::PSP_ENOBUFS,
	INET_EAFNOSUPPORT = hostnet::PSP_EAFNOSUPPORT,
	INET_EPROTOTYPE = hostnet::PSP_EPROTOTYPE,
	INET_ENOTSOCK = hostnet::PSP_ENOTSOCK,
	INET_ENOPROTOOPT = hostnet::PSP_ENOPROTOOPT,
	INET_ESHUTDOWN = hostnet::PSP_ESHUTDOWN,
	INET_ECONNREFUSED = hostnet::PSP_ECONNREFUSED,
	INET_EADDRINUSE = hostnet::PSP_EADDRINUSE,
	INET_ECONNABORTED = hostnet::PSP_ECONNABORTED,
	INET_ENETUNREACH = hostnet::PSP_ENETUNREACH,
	INET_ENETDOWN = hostnet::PSP_ENETDOWN,
	INET_ETIMEDOUT = hostnet::PSP_ETIMEDOUT,
	INET_EHOSTDOWN = hostnet::PSP_EHOSTDOWN,
	INET_EHOSTUNREACH = hostnet::PSP_EHOSTUNREACH,
	INET_EINPROGRESS = hostnet::PSP_EINPROGRESS,
	INET_EALREADY = hostnet::PSP_EALREADY,
	INET_EDESTADDRREQ = hostnet::PSP_EDESTADDRREQ,
	INET_EMSGSIZE = hostnet::PSP_EMSGSIZE,
	INET_EPROTONOSUPPORT = hostnet::PSP_EPROTONOSUPPORT,
	INET_ESOCKTNOSUPPORT = hostnet::PSP_ESOCKTNOSUPPORT,
	INET_EADDRNOTAVAIL = hostnet::PSP_EADDRNOTAVAIL,
	INET_EISCONN = hostnet::PSP_EISCONN,
	INET_ENOTCONN = hostnet::PSP_ENOTCONN,
};

// Error codes of the core / WLAN layers used by more than one file.
constexpr u32 SCE_NET_RESOLVER_NO_DNS_SERVER = 0x80410402;

// ---------------------------------------------------------------- guest <-> host glue (net_host.cpp)

// Host sockets the guest owns: while any is open, fast-time mode idles in real time
// (sched::HoldRealTime), so network time-outs keep their meaning. Pair every Opened with a Closed.
void HostSocketOpened();
void HostSocketClosed();

enum WaitResult : int {
	HOSTWAIT_TIMEOUT = 0,  // the deadline passed (or the periodic re-check came due; see below)
	HOSTWAIT_READY = 1,    // one of the events fired
	HOSTWAIT_ABORTED = 2,  // AbortWaits(key) was called, or the thread couldn't wait at all
};
// Puts the current guest thread to sleep until one of the host socket events fires, `deadlineUs`
// (sched::NowUs time, 0 = none) passes, or AbortWaits(abortKey) is called. Also returns
// HOSTWAIT_TIMEOUT after at most `recheckUs` so the caller re-checks the real socket state (readiness
// events are edge-like). Callers loop: try the operation, wait, try again.
WaitResult WaitHostEvents(const WSAEVENT *events, size_t count, u64 deadlineUs, u64 abortKey, u64 recheckUs = 100000);
// Wakes every guest thread in WaitHostEvents with this key (socket closed or aborted).
void AbortWaits(u64 abortKey);

// Runs `work` on a host worker thread and puts the current guest thread to sleep until it returns
// (or `deadlineUs` passes: then returns false and the work's result is dropped). For blocking host
// calls such as getaddrinfo or WinHTTP. `work` must not touch guest memory or guest state.
bool RunBlockingOnHost(std::function<void()> work, u64 deadlineUs, u64 abortKey = 0);

// A guest thread that runs `body` natively, like the threads the firmware's network libraries start
// (SceNetApctl, SceNetAdhocctl, matching threads...). `body` may block (sched::Wait...) and call
// guest handlers with cpu::CallGuest(&sched::Current()->ctx, ...). The thread runs with the gp of
// the guest thread that started it, so handlers in the game see the game's gp. It is deleted when
// `body` returns. Returns the thread UID or a negative error.
SceUID StartLibraryThread(const char *name, int priority, int stackSize, std::function<void()> body);

// A library thread (StartLibraryThread) that runs work items posted from the emulation thread, in
// order: the libraries call their guest handlers from such items (cpu::CallGuest on
// sched::Current()). Stop() ends the thread after the item it is running; queued items are dropped.
// Copies share the thread.
class HandlerThread {
public:
	bool Start(const char *name, int priority, int stackSize);
	void Stop();
	bool Running() const;
	void Post(std::function<void()> item);  // emulation thread only
	// Calls a guest function on the current (library) thread and returns $v0.
	static u32 CallHandler(u32 func, std::initializer_list<u32> args);

private:
	struct State;
	std::shared_ptr<State> st_;
};

}  // namespace net
