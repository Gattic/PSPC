// SPDX-License-Identifier: GPL-2.0-or-later
//
// ThreadManForUser message pipes. Semantics (partial transfers, ASAP/FULL modes, the zero-size
// pipe that hands data straight between threads, wake order) follow PPSSPP's
// sceKernelMsgPipe.cpp (Portions derived from PPSSPP, https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include <algorithm>
#include <cstring>
#include <map>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace {

constexpr u32 SCE_KERNEL_MPA_THPRI_S = 0x0100;
constexpr u32 SCE_KERNEL_MPA_THPRI_R = 0x1000;
constexpr u32 SCE_KERNEL_MPA_HIGHMEM = 0x4000;
constexpr u32 SCE_KERNEL_MPA_KNOWN = SCE_KERNEL_MPA_THPRI_S | SCE_KERNEL_MPA_THPRI_R | SCE_KERNEL_MPA_HIGHMEM;
constexpr int SCE_KERNEL_MPW_FULL = 0;
constexpr int SCE_KERNEL_MPW_ASAP = 1;
constexpr u32 WAIT_SEND = 0, WAIT_RECV = 1;

bool Valid4(u32 addr) { return (addr & 3) == 0 && mem::IsValidRange(addr, 4); }

struct Waiter {
	SceUID threadId;
	u32 bufAddr;
	u32 bufSize;
	// Free space at the end for receive; bytes still to be read for send.
	u32 freeSize;
	int waitMode;
	u32 transferredAddr;

	bool StillWaiting(SceUID pipe) const {
		return WaitQueue::IsWaitingOn(Get<PspThread>(threadId), WAITTYPE_MSGPIPE, pipe);
	}
	void Complete(SceUID pipe, int result) const {
		if (StillWaiting(pipe)) sched::Wake(Get<PspThread>(threadId), result);
	}
	// A sender's data goes out.
	void ReadBuffer(u32 destPtr, u32 len) {
		mem::Memcpy(destPtr, bufAddr + bufSize - freeSize, len);
		freeSize -= len;
		if (Valid4(transferredAddr)) GWrite32(transferredAddr, GRead32(transferredAddr) + len);
	}
	// A receiver's buffer fills.
	void WriteBuffer(u32 srcPtr, u32 len) {
		mem::Memcpy(bufAddr + (bufSize - freeSize), srcPtr, len);
		freeSize -= len;
		if (Valid4(transferredAddr)) GWrite32(transferredAddr, GRead32(transferredAddr) + len);
	}
};

struct MsgPipe : KObject {
	static constexpr KType kType = KType::MsgPipe;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 bufSize = 0, freeSize = 0;
	u32 buffer = 0;
	std::vector<Waiter> sendWaiters, recvWaiters;
	std::map<SceUID, Waiter> pausedSend, pausedRecv;

	~MsgPipe() override {
		if (buffer) {
			if (BlockAllocator *a = AllocatorForAddress(buffer)) a->Free(buffer);
		}
	}

	u32 UsedSize() const { return (u32)(bufSize - freeSize); }

	void AddWaiter(std::vector<Waiter> &list, SceUID id, u32 addr, u32 size, int mode, u32 transferred) {
		Waiter w{id, addr, size, size, mode, transferred};
		// Starts out with 0 bytes transferred while waiting.
		if (Valid4(transferred)) GWrite32(transferred, 0);
		list.push_back(w);
	}

	void SortThreads(std::vector<Waiter> &list, bool usePrio) {
		list.erase(std::remove_if(list.begin(), list.end(), [&](const Waiter &w) { return !w.StillWaiting(uid); }),
		           list.end());
		if (usePrio) {
			std::stable_sort(list.begin(), list.end(), [](const Waiter &a, const Waiter &b) {
				PspThread *ta = Get<PspThread>(a.threadId), *tb = Get<PspThread>(b.threadId);
				return (ta ? ta->currentPriority : 0x7FFF) < (tb ? tb->currentPriority : 0x7FFF);
			});
		}
	}
	void SortSend() { SortThreads(sendWaiters, (attr & SCE_KERNEL_MPA_THPRI_S) != 0); }
	void SortRecv() { SortThreads(recvWaiters, (attr & SCE_KERNEL_MPA_THPRI_R) != 0); }

	bool CheckReceiveThreads();

	bool CheckSendThreads() {
		SortSend();
		bool woke = false, filled = false;
		while (!sendWaiters.empty() && freeSize > 0) {
			Waiter &w = sendWaiters.front();
			u32 n = std::min(w.freeSize, (u32)freeSize);
			w.ReadBuffer(buffer + UsedSize(), n);
			freeSize -= n;
			filled = true;
			if (w.waitMode == SCE_KERNEL_MPW_ASAP || w.freeSize == 0) {
				w.Complete(uid, 0);
				sendWaiters.erase(sendWaiters.begin());
				woke = true;
			} else {
				// Unlike receives, no partial sends: stop at the first blocked thread.
				break;
			}
		}
		if (filled) woke |= CheckReceiveThreads();
		return woke;
	}
};

// Only run when the buffer isn't 0 bytes (otherwise data goes straight between threads).
bool MsgPipe::CheckReceiveThreads() {
	SortRecv();
	bool woke = false, freed = false;
	while (!recvWaiters.empty() && UsedSize() > 0) {
		Waiter &w = recvWaiters.front();
		// Receive as much as possible, even if it's not enough to wake up.
		u32 n = std::min(w.freeSize, UsedSize());
		w.WriteBuffer(buffer, n);
		freeSize += n;
		// Put the unused data at the start of the buffer.
		if (UsedSize()) memmove(GPtr(buffer), GPtr(buffer) + n, UsedSize());
		freed = true;
		if (w.waitMode == SCE_KERNEL_MPW_ASAP || w.freeSize == 0) {
			w.Complete(uid, 0);
			recvWaiters.erase(recvWaiters.begin());
			woke = true;
		} else {
			break;
		}
	}
	if (freed) woke |= CheckSendThreads();
	return woke;
}

int DoSend(MsgPipe *m, u32 sendBufAddr, u32 sendSize, int waitMode, u32 resultAddr, bool poll, bool &needsResched,
           bool &needsWait) {
	u32 cur = sendBufAddr;
	SceUID id = m->uid;
	SceUID me = sched::CurrentId();
	if (m->bufSize == 0) {
		// No buffer: everything goes straight to waiting receivers.
		m->SortRecv();
		while (!m->recvWaiters.empty() && sendSize != 0) {
			Waiter &w = m->recvWaiters.front();
			u32 n = std::min(w.freeSize, sendSize);
			if (n > 0) {
				w.WriteBuffer(cur, n);
				sendSize -= n;
				cur += n;
				if (w.freeSize == 0 || w.waitMode == SCE_KERNEL_MPW_ASAP) {
					w.Complete(id, 0);
					m->recvWaiters.erase(m->recvWaiters.begin());
					needsResched = true;
				}
			} else {
				break;
			}
		}
		if (sendSize != 0 && (waitMode != SCE_KERNEL_MPW_ASAP || cur == sendBufAddr)) {
			if (poll) {
				// Generally the result isn't updated, but it is for a 0 size buffer in ASAP mode.
				if (Valid4(resultAddr) && waitMode == SCE_KERNEL_MPW_ASAP) GWrite32(resultAddr, cur - sendBufAddr);
				return (int)SCE_KERNEL_ERROR_MPP_FULL;
			}
			m->AddWaiter(m->sendWaiters, me, cur, sendSize, waitMode, resultAddr);
			needsWait = true;
			return 0;
		}
	} else {
		if (sendSize > (u32)m->bufSize) return (int)SCE_KERNEL_ERROR_ILLEGAL_SIZE;
		u32 n = 0;
		// If others are already waiting, space or not, this one gets in line.
		m->SortSend();
		if (m->sendWaiters.empty()) {
			if (sendSize <= (u32)m->freeSize) n = sendSize;
			else if (waitMode == SCE_KERNEL_MPW_ASAP) n = (u32)m->freeSize;
		}
		if (n != 0) {
			mem::Memcpy(m->buffer + (m->bufSize - m->freeSize), sendBufAddr, n);
			m->freeSize -= n;
			cur += n;
			sendSize -= n;
			if (m->CheckReceiveThreads()) needsResched = true;
		} else if (sendSize != 0) {
			if (poll) return (int)SCE_KERNEL_ERROR_MPP_FULL;
			m->AddWaiter(m->sendWaiters, me, cur, sendSize, waitMode, resultAddr);
			needsWait = true;
			return 0;
		}
	}
	// It didn't wait, so the number of bytes transferred is known now.
	if (Valid4(resultAddr)) GWrite32(resultAddr, cur - sendBufAddr);
	return 0;
}

int DoReceive(MsgPipe *m, u32 recvBufAddr, u32 recvSize, int waitMode, u32 resultAddr, bool poll, bool &needsResched,
              bool &needsWait) {
	u32 cur = recvBufAddr;
	SceUID id = m->uid;
	SceUID me = sched::CurrentId();
	if (m->bufSize == 0) {
		// No buffer: take data straight from waiting senders.
		m->SortSend();
		while (!m->sendWaiters.empty() && recvSize != 0) {
			Waiter &w = m->sendWaiters.front();
			u32 n = std::min(w.freeSize, recvSize);
			if (n > 0) {
				w.ReadBuffer(cur, n);
				recvSize -= n;
				cur += n;
				if (w.freeSize == 0 || w.waitMode == SCE_KERNEL_MPW_ASAP) {
					w.Complete(id, 0);
					m->sendWaiters.erase(m->sendWaiters.begin());
					needsResched = true;
				}
			} else {
				break;
			}
		}
		if (recvSize != 0 && (waitMode != SCE_KERNEL_MPW_ASAP || cur == recvBufAddr)) {
			if (poll) {
				if (Valid4(resultAddr) && waitMode == SCE_KERNEL_MPW_ASAP) GWrite32(resultAddr, cur - recvBufAddr);
				return (int)SCE_KERNEL_ERROR_MPP_EMPTY;
			}
			m->AddWaiter(m->recvWaiters, me, cur, recvSize, waitMode, resultAddr);
			needsWait = true;
			return 0;
		}
	} else {
		if (recvSize > (u32)m->bufSize) return (int)SCE_KERNEL_ERROR_ILLEGAL_SIZE;
		while (m->UsedSize() > 0) {
			u32 n = std::min(recvSize, m->UsedSize());
			if (n == 0) break;
			mem::Memcpy(cur, m->buffer, n);
			m->freeSize += n;
			if (m->UsedSize()) memmove(GPtr(m->buffer), GPtr(m->buffer) + n, m->UsedSize());
			cur += n;
			recvSize -= n;
			m->CheckSendThreads();
		}
		if (recvSize != 0 && (waitMode != SCE_KERNEL_MPW_ASAP || cur == recvBufAddr)) {
			if (poll) return (int)SCE_KERNEL_ERROR_MPP_EMPTY;
			m->AddWaiter(m->recvWaiters, me, cur, recvSize, waitMode, resultAddr);
			needsWait = true;
			return 0;
		}
	}
	if (Valid4(resultAddr)) GWrite32(resultAddr, cur - recvBufAddr);
	return 0;
}

std::vector<Waiter> &ListFor(MsgPipe *m, u32 dir) { return dir == WAIT_SEND ? m->sendWaiters : m->recvWaiters; }
std::map<SceUID, Waiter> &PausedFor(MsgPipe *m, u32 dir) { return dir == WAIT_SEND ? m->pausedSend : m->pausedRecv; }

WaitHooks PipeHooks(SceUID id, SceUID me, u32 dir) {
	WaitHooks hooks;
	hooks.pause = [id, me, dir]() {
		MsgPipe *m = Get<MsgPipe>(id);
		if (!m) return;
		auto &list = ListFor(m, dir);
		for (auto it = list.begin(); it != list.end(); ++it) {
			if (it->threadId == me) {
				PausedFor(m, dir)[me] = *it;
				list.erase(it);
				break;
			}
		}
	};
	hooks.resume = [id, me, dir]() {
		PspThread *w = Get<PspThread>(me);
		MsgPipe *m = Get<MsgPipe>(id);
		if (!m) {
			if (w->waitTimeoutPtr && mem::IsValidRange(w->waitTimeoutPtr, 4)) GWrite32(w->waitTimeoutPtr, 0);
			w->waitTimeoutPtr = 0;
			sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		auto &paused = PausedFor(m, dir);
		auto pit = paused.find(me);
		if (pit == paused.end()) {
			sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		Waiter info = pit->second;
		paused.erase(pit);
		// Try again; this queues it again if it still has to wait. The caller is the waiting
		// thread itself (it runs this after its callbacks), so DoSend/DoReceive see it as current.
		bool needsResched = false, needsWait = false;
		int result = dir == WAIT_SEND
		                 ? DoSend(m, info.bufAddr, info.bufSize, info.waitMode, info.transferredAddr, false, needsResched, needsWait)
		                 : DoReceive(m, info.bufAddr, info.bufSize, info.waitMode, info.transferredAddr, false, needsResched,
		                             needsWait);
		if (needsWait) return false;
		sched::Wake(w, result);
		return true;
	};
	hooks.timeout = [id, me, dir]() {
		if (MsgPipe *m = Get<MsgPipe>(id)) {
			auto &list = ListFor(m, dir);
			list.erase(std::remove_if(list.begin(), list.end(), [&](const Waiter &x) { return x.threadId == me; }),
			           list.end());
		}
	};
	return hooks;
}

int ValidateTransfer(u32 bufAddr, u32 size, int waitMode, bool tryMode) {
	if (size & 0x80000000) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (size != 0 && !mem::IsValidAddress(bufAddr)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (waitMode != SCE_KERNEL_MPW_ASAP && waitMode != SCE_KERNEL_MPW_FULL) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	if (!tryMode) {
		if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
		if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	}
	return 0;
}

int Transfer(u32 dir, SceUID id, u32 bufAddr, u32 size, int waitMode, u32 resultAddr, u32 timeoutPtr, bool cb, bool poll) {
	int error = ValidateTransfer(bufAddr, size, waitMode, poll);
	if (error) return error;
	MsgPipe *m = Get<MsgPipe>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MPPID;
	PspThread *t = sched::Current();
	if (dir == WAIT_SEND) sched::ChargeCycles(2400);
	bool needsResched = false, needsWait = false;
	int result = dir == WAIT_SEND ? DoSend(m, bufAddr, size, waitMode, resultAddr, poll, needsResched, needsWait)
	                              : DoReceive(m, bufAddr, size, waitMode, resultAddr, poll, needsResched, needsWait);
	if (needsWait) {
		// Always at once up to 2us, never from 3us (threads/msgpipe/send and receive).
		if (timeoutPtr && sched::WaitTimesOutAtOnce(timeoutPtr, 210, 55)) {
			auto &list = ListFor(m, dir);
			list.erase(std::remove_if(list.begin(), list.end(), [&](const Waiter &x) { return x.threadId == t->uid; }),
			           list.end());
			if (needsResched) sched::Reschedule();
			return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
		}
		t->waitValue[0] = dir;
		WaitHooks hooks = PipeHooks(id, t->uid, dir);
		return sched::Wait(WAITTYPE_MSGPIPE, id, timeoutPtr, cb, &hooks);
	}
	if (cb && t) RunPendingCallbacks(t);
	if (needsResched) sched::Reschedule();
	return result;
}

int sceKernelCreateMsgPipe(const char *name, int partition, u32 attr, u32 size, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	if (partition < 1 || partition > 6) return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	BlockAllocator *alloc = AllocatorForPartitionId(partition, CallerIsKernelMode());
	if (!alloc) return (int)SCE_KERNEL_ERROR_ILLEGAL_PERM;
	if ((attr & ~SCE_KERNEL_MPA_KNOWN) >= 0x100) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	u32 block = 0;
	if (size != 0) {
		block = alloc->Alloc(size, (attr & SCE_KERNEL_MPA_HIGHMEM) != 0, ("MsgPipe/" + TruncName(name)).c_str());
		if (!block) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	}
	(void)optPtr;
	auto *m = new MsgPipe();
	m->name = TruncName(name);
	m->attr = attr;
	m->bufSize = (s32)size;
	m->freeSize = (s32)size;
	m->buffer = block;
	return Register(m);
}

int sceKernelDeleteMsgPipe(SceUID id) {
	sched::ChargeCycles(900);
	MsgPipe *m = Get<MsgPipe>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MPPID;
	sched::ChargeCycles(3100);
	if (!m->sendWaiters.empty() || !m->recvWaiters.empty()) sched::ChargeCycles(4000);
	for (auto &w : m->sendWaiters) w.Complete(id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	for (auto &w : m->recvWaiters) w.Complete(id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(id);
	return 0;
}

int sceKernelSendMsgPipe(SceUID id, u32 buf, u32 size, u32 mode, u32 resultAddr, u32 timeoutPtr) {
	return Transfer(WAIT_SEND, id, buf, size, (int)mode, resultAddr, timeoutPtr, false, false);
}
int sceKernelSendMsgPipeCB(SceUID id, u32 buf, u32 size, u32 mode, u32 resultAddr, u32 timeoutPtr) {
	return Transfer(WAIT_SEND, id, buf, size, (int)mode, resultAddr, timeoutPtr, true, false);
}
int sceKernelTrySendMsgPipe(SceUID id, u32 buf, u32 size, u32 mode, u32 resultAddr) {
	return Transfer(WAIT_SEND, id, buf, size, (int)mode, resultAddr, 0, false, true);
}
int sceKernelReceiveMsgPipe(SceUID id, u32 buf, u32 size, u32 mode, u32 resultAddr, u32 timeoutPtr) {
	return Transfer(WAIT_RECV, id, buf, size, (int)mode, resultAddr, timeoutPtr, false, false);
}
int sceKernelReceiveMsgPipeCB(SceUID id, u32 buf, u32 size, u32 mode, u32 resultAddr, u32 timeoutPtr) {
	return Transfer(WAIT_RECV, id, buf, size, (int)mode, resultAddr, timeoutPtr, true, false);
}
int sceKernelTryReceiveMsgPipe(SceUID id, u32 buf, u32 size, u32 mode, u32 resultAddr) {
	return Transfer(WAIT_RECV, id, buf, size, (int)mode, resultAddr, 0, false, true);
}

int sceKernelCancelMsgPipe(SceUID id, u32 numSendPtr, u32 numRecvPtr) {
	sched::ChargeCycles(900);
	MsgPipe *m = Get<MsgPipe>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MPPID;
	sched::ChargeCycles(1100);
	if (!m->sendWaiters.empty() || !m->recvWaiters.empty()) sched::ChargeCycles(4000);
	if (Valid4(numSendPtr)) GWrite32(numSendPtr, (u32)m->sendWaiters.size());
	if (Valid4(numRecvPtr)) GWrite32(numRecvPtr, (u32)m->recvWaiters.size());
	for (auto &w : m->sendWaiters) w.Complete(id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	m->sendWaiters.clear();
	for (auto &w : m->recvWaiters) w.Complete(id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	m->recvWaiters.clear();
	// And now the entire buffer is free.
	m->freeSize = m->bufSize;
	return 0;
}

int sceKernelReferMsgPipeStatus(SceUID id, u32 statusPtr) {
	MsgPipe *m = Get<MsgPipe>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MPPID;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 bufSize, freeSize, numSendWaitThreads, numReceiveWaitThreads;
	} info{};
	if (!mem::IsValidRange(statusPtr, sizeof(info))) return -1;
	// Clean up any that have timed out.
	m->SortRecv();
	m->SortSend();
	if (GRead32(statusPtr) != 0) {
		info.size = sizeof(info);
		CopyName(info.name, m->name);
		info.attr = m->attr;
		info.bufSize = m->bufSize;
		info.freeSize = m->freeSize;
		info.numSendWaitThreads = (s32)m->sendWaiters.size();
		info.numReceiveWaitThreads = (s32)m->recvWaiters.size();
		mem::WriteBytes(statusPtr, &info, sizeof(info));
	}
	return 0;
}

}  // namespace

void kernel::RegisterThreadManMsgPipe() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x7C0DC2A0, sceKernelCreateMsgPipe),
		HLEFN(0xF0B7DA1C, sceKernelDeleteMsgPipe),
		HLEFN(0x876DBFAD, sceKernelSendMsgPipe),
		HLEFN(0x7C41F2C2, sceKernelSendMsgPipeCB),
		HLEFN(0x884C9F90, sceKernelTrySendMsgPipe),
		HLEFN(0x74829B76, sceKernelReceiveMsgPipe),
		HLEFN(0xFBFA697D, sceKernelReceiveMsgPipeCB),
		HLEFN(0xDF52098F, sceKernelTryReceiveMsgPipe),
		HLEFN(0x349B864D, sceKernelCancelMsgPipe),
		HLEFN(0x33BE4024, sceKernelReferMsgPipeStatus),
	};
	hle::RegisterModule("ThreadManForUser", funcs);
	hle::RegisterModule("ThreadManForKernel", funcs);
}
