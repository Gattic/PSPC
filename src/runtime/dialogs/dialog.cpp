// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPDialog.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/dialogs/dialog.h"

#include <algorithm>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/host/host.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

namespace utility {

// In seconds. Dialog animations advance by animSpeed frames per Update and take about this long.
static constexpr float FADE_TIME = 0.2f;

const char *DialogStatusName(int status) {
	switch (status) {
	case Dialog::STATUS_NONE: return "NONE";
	case Dialog::STATUS_INITIALIZE: return "INITIALIZE";
	case Dialog::STATUS_RUNNING: return "RUNNING";
	case Dialog::STATUS_FINISHED: return "FINISHED";
	case Dialog::STATUS_SHUTDOWN: return "SHUTDOWN";
	case Dialog::STATUS_SCREENSHOT_UNKNOWN: return "SCREENSHOT_UNKNOWN";
	default: return "(unknown)";
	}
}

void Dialog::InitCommon() {
	UpdateCommon();
	if (const DialogCommon *common = GetCommonParam()) {
		LOGD("UTIL", "dialog %d thread priorities: graphics=%d access=%d font=%d sound=%d", (int)type_,
		     common->graphicsThread, common->accessThread, common->fontThread, common->soundThread);
	}
}

void Dialog::UpdateCommon() {
	okButtonFlag_ = CTRL_CIRCLE;
	cancelButtonFlag_ = CTRL_CROSS;
	if (GetCommonParam() && GetCommonParam()->buttonSwap == 1) {
		okButtonFlag_ = CTRL_CROSS;
		cancelButtonFlag_ = CTRL_CIRCLE;
	}
}

void Dialog::UpdatePendingStatus() {
	if (pendingStatusUs_ != 0 && sched::NowUs() >= pendingStatusUs_) {
		bool changeAllowed = true;
		if (pendingStatus_ == STATUS_NONE && status_ == STATUS_SHUTDOWN) {
			FinishVolatile();
		} else if (pendingStatus_ == STATUS_RUNNING && status_ == STATUS_INITIALIZE) {
			if (!volatileLocked_ && LocksVolatileMemory()) {
				volatileLocked_ = VolatileMemTryLock() == 0;
				changeAllowed = volatileLocked_;
			}
		}
		if (changeAllowed) {
			status_ = pendingStatus_;
			pendingStatusUs_ = 0;
		}
	}
}

Dialog::Status Dialog::GetStatus() {
	UpdatePendingStatus();
	Status ret = status_;
	if (UseAutoStatus()) {
		if (status_ == STATUS_SHUTDOWN) {
			FinishVolatile();
			status_ = STATUS_NONE;
		}
		if (status_ == STATUS_INITIALIZE) status_ = STATUS_RUNNING;
	}
	return ret;
}

bool Dialog::IsBusy() {
	UpdatePendingStatus();
	if (status_ == STATUS_SHUTDOWN && UseAutoStatus()) return false;
	return status_ != STATUS_NONE;
}

void Dialog::FinishAutoShutdown() {
	if (status_ == STATUS_SHUTDOWN && UseAutoStatus()) {
		FinishVolatile();
		status_ = STATUS_NONE;
	}
}

void Dialog::ChangeStatus(Status newStatus, int delayUs) {
	if (delayUs <= 0) {
		if (newStatus == STATUS_NONE && status_ == STATUS_SHUTDOWN) {
			FinishVolatile();
		} else if (newStatus == STATUS_RUNNING && status_ == STATUS_INITIALIZE) {
			if (!volatileLocked_ && LocksVolatileMemory()) volatileLocked_ = VolatileMemTryLock() == 0;
		}
		status_ = newStatus;
		pendingStatus_ = newStatus;
		pendingStatusUs_ = 0;
	} else {
		pendingStatus_ = newStatus;
		pendingStatusUs_ = sched::NowUs() + (u64)delayUs;
	}
}

void Dialog::FinishVolatile() {
	if (!volatileLocked_) return;
	if (VolatileMemUnlock() == 0) {
		volatileLocked_ = false;
		// The dialog used volatile memory meanwhile.
		mem::Memset(0x08400000, 0, 0x00400000);
	}
}

int Dialog::FinishInit(bool locked) {
	// The dialog thread has locked volatile memory (unless that failed).
	if (ReadStatus() == STATUS_NONE) {
		if (locked) VolatileMemUnlock();
		return -1;
	}
	if (locked) volatileLocked_ = true;
	if (ReadStatus() == STATUS_INITIALIZE) ChangeStatus(STATUS_RUNNING, 0);
	return 0;
}

int Dialog::FinishShutdown() {
	if (ReadStatus() != STATUS_SHUTDOWN) return -1;
	ChangeStatus(STATUS_NONE, 0);
	return 0;
}

void Dialog::ChangeStatusInit(int delayUs) {
	ChangeStatus(STATUS_INITIALIZE, 0);
	if (DialogCommon *p = GetCommonParam()) {
		DialogThreadInit(type_, delayUs, p->accessThread, p->graphicsThread);
	} else {
		ChangeStatus(STATUS_RUNNING, delayUs);
	}
}

void Dialog::ChangeStatusShutdown(int delayUs) {
	// Shutting down right away from a start that never happened skips the dialog thread.
	bool skipThread = status_ == STATUS_NONE && pendingStatus_ == STATUS_NONE;
	ChangeStatus(STATUS_SHUTDOWN, 0);
	DialogCommon *p = GetCommonParam();
	if (p && !skipThread) {
		DialogThreadShutdown(type_, delayUs, p->accessThread, p->graphicsThread);
	} else {
		ChangeStatus(STATUS_NONE, delayUs);
	}
}

int Dialog::Shutdown(bool force) {
	ChangeStatus(force ? STATUS_NONE : STATUS_SHUTDOWN, 0);
	return 0;
}

void Dialog::ResetState() {
	status_ = STATUS_NONE;
	pendingStatus_ = STATUS_NONE;
	pendingStatusUs_ = 0;
	volatileLocked_ = false;
	isFading_ = false;
	fadeValue_ = 0;
}

void Dialog::StartFade(bool fadeIn) {
	isFading_ = true;
	fadeTimer_ = 0;
	fadeIn_ = fadeIn;
}

void Dialog::UpdateFade(int animSpeed) {
	if (!isFading_) return;
	// At least a frame per Update, or it would never finish.
	fadeTimer_ += std::max(animSpeed, 1) / 60.0f;
	if (fadeTimer_ < FADE_TIME) {
		fadeValue_ = fadeIn_ ? (u32)(fadeTimer_ / FADE_TIME * 255) : 255 - (u32)(fadeTimer_ / FADE_TIME * 255);
	} else {
		fadeValue_ = fadeIn_ ? 255 : 0;
		isFading_ = false;
		if (!fadeIn_) FinishFadeOut();
	}
}

void Dialog::FinishFadeOut() { ChangeStatus(STATUS_FINISHED, 0); }

void Dialog::UpdateButtons() {
	u32 now = host::IsHeadless() ? 0 : host::GetCtrl().buttons;
	buttons_ = now & ~lastButtons_;  // newly pressed since the last Update
	lastButtons_ = now;
}

bool Dialog::IsButtonPressed(u32 button) const { return !IsFadingOut() && (buttons_ & button) != 0; }

int Dialog::CheckRequest(u32 addr, std::initializer_list<u32> sizes) {
	if (!mem::IsValidRange(addr, sizeof(DialogCommon))) return (int)SCE_ERROR_UTILITY_INVALID_ADDRESS;
	const u32 size = GRead32(addr);
	if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) return (int)SCE_ERROR_UTILITY_INVALID_PARAM_SIZE;
	if (!mem::IsValidRange(addr, size)) return (int)SCE_ERROR_UTILITY_INVALID_ADDRESS;
	return 0;
}

u32 Dialog::GetConfirmButton() { return SystemButtonPreference() == 1 ? CTRL_CROSS : CTRL_CIRCLE; }
u32 Dialog::GetCancelButton() { return SystemButtonPreference() == 1 ? CTRL_CIRCLE : CTRL_CROSS; }

}  // namespace utility
