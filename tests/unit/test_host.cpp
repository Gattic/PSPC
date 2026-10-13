// SPDX-License-Identifier: GPL-2.0-or-later
#include <cfenv>
#include <thread>

#include "common/host.h"
#include "net/socket.h"
#include "testing.h"

namespace {
struct FiberProbe {
	void *caller;
	void *worker = nullptr;
	int stage = 0;
	bool identity = false;
	bool rounding = false;
};

void ProbeFiber(void *arg) {
	auto &probe = *static_cast<FiberProbe *>(arg);
	probe.identity = hostsys::CurrentFiber() == probe.worker;
	int stackValue = 42;
	std::fesetround(FE_UPWARD);
	probe.stage = 1;
	hostsys::SwitchFiber(probe.caller);
	probe.rounding = std::fegetround() == FE_UPWARD;
	probe.stage = stackValue;
	hostsys::SwitchFiber(probe.caller);
}
}

TEST(host_fiber_stack_and_float_environment) {
	const int originalRound = std::fegetround();
	void *caller = hostsys::EnterFibers();
	CHECK(caller != nullptr);
	if (!caller) return;
	std::fesetround(FE_DOWNWARD);
	FiberProbe probe{caller};
	probe.worker = hostsys::NewFiber(256 * 1024, ProbeFiber, &probe);
	CHECK(probe.worker != nullptr);
	if (probe.worker) {
		hostsys::SwitchFiber(probe.worker);
		CHECK(probe.identity);
		CHECK_EQ(probe.stage, 1);
		CHECK_EQ(std::fegetround(), FE_DOWNWARD);
		CHECK(hostsys::CurrentFiber() == caller);
		hostsys::SwitchFiber(probe.worker);
		CHECK_EQ(probe.stage, 42);
		CHECK(probe.rounding);
		CHECK_EQ(std::fegetround(), FE_DOWNWARD);
		hostsys::FreeFiber(probe.worker);
	}
	hostsys::LeaveFibers();
	std::fesetround(originalRound);
}

TEST(host_network_event_reset_and_cross_thread_wake) {
	auto automatic = hostnet::NewEvent();
	auto manual = hostnet::NewEvent(true);
	CHECK(automatic != nullptr && manual != nullptr);
	if (!automatic || !manual) {
		hostnet::CloseEvent(automatic);
		hostnet::CloseEvent(manual);
		return;
	}
	hostnet::EventHandle events[] = {automatic, manual};
	CHECK_EQ(hostnet::WaitEvents(events, 2, 0), -1);
	std::thread sender([&] { hostnet::SignalEvent(manual); });
	CHECK_EQ(hostnet::WaitEvents(events, 2, 2000), 1);
	sender.join();
	CHECK_EQ(hostnet::WaitEvents(&manual, 1, 0), 0);
	hostnet::ResetEvent(manual);
	CHECK_EQ(hostnet::WaitEvents(events, 2, 0), -1);
	hostnet::SignalEvent(automatic);
	CHECK_EQ(hostnet::WaitEvents(events, 2, 0), 0);
	CHECK_EQ(hostnet::WaitEvents(events, 2, 0), -1);
	hostnet::CloseEvent(automatic);
	hostnet::CloseEvent(manual);
}
