// SPDX-License-Identifier: GPL-2.0-or-later
//
// ThreadManForUser message boxes. Messages are guest structures linked in a circular list
// through their first word, as on hardware (tests poke at the links). Semantics follow PPSSPP's
// sceKernelMbx.cpp (Portions derived from PPSSPP, https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include <algorithm>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace {

constexpr u32 SCE_KERNEL_MBA_THPRI = 0x100;
constexpr u32 SCE_KERNEL_MBA_MSPRI = 0x400;
constexpr u32 SCE_KERNEL_MBA_ATTR_KNOWN = SCE_KERNEL_MBA_THPRI | SCE_KERNEL_MBA_MSPRI;

bool Valid4(u32 addr) { return (addr & 3) == 0 && mem::IsValidRange(addr, 4); }

struct Mbx : KObject {
	static constexpr KType kType = KType::Mbx;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 numMessages = 0;
	u32 packetListHead = 0;
	WaitQueue waiters;  // waitValue[0] = where to store the received packet address

	void AddWaiter(PspThread *t) {
		if (attr & SCE_KERNEL_MBA_THPRI) {
			for (auto it = waiters.threads.begin(); it != waiters.threads.end(); ++it) {
				PspThread *o = Get<PspThread>(*it);
				if (o && t->currentPriority < o->currentPriority) {
					waiters.threads.insert(it, t->uid);
					return;
				}
			}
		}
		waiters.threads.push_back(t->uid);
	}

	void AddInitialMessage(u32 ptr) {
		numMessages++;
		GWrite32(ptr, ptr);
		packetListHead = ptr;
	}
	void AddFirstMessage(u32 endPtr, u32 ptr) {
		numMessages++;
		GWrite32(ptr, packetListHead);
		GWrite32(endPtr, ptr);
		packetListHead = ptr;
	}
	void AddLastMessage(u32 endPtr, u32 ptr) {
		numMessages++;
		GWrite32(endPtr, ptr);
		GWrite32(ptr, packetListHead);
	}
	void AddMessage(u32 beforePtr, u32 afterPtr, u32 ptr) {
		numMessages++;
		GWrite32(ptr, afterPtr);
		GWrite32(beforePtr, ptr);
	}

	// receivePtr must be valid.
	int ReceiveMessage(u32 receivePtr) {
		u32 ptr = packetListHead;
		if (!Valid4(packetListHead)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
		// Walk the list and reset the head.
		int c = 0;
		for (;;) {
			u32 next = GRead32(packetListHead);
			if (!Valid4(next)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
			if (next == ptr) {
				if (packetListHead != ptr) {
					next = GRead32(next);
					GWrite32(packetListHead, next);
					packetListHead = next;
					break;
				}
				if (c < numMessages - 1) return (int)SCE_KERNEL_ERROR_MESSAGEBOX_DUPLICATE_MESSAGE;
				packetListHead = 0;
				break;
			}
			packetListHead = next;
			c++;
			// A corrupt list may loop without coming back to ptr.
			if (c > numMessages) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
		}
		GWrite32(receivePtr, ptr);
		numMessages--;
		return 0;
	}
};

SceUID sceKernelCreateMbx(const char *name, u32 attr, u32 optAddr) {
	if (!name) return (SceUID)SCE_KERNEL_ERROR_ERROR;
	// Accepts 0x000 - 0x0FF, 0x100 - 0x1FF, and 0x400 - 0x4FF.
	if (((attr & ~SCE_KERNEL_MBA_ATTR_KNOWN) & ~0xFF) != 0) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	(void)optAddr;
	auto *m = new Mbx();
	m->name = TruncName(name);
	m->attr = attr;
	return Register(m);
}

int sceKernelDeleteMbx(SceUID id) {
	Mbx *m = Get<Mbx>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MBXID;
	bool woke = m->waiters.WakeAll(WAITTYPE_MBX, id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(id);
	if (woke) sched::Reschedule();
	return 0;
}

int sceKernelSendMbx(SceUID id, u32 packetAddr) {
	Mbx *m = Get<Mbx>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MBXID;
	if (!mem::IsValidRange(packetAddr, 8)) return -1;
	// If the queue is empty, maybe someone is waiting (they might have timed out).
	if (m->numMessages == 0) {
		m->waiters.Cleanup(WAITTYPE_MBX, id);
		if (!m->waiters.threads.empty()) {
			size_t idx = 0;
			if (m->attr & SCE_KERNEL_MBA_THPRI) {
				int best = m->waiters.BestPriorityIndex();
				idx = best < 0 ? 0 : (size_t)best;
			}
			PspThread *t = Get<PspThread>(m->waiters.threads[idx]);
			m->waiters.threads.erase(m->waiters.threads.begin() + idx);
			if (Valid4(t->waitValue[0])) GWrite32(t->waitValue[0], packetAddr);
			sched::Wake(t, 0);
			sched::Reschedule();
			return 0;
		}
	}
	if (m->numMessages == 0) {
		m->AddInitialMessage(packetAddr);
		return 0;
	}
	u32 next = m->packetListHead, prev = 0;
	for (int i = 0, n = m->numMessages; i < n; i++) {
		if (next == packetAddr) return (int)SCE_KERNEL_ERROR_MESSAGEBOX_DUPLICATE_MESSAGE;
		if (!Valid4(next)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
		prev = next;
		next = GRead32(next);
	}
	if (m->attr & SCE_KERNEL_MBA_MSPRI) {
		u8 prio = GRead8(packetAddr + 4);
		for (int i = 0, n = m->numMessages; i < n; i++) {
			if (!mem::IsValidRange(next, 8)) break;
			if (prio < GRead8(next + 4)) {
				if (i == 0) m->AddFirstMessage(prev, packetAddr);
				else m->AddMessage(prev, next, packetAddr);
				return 0;
			}
			prev = next;
			next = GRead32(next);
		}
	}
	m->AddLastMessage(prev, packetAddr);
	return 0;
}

int ReceiveMbx(SceUID id, u32 packetAddrPtr, u32 timeoutPtr, bool cb) {
	Mbx *m = Get<Mbx>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MBXID;
	PspThread *t = sched::Current();
	if (m->numMessages > 0) {
		int r = m->ReceiveMessage(packetAddrPtr);
		if (cb) RunPendingCallbacks(t);
		return r;
	}
	m->waiters.Remove(t->uid);
	if (sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	t->waitValue[0] = packetAddrPtr;
	m->AddWaiter(t);
	SceUID me = t->uid;
	WaitHooks hooks;
	hooks.pause = [id, me]() {
		if (Mbx *mb = Get<Mbx>(id)) mb->waiters.Remove(me);
	};
	hooks.resume = [id, me]() {
		PspThread *w = Get<PspThread>(me);
		Mbx *mb = Get<Mbx>(id);
		if (!mb) {
			if (w->waitTimeoutPtr && mem::IsValidRange(w->waitTimeoutPtr, 4)) GWrite32(w->waitTimeoutPtr, 0);
			w->waitTimeoutPtr = 0;
			sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		if (mb->numMessages > 0) {
			sched::Wake(w, 0);
			mb->ReceiveMessage(w->waitValue[0]);
			return true;
		}
		mb->waiters.threads.push_back(me);
		return false;
	};
	hooks.timeout = [id, me]() {
		if (Mbx *mb = Get<Mbx>(id)) mb->waiters.Remove(me);
	};
	return sched::Wait(WAITTYPE_MBX, id, timeoutPtr, cb, &hooks);
}

int sceKernelReceiveMbx(SceUID id, u32 packetAddrPtr, u32 timeoutPtr) {
	return ReceiveMbx(id, packetAddrPtr, timeoutPtr, false);
}
int sceKernelReceiveMbxCB(SceUID id, u32 packetAddrPtr, u32 timeoutPtr) {
	return ReceiveMbx(id, packetAddrPtr, timeoutPtr, true);
}

int sceKernelPollMbx(SceUID id, u32 packetAddrPtr) {
	Mbx *m = Get<Mbx>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MBXID;
	if (m->numMessages > 0) return m->ReceiveMessage(packetAddrPtr);
	return (int)SCE_KERNEL_ERROR_MBOX_NOMSG;
}

int sceKernelCancelReceiveMbx(SceUID id, u32 numWaitingPtr) {
	Mbx *m = Get<Mbx>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MBXID;
	m->waiters.Cleanup(WAITTYPE_MBX, id);
	u32 count = (u32)m->waiters.threads.size();
	bool woke = m->waiters.WakeAll(WAITTYPE_MBX, id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	if (woke) sched::Reschedule();
	if (numWaitingPtr && mem::IsValidRange(numWaitingPtr, 4)) GWrite32(numWaitingPtr, count);
	return 0;
}

int sceKernelReferMbxStatus(SceUID id, u32 infoAddr) {
	Mbx *m = Get<Mbx>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MBXID;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 numWaitThreads;
		s32 numMessages;
		u32 packetListHead;
	} info{};
	if (!mem::IsValidRange(infoAddr, sizeof(info))) return -1;
	// The firmware walks the list and fixes up the head, which tests that corrupt the links see.
	u32 head = m->packetListHead;
	u32 packet = head;
	for (int i = 0, n = m->numMessages; i < n; ++i) {
		if (packet == 0 || !mem::IsValidAddress(packet)) {
			m->packetListHead = 0;
			packet = 0;
			break;
		}
		u32 next = GRead32(packet);
		if (next == 0) {
			m->packetListHead = 0;
			packet = 0;
			break;
		}
		if (next == packet) {
			if (n > 1) m->packetListHead = packet;
			break;
		}
		packet = next;
	}
	if (packet != 0 && packet != head) {
		for (int i = 0, n = m->numMessages; i < n; ++i) {
			if (packet == 0 || !mem::IsValidAddress(packet)) break;
			u32 next = GRead32(packet);
			if (next == head) {
				m->packetListHead = packet;
				break;
			}
			if (next == 0 || next == packet) break;
			packet = next;
		}
	}
	if (GRead32(infoAddr) != 0) {
		info.size = sizeof(info);
		CopyName(info.name, m->name);
		info.attr = m->attr;
		info.numWaitThreads = m->waiters.Count(WAITTYPE_MBX, id);
		info.numMessages = m->numMessages;
		info.packetListHead = m->packetListHead;
		mem::WriteBytes(infoAddr, &info, sizeof(info));
	}
	return 0;
}

}  // namespace

void kernel::RegisterThreadManMbx() {
	constexpr u32 NID = KF_NOT_IN_INTERRUPT | KF_NOT_DISPATCH_SUSPENDED;
	static const hle::HleFunction funcs[] = {
		HLEFN(0x8125221D, sceKernelCreateMbx),
		HLEFN(0x86255ADA, sceKernelDeleteMbx),
		HLEFN(0xE9B3061E, sceKernelSendMbx),
		KFN(0x18260574, sceKernelReceiveMbx, NID),
		KFN(0xF3986382, sceKernelReceiveMbxCB, NID),
		HLEFN(0x0D81716A, sceKernelPollMbx),
		HLEFN(0x87D4DD36, sceKernelCancelReceiveMbx),
		HLEFN(0xA8E8C846, sceKernelReferMbxStatus),
	};
	hle::RegisterModule("ThreadManForUser", funcs);
	hle::RegisterModule("ThreadManForKernel", funcs);
}
