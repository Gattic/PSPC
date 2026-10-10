// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPDialog.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/dialogs/dialog.h"

#include <algorithm>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/dialogs/home_menu.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

namespace ctrl {
u32 RawButtons();
}

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
	ppge::Clear(ppge::Layer::Dialog);
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
	// Not while the HOME menu is up.
	u32 now = Headless() ? 0 : homemenu::FilterButtons(ctrl::RawButtons());
	buttons_ = now & ~lastButtons_;  // newly pressed since the last Update
	lastButtons_ = now;
}

bool Dialog::IsButtonPressed(u32 button) const { return !IsFadingOut() && (buttons_ & button) != 0; }

bool Dialog::IsButtonHeld(u32 button, int &framesHeld, int threshold, int repeatRate) const {
	// Held since the last Update too (not just pressed).
	if (!IsFadingOut() && (lastButtons_ & button) && !(buttons_ & button)) {
		framesHeld++;
	} else {
		framesHeld = 0;
		return false;
	}
	return framesHeld >= threshold && (framesHeld % repeatRate) == 0;
}

// ---- drawing

void Dialog::StartDraw() {
	ppge::Begin(ppge::Layer::Dialog);
	ppge::DrawRect(0, 0, 480, 272, CalcFadedColor(0x20000000));
}

void Dialog::EndDraw() { ppge::End(); }

u32 Dialog::CalcFadedColor(u32 color) const {
	u32 alpha = (color >> 24) * fadeValue_ / 255;
	return (color & 0x00FFFFFF) | (alpha << 24);
}

ppge::Style Dialog::FadedStyle(ppge::Align align, float scale) const {
	ppge::Style s;
	s.align = align;
	s.scale = scale;
	s.color = CalcFadedColor(s.color);
	s.hasShadow = true;
	s.shadowColor = CalcFadedColor(s.shadowColor);
	return s;
}

ppge::Style Dialog::FadedImageStyle() const {
	ppge::Style s;
	s.color = CalcFadedColor(s.color);
	return s;
}

void Dialog::DisplayButtons(int flags, std::string_view caption) {
	char safeCaption[65] = {0};
	const bool useCaption = !caption.empty();
	if (useCaption) memcpy(safeCaption, caption.data(), std::min<size_t>(caption.size(), 64));
	ppge::Style textStyle = FadedStyle(ppge::BOX_LEFT, 0.55f);
	const DialogCommon *common = const_cast<Dialog *>(this)->GetCommonParam();
	if (!common) return;
	float x1 = 183.5f, x2 = 261.5f;
	if (common->buttonSwap == 1) {
		x1 = 261.5f;
		x2 = 183.5f;
	}
	if (flags & DS_BUTTON_OK) {
		ppge::DrawIcon(OkIcon(), x2, 256, 11.5f, 11.5f, textStyle);
		ppge::DrawString(useCaption ? safeCaption : "Enter", x2 + 14.5f, 252, textStyle);
	}
	if (flags & DS_BUTTON_CANCEL) {
		ppge::DrawIcon(CancelIcon(), x1, 256, 11.5f, 11.5f, textStyle);
		ppge::DrawString(useCaption ? safeCaption : "Back", x1 + 14.5f, 252, textStyle);
	}
}

int Dialog::CheckRequest(u32 addr, std::initializer_list<u32> sizes) {
	if (!mem::IsValidRange(addr, sizeof(DialogCommon))) return (int)SCE_ERROR_UTILITY_INVALID_ADDRESS;
	const u32 size = GRead32(addr);
	if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) return (int)SCE_ERROR_UTILITY_INVALID_PARAM_SIZE;
	if (!mem::IsValidRange(addr, size)) return (int)SCE_ERROR_UTILITY_INVALID_ADDRESS;
	return 0;
}

u32 Dialog::GetConfirmButton() { return SystemButtonPreference() == 1 ? CTRL_CROSS : CTRL_CIRCLE; }
u32 Dialog::GetCancelButton() { return SystemButtonPreference() == 1 ? CTRL_CIRCLE : CTRL_CROSS; }

// The layout of PPSSPP's network dialog (Core/Dialog/PSPNetconfDialog.cpp).
void Dialog::DisplayStatusScreen(std::string_view title, std::string_view message, int buttons, float progress) {
	StartDraw();
	ppge::DrawRect(0, 0, 480, 272, CalcFadedColor(0xC0C8B2AC));
	ppge::DrawRect(0, 0, 480, 22, CalcFadedColor(0x65636358));
	ppge::Style bannerStyle = FadedStyle(ppge::BOX_VCENTER, 0.6f);
	bannerStyle.hasShadow = false;
	ppge::DrawIcon(ppge::Icon::Hexagon, 10, 5, 11.0f, 11.0f, FadedImageStyle());
	ppge::DrawString(title, 31, 10, bannerStyle);
	ppge::DrawStringWrapped(message, 241, 132, 360.0f, 0, FadedStyle(ppge::BOX_CENTER, 0.55f));
	if (progress >= 0.0f) {
		const float x1 = 140, x2 = 340, y = 175;
		ppge::DrawRect(x1, y, x2, y + 6, CalcFadedColor(0x60000000));
		ppge::DrawRect(x1, y, x1 + (x2 - x1) * std::min(progress, 1.0f), y + 6, CalcFadedColor(0xFFFFFFFF));
	}
	if (buttons) DisplayButtons(buttons);
	EndDraw();
}

}  // namespace utility
