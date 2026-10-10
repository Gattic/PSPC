// SPDX-License-Identifier: GPL-2.0-or-later
//
// Glue between the network libraries' guest side and the host (see net_internal.h): the per-run
// configuration, the instance number and MAC address, guest threads sleeping on host socket events
// or on blocking host calls, and the guest threads the libraries run their handlers on.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <thread>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/modules/console_id.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/thread.h"
#include "runtime/kernel/threads.h"

namespace net {

// The default MAC: a unicast, globally administered address (games such as Gran Turismo check the
// two low bits of the first byte). 00:16:FE is the PSP's WLAN chip vendor.
u8 kLocalMac[6] = {0x00, 0x16, 0xFE, 0x50, 0x53, 0x43};

namespace {

constexpr const char *LOG = "NET";

Config g_config;
int g_instance = 1;
HANDLE g_instanceMutex = nullptr;
int g_hostSockets = 0;

// One number per networked PSPC process on this PC, kept for the process's lifetime. A relaunch
// (LoadExec, back to the XMB) inherits the number through the environment: the parent waits for
// it and still holds the mutex.
int ClaimInstance() {
	char buf[16];
	DWORD n = GetEnvironmentVariableA("PSPC_NET_INSTANCE", buf, sizeof(buf));
	if (n > 0 && n < sizeof(buf)) {
		int v = atoi(buf);
		if (v >= 1 && v <= 32) return v;
	}
	for (int i = 1; i <= 32; i++) {
		char name[64];
		snprintf(name, sizeof(name), "Local\\PSPC-net-instance-%d", i);
		HANDLE h = CreateMutexA(nullptr, TRUE, name);
		if (!h) continue;
		if (GetLastError() == ERROR_ALREADY_EXISTS) {
			CloseHandle(h);
			continue;
		}
		g_instanceMutex = h;  // held until the process ends
		snprintf(buf, sizeof(buf), "%d", i);
		SetEnvironmentVariableA("PSPC_NET_INSTANCE", buf);
		return i;
	}
	return 1;
}

}  // namespace

void Configure(const Config &config) {
	g_config = config;
	if (!g_config.enabled) {
		LOGI(LOG, "Networking off (network=0): the WLAN switch reads off");
		return;
	}
	g_instance = ClaimInstance();
	// The console's own MAC (IdStorage: random per installation, so every PC has its own, and the
	// XMB's System Information shows the same one), plus one per extra instance on this PC.
	memcpy(kLocalMac, console::Get().mac, 6);
	kLocalMac[5] = (u8)(kLocalMac[5] + (g_instance - 1));
	LOGI(LOG, "Networking on: instance %d, MAC %02x:%02x:%02x:%02x:%02x:%02x, ad hoc server %s", g_instance,
	     kLocalMac[0], kLocalMac[1], kLocalMac[2], kLocalMac[3], kLocalMac[4], kLocalMac[5], g_config.adhocServer.c_str());
}

bool Enabled() { return g_config.enabled; }
const Config &CurrentConfig() { return g_config; }
int InstanceNumber() { return g_instance; }

// ---------------------------------------------------------------- real-time hold

void HostSocketOpened() {
	if (g_hostSockets++ == 0) sched::HoldRealTime(true);
}

void HostSocketClosed() {
	if (g_hostSockets <= 0) return;
	if (--g_hostSockets == 0) sched::HoldRealTime(false);
}

// ---------------------------------------------------------------- guest waits

namespace {

struct PendingWait {
	SceUID tid;
	u64 abortKey;
};
std::map<SceUID, PendingWait> g_waits;  // wait id -> waiter (emulation thread only)
u32 g_nextWaitId = 1;

// WAITTYPE_NET ids of these waits: a range of their own.
SceUID NextWaitId() {
	g_nextWaitId = (g_nextWaitId + 1) & 0x0FFFFFFF;
	if (g_nextWaitId == 0) g_nextWaitId = 1;
	return (SceUID)(0x40000000u | g_nextWaitId);
}

// Called on the emulation thread: wake `tid` if it is still in wait `waitId`.
void WakeWaiter(SceUID tid, SceUID waitId, s32 result) {
	auto *t = kernel::Get<kernel::PspThread>(tid);
	if (t && t->IsWaitingFor(kernel::WAITTYPE_NET, waitId)) sched::Wake(t, result);
}

bool CanWait() {
	return sched::Current() != nullptr && !sched::InInterrupt() && sched::DispatchEnabled();
}

}  // namespace

WaitResult WaitHostEvents(const WSAEVENT *events, size_t count, u64 deadlineUs, u64 abortKey, u64 recheckUs) {
	if (!CanWait()) return HOSTWAIT_ABORTED;
	SceUID tid = sched::CurrentId();
	SceUID waitId = NextWaitId();
	hostnet::WatchId watch = 0;
	if (count) {
		watch = hostnet::Watch(events, count, [tid, waitId] {
			sched::PostFromHost([tid, waitId] { WakeWaiter(tid, waitId, HOSTWAIT_READY); });
		});
	}
	g_waits[waitId] = PendingWait{tid, abortKey};
	// Waiting on the host: idle time must pass in real time meanwhile (fast-time mode).
	sched::HoldRealTime(true);
	u64 now = sched::NowUs();
	u64 until = now + (recheckUs ? recheckUs : 100000);
	if (deadlineUs && deadlineUs < until) until = deadlineUs;
	s32 r = until > now ? sched::WaitUntil(kernel::WAITTYPE_NET, waitId, until, false) : 0;
	g_waits.erase(waitId);
	sched::HoldRealTime(false);
	if (watch) hostnet::Unwatch(watch);
	if (r == HOSTWAIT_READY) return HOSTWAIT_READY;
	if (r == HOSTWAIT_ABORTED || r < 0) return HOSTWAIT_ABORTED;  // aborted, or the wait was released
	return HOSTWAIT_TIMEOUT;
}

void AbortWaits(u64 abortKey) {
	std::vector<std::pair<SceUID, SceUID>> hit;
	for (auto &w : g_waits)
		if (w.second.abortKey == abortKey) hit.push_back({w.second.tid, w.first});
	for (auto &h : hit) WakeWaiter(h.first, h.second, HOSTWAIT_ABORTED);
}

bool RunBlockingOnHost(std::function<void()> work, u64 deadlineUs, u64 abortKey) {
	if (!CanWait()) {
		// Nowhere to sleep (an interrupt handler): run it here. Rare; the call stays correct.
		work();
		return true;
	}
	auto done = std::make_shared<std::atomic<bool>>(false);
	SceUID tid = sched::CurrentId();
	SceUID waitId = NextWaitId();
	std::thread([work = std::move(work), done, tid, waitId] {
		work();
		done->store(true);
		sched::PostFromHost([tid, waitId] { WakeWaiter(tid, waitId, HOSTWAIT_READY); });
	}).detach();
	g_waits[waitId] = PendingWait{tid, abortKey};
	// Waiting on the host: idle time must pass in real time meanwhile (fast-time mode).
	sched::HoldRealTime(true);
	bool ok = true;
	while (!done->load()) {
		u64 now = sched::NowUs();
		if (deadlineUs && now >= deadlineUs) {
			ok = false;
			break;
		}
		u64 until = now + 100000;
		if (deadlineUs && deadlineUs < until) until = deadlineUs;
		s32 r = sched::WaitUntil(kernel::WAITTYPE_NET, waitId, until, false);
		if (r == HOSTWAIT_ABORTED || r < 0) {
			ok = false;
			break;
		}
	}
	g_waits.erase(waitId);
	sched::HoldRealTime(false);
	return ok && done->load();
}

// ---------------------------------------------------------------- library threads

SceUID StartLibraryThread(const char *name, int priority, int stackSize, std::function<void()> body) {
	// An entry that just returns: the work happens on the thread before its entry runs.
	static u32 entry = 0;
	if (!entry || !mem::IsValidRange(entry, 8)) {
		entry = kernel::AllocKernelMemory(16, "net library thread entry");
		if (!entry) return (SceUID)SCE_KERNEL_ERROR_NO_MEMORY;
		GWrite32(entry, 0x03E00008);      // jr ra
		GWrite32(entry + 4, 0x00001021);  // move v0, zero
	}
	u32 gp = 0;
	if (kernel::PspThread *cur = sched::Current()) gp = cur->ctx.r[28];
	if (priority < 0x08 || priority > 0x77) priority = 0x30;
	if (stackSize < 0x1000) stackSize = 0x1000;  // room for the guest handlers it calls
	SceUID tid = kernel::CreateThread(name, entry, priority, stackSize, kernel::PSP_THREAD_ATTR_USER, gp, 0, false);
	if (tid < 0) return tid;
	kernel::PspThread *t = kernel::Get<kernel::PspThread>(tid);
	t->deleteOnReturn = true;
	t->onFirstRun = std::move(body);
	int r = kernel::StartThread(t, 0, 0);
	return r < 0 ? (SceUID)r : tid;
}

struct HandlerThread::State {
	std::deque<std::function<void()>> queue;
	bool stop = false;
	bool started = false;
	SceUID tid = 0;
	SceUID waitId = 0;
};

bool HandlerThread::Start(const char *name, int priority, int stackSize) {
	if (Running()) return true;
	auto st = std::make_shared<State>();
	st->waitId = NextWaitId();
	SceUID tid = StartLibraryThread(name, priority, stackSize, [st] {
		while (!st->stop) {
			if (!st->queue.empty()) {
				auto item = std::move(st->queue.front());
				st->queue.pop_front();
				item();
				continue;
			}
			if (sched::Wait(kernel::WAITTYPE_NET, st->waitId, 0, false) < 0 && !st->stop) {
				// The wait was released from outside (sceKernelReleaseWaitThread): just go round.
				continue;
			}
		}
		st->queue.clear();
	});
	if (tid < 0) {
		LOGW(LOG, "Cannot start %s thread: %08x", name, tid);
		return false;
	}
	st->tid = tid;
	st->started = true;
	st_ = st;
	return true;
}

void HandlerThread::Stop() {
	if (!st_) return;
	st_->stop = true;
	st_->queue.clear();
	WakeWaiter(st_->tid, st_->waitId, 0);
	st_.reset();
}

bool HandlerThread::Running() const {
	if (!st_ || st_->stop) return false;
	auto *t = kernel::Get<kernel::PspThread>(st_->tid);
	return t && !(t->status & (kernel::THREADSTATUS_DORMANT | kernel::THREADSTATUS_DEAD));
}

void HandlerThread::Post(std::function<void()> item) {
	if (!st_ || st_->stop) return;
	st_->queue.push_back(std::move(item));
	WakeWaiter(st_->tid, st_->waitId, 0);
}

u32 HandlerThread::CallHandler(u32 func, std::initializer_list<u32> args) {
	kernel::PspThread *t = sched::Current();
	if (!t || !mem::IsValidAddress(func)) return 0;
	return cpu::CallGuest(&t->ctx, func, args);
}

}  // namespace net
