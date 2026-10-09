// SPDX-License-Identifier: GPL-2.0-or-later
//
// ThreadManForUser alarms and VTimers. Handlers run in interrupt context, from the scheduler.
// Semantics follow PPSSPP's sceKernelAlarm.cpp and sceKernelVTimer.cpp (Portions derived from
// PPSSPP, https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later).
#include <algorithm>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/scheduler.h"

using namespace kernel;

namespace {

PspThread *CurThread() { return sched::Current(); }

// ================================================================ alarms

struct Alarm : KObject {
	static constexpr KType kType = KType::Alarm;
	KType Type() const override { return kType; }
	u32 handler = 0, common = 0, gp = 0;
	u64 schedule = 0;  // absolute time of the deadline the status reports
	u64 eventId = 0;
};

void ArmAlarm(SceUID id, u64 atUs);

void FireAlarm(SceUID id) {
	Alarm *a = Get<Alarm>(id);
	if (!a) return;
	a->eventId = 0;
	sched::QueueInterrupt([id]() {
		Alarm *al = Get<Alarm>(id);
		if (!al) return;
		// On hardware a thread that keeps running loses about 70us to an alarm handler, and one
		// the handler wakes runs about 50us after it (threads/scheduling/alarmcosts).
		sched::ChargeUs(21);
		u32 handler = al->handler, common = al->common, gp = al->gp;
		s32 next = (s32)sched::RunInterruptHandler(handler, gp, {common});
		sched::ChargeUs(44);
		al = Get<Alarm>(id);
		if (!al) return;  // the handler deleted its own alarm
		if (next > 0) {
			// Counts from the previous deadline, so a repeating alarm doesn't drift - unless that
			// has already gone by (say interrupts were suspended), when it counts from now.
			u64 now = sched::NowUs();
			u64 at = al->schedule + (u64)next > now ? al->schedule + (u64)next : now + (u64)next;
			al->schedule = at;
			ArmAlarm(id, at);
		} else {
			Destroy(id);
		}
	});
}

void ArmAlarm(SceUID id, u64 atUs) {
	Alarm *a = Get<Alarm>(id);
	if (!a) return;
	a->eventId = sched::ScheduleEvent(atUs, [id]() { FireAlarm(id); });
}

SceUID SetAlarm(u64 us, u32 handler, u32 common) {
	if (!mem::IsValidAddress(handler)) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	auto *a = new Alarm();
	a->name = "[Alarm]";
	a->handler = handler;
	a->common = common;
	PspThread *t = CurThread();
	a->gp = t ? t->ctx.r[28] : 0;
	SceUID id = Register(a);
	// The call takes about 40us, and the alarm doesn't go off sooner than about 215us after the
	// deadline is taken, however short it's asked to be. The status shows the time asked for.
	sched::ChargeUs(20);
	u64 now = sched::NowUs();
	a->schedule = now + us;
	a->eventId = sched::ScheduleEvent(now + std::clamp<u64>(us, 215, (u64)1 << 52), [id]() { FireAlarm(id); });
	sched::ChargeUs(20);
	return id;
}

SceUID sceKernelSetAlarm(u32 us, u32 handler, u32 common) { return SetAlarm(us, handler, common); }

SceUID sceKernelSetSysClockAlarm(u32 clockPtr, u32 handler, u32 common) {
	if (!mem::IsValidRange(clockPtr, 8) || (clockPtr & 3)) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	return SetAlarm(GRead<u64>(clockPtr), handler, common);
}

int sceKernelCancelAlarm(SceUID id) {
	Alarm *a = Get<Alarm>(id);
	if (!a) return (int)SCE_KERNEL_ERROR_UNKNOWN_ALMID;
	sched::CancelEvent(a->eventId);
	Destroy(id);
	return 0;
}

int sceKernelReferAlarmStatus(SceUID id, u32 infoPtr) {
	Alarm *a = Get<Alarm>(id);
	if (!a) return (int)SCE_KERNEL_ERROR_UNKNOWN_ALMID;
	if (!mem::IsValidRange(infoPtr, 20)) return -1;
	u32 size = GRead32(infoPtr);
	// Alarms actually respect the size, and write (kinda) what it can hold.
	if (size > 0) GWrite32(infoPtr, 20);
	if (size > 4) GWrite<u64>(infoPtr + 4, a->schedule);
	if (size > 12) GWrite32(infoPtr + 12, a->handler);
	if (size > 16) GWrite32(infoPtr + 16, a->common);
	return 0;
}

// ================================================================ VTimers

struct VTimer : KObject {
	static constexpr KType kType = KType::VTimer;
	KType Type() const override { return kType; }
	s32 active = 0;
	u64 base = 0, current = 0, schedule = 0;
	u32 handler = 0, common = 0, gp = 0;
	u64 eventId = 0;
};

// The VTimer whose handler is running (0 outside one; that's why 0 is treated differently).
SceUID g_runningVTimer = 0;

u64 RunningTime(const VTimer *vt) { return vt->active ? sched::NowUs() - vt->base : 0; }
u64 CurrentTime(const VTimer *vt) { return vt->current + RunningTime(vt); }

void ScheduleVTimer(VTimer *vt, u64 schedule);

void FireVTimer(SceUID id) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return;
	vt->eventId = 0;
	sched::QueueInterrupt([id]() {
		VTimer *v = Get<VTimer>(id);
		if (!v || !v->handler) return;
		// The handler gets pointers to the schedule and the current time, on its stack.
		constexpr u32 kHandlerStackSpace = 48;
		u32 argArea = sched::InterruptStackTop();
		GWrite<u64>(argArea - 16, v->schedule);
		GWrite<u64>(argArea - 8, CurrentTime(v));
		g_runningVTimer = id;
		u32 result = sched::RunInterruptHandler(v->handler, v->gp, {(u32)id, argArea - 16, argArea - 8, v->common},
		                                        kHandlerStackSpace);
		g_runningVTimer = 0;
		v = Get<VTimer>(id);
		if (!v) return;
		if (result == 0) {
			sched::CancelEvent(v->eventId);
			v->eventId = 0;
			v->handler = 0;
		} else {
			ScheduleVTimer(v, v->schedule + result);
		}
	});
}

void ScheduleVTimer(VTimer *vt, u64 schedule) {
	sched::CancelEvent(vt->eventId);
	vt->eventId = 0;
	vt->schedule = schedule;
	if (vt->active == 1 && vt->handler != 0) {
		// The "real" base is base + current, but when setting the time base is what matters.
		if (schedule < 250) schedule = 250;
		u64 now = sched::NowUs();
		s64 goal = (s64)(vt->base + schedule - vt->current);
		s64 minGoal = (s64)now + 250;
		u64 at = goal < minGoal ? now + 250 : (u64)goal;
		SceUID id = vt->uid;
		vt->eventId = sched::ScheduleEvent(at, [id]() { FireVTimer(id); });
	}
}

u32 sceKernelCreateVTimer(const char *name, u32 optPtr) {
	if (!name) return SCE_KERNEL_ERROR_ERROR;
	(void)optPtr;
	auto *vt = new VTimer();
	vt->name = TruncName(name);
	PspThread *t = CurThread();
	vt->gp = t ? t->ctx.r[28] : 0;
	return (u32)Register(vt);
}

u32 sceKernelDeleteVTimer(SceUID id) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	sched::CancelEvent(vt->eventId);
	Destroy(id);
	return 0;
}

u32 sceKernelGetVTimerBase(SceUID id, u32 basePtr) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	if (mem::IsValidRange(basePtr, 8) && (basePtr & 3) == 0) GWrite<u64>(basePtr, vt->base);
	return 0;
}

u64 sceKernelGetVTimerBaseWide(SceUID id) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return (u64)(s64)-1;
	return vt->base;
}

u32 sceKernelGetVTimerTime(SceUID id, u32 timePtr) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	u64 time = CurrentTime(vt);
	if (mem::IsValidRange(timePtr, 8) && (timePtr & 3) == 0) GWrite<u64>(timePtr, time);
	return 0;
}

u64 sceKernelGetVTimerTimeWide(SceUID id) {
	VTimer *vt = Get<VTimer>(id);
	// Strange error code, but it matches tests.
	if (!vt) return (u64)(s64)-1;
	return CurrentTime(vt);
}

u64 SetVTimerTime(VTimer *vt, u64 time) {
	u64 current = CurrentTime(vt);
	vt->current = time - RunningTime(vt);
	// Run if we're now past the schedule.
	ScheduleVTimer(vt, vt->schedule);
	return current;
}

u32 sceKernelSetVTimerTime(SceUID id, u32 timePtr) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	if (mem::IsValidRange(timePtr, 8) && (timePtr & 3) == 0) {
		u64 time = GRead<u64>(timePtr);
		GWrite<u64>(timePtr, SetVTimerTime(vt, time));
	}
	return 0;
}

u64 sceKernelSetVTimerTimeWide(SceUID id, u64 time) {
	if (sched::InInterrupt()) return (u64)(s64)-1;
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return (u64)(s64)-1;
	return SetVTimerTime(vt, time);
}

u32 sceKernelStartVTimer(SceUID id) {
	sched::ChargeCycles(12200);
	if (id == g_runningVTimer) return SCE_KERNEL_ERROR_ILLEGAL_VTID;
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	if (vt->active) return 1;
	vt->active = 1;
	vt->base = sched::NowUs();
	if (vt->handler) ScheduleVTimer(vt, vt->schedule);
	return 0;
}

u32 sceKernelStopVTimer(SceUID id) {
	if (id == g_runningVTimer) return SCE_KERNEL_ERROR_ILLEGAL_VTID;
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	if (vt->active == 0) return 0;
	vt->current = CurrentTime(vt);
	vt->active = 0;
	vt->base = 0;
	return 1;
}

u32 SetVTimerHandler(SceUID id, u64 schedule, u32 handler, u32 common) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	vt->handler = handler;
	if (handler) {
		vt->common = common;
		PspThread *t = CurThread();
		if (t) vt->gp = t->ctx.r[28];
		ScheduleVTimer(vt, schedule);
	} else {
		ScheduleVTimer(vt, vt->schedule);
	}
	return 0;
}

u32 sceKernelSetVTimerHandler(SceUID id, u32 schedulePtr, u32 handler, u32 common) {
	sched::ChargeCycles(900);
	if (id == g_runningVTimer) return SCE_KERNEL_ERROR_ILLEGAL_VTID;
	if (!Get<VTimer>(id)) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	sched::ChargeCycles(2000);
	u64 schedule = mem::IsValidRange(schedulePtr, 8) ? GRead<u64>(schedulePtr) : 0;
	return SetVTimerHandler(id, schedule, handler, common);
}

u32 sceKernelSetVTimerHandlerWide(SceUID id, u64 schedule, u32 handler, u32 common) {
	sched::ChargeCycles(900);
	if (id == g_runningVTimer) return SCE_KERNEL_ERROR_ILLEGAL_VTID;
	return SetVTimerHandler(id, schedule, handler, common);
}

u32 sceKernelCancelVTimerHandler(SceUID id) {
	if (id == g_runningVTimer) return SCE_KERNEL_ERROR_ILLEGAL_VTID;
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
	sched::CancelEvent(vt->eventId);
	vt->eventId = 0;
	vt->handler = 0;
	return 0;
}

u32 sceKernelReferVTimerStatus(SceUID id, u32 statusPtr) {
	VTimer *vt = Get<VTimer>(id);
	if (!vt) return SCE_KERNEL_ERROR_UNKNOWN_VTID;
#pragma pack(push, 4)
	struct {
		u32 size;
		char name[32];
		s32 active;
		u64 base, current, schedule;
		u32 handler, common;
	} st{};
#pragma pack(pop)
	static_assert(sizeof(st) == 72, "VTimer status size");
	if (mem::IsValidRange(statusPtr, 4) && (statusPtr & 3) == 0) {
		st.size = sizeof(st);
		CopyName(st.name, vt->name);
		st.active = vt->active;
		st.base = vt->base;
		st.current = CurrentTime(vt);
		st.schedule = vt->schedule;
		st.handler = vt->handler;
		st.common = vt->common;
		u32 size = std::min<u32>(GRead32(statusPtr), sizeof(st));
		if (size && mem::IsValidRange(statusPtr, size)) mem::WriteBytes(statusPtr, &st, size);
	}
	return 0;
}

void _sceKernelReturnFromTimerHandler() {}

}  // namespace

void kernel::RegisterThreadManTimers() {
	constexpr u32 NI = KF_NOT_IN_INTERRUPT;
	static const hle::HleFunction funcs[] = {
		HLEFN(0x6652B8CA, sceKernelSetAlarm),
		HLEFN(0xB2C25152, sceKernelSetSysClockAlarm),
		HLEFN(0x7E65B999, sceKernelCancelAlarm),
		HLEFN(0xDAA3F564, sceKernelReferAlarmStatus),

		KFN(0x20FFF560, sceKernelCreateVTimer, NI),
		KFN(0x328F9E52, sceKernelDeleteVTimer, NI),
		HLEFN(0xC68D9437, sceKernelStartVTimer),
		HLEFN(0xD0AEEE87, sceKernelStopVTimer),
		HLEFN(0xD2D615EF, sceKernelCancelVTimerHandler),
		HLEFN(0xB3A59970, sceKernelGetVTimerBase),
		HLEFN(0xB7C18B77, sceKernelGetVTimerBaseWide),
		HLEFN(0x034A921F, sceKernelGetVTimerTime),
		HLEFN(0xC0B3FFD2, sceKernelGetVTimerTimeWide),
		HLEFN(0x5F32BEAA, sceKernelReferVTimerStatus),
		HLEFN(0x542AD630, sceKernelSetVTimerTime),
		HLEFN(0xFB6425C3, sceKernelSetVTimerTimeWide),
		HLEFN(0xD8B299AE, sceKernelSetVTimerHandler),
		HLEFN(0x53B00E9A, sceKernelSetVTimerHandlerWide),
		HLEFN(0x0E927AED, _sceKernelReturnFromTimerHandler),
	};
	hle::RegisterModule("ThreadManForUser", funcs);
	hle::RegisterModule("ThreadManForKernel", funcs);
}
