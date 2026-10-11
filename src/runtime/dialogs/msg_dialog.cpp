// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPMsgDialog.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/dialogs/msg_dialog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"

namespace utility {

namespace {

// On a PSP this takes 534-550 ms in utility/msgdialog/dialog, and 625-675 ms when the access and
// graphics threads share one priority and take turns (utility/msgdialog/workshare).
constexpr int MSG_INIT_DELAY_US = 540000;
constexpr int MSG_INIT_SHARED_DELAY_US = 640000;
constexpr int MSG_SHUTDOWN_DELAY_US = 26000;

constexpr u32 SIZE_V1 = 572, SIZE_V2 = 580, SIZE_V3 = 708;

constexpr u32 OPTION_TEXTSOUND = 0x00000001;
constexpr u32 OPTION_NOSOUND = 0x00000002;
constexpr u32 OPTION_YESNO = 0x00000010;
constexpr u32 OPTION_OK = 0x00000020;
constexpr u32 OPTION_NOCANCEL = 0x00000080;
constexpr u32 OPTION_DEFAULT_NO = 0x00000100;
constexpr u32 OPTION_SUPPORTED = 0x000001B3;

// Without a window nobody can press a button: answer with the default choice after this many
// Updates (about 2 s), long enough for games (and tests) that Abort the dialog themselves.
constexpr int HEADLESS_ANSWER_UPDATES = 120;

constexpr float FONT_SCALE = 0.55f;

void CopyCString(char *dst, size_t dstSize, const char *src, size_t srcMax) {
	size_t n = strnlen(src, srcMax);
	if (n >= dstSize) n = dstSize - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
}

std::string_view FixedField(const char *s, size_t maxLen) { return std::string_view(s, strnlen(s, maxLen)); }

int NonAsciiCount(const std::string &s) {
	int n = 0;
	for (unsigned char c : s)
		if (c >= 0xC0) n++;  // lead bytes
	return n;
}

}  // namespace

int MsgDialog::Init(u32 paramAddr) {
	if (GetStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {SIZE_V1, SIZE_V2, SIZE_V3});
	if (check < 0) return check;
	if (paramAddr & 3) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	paramsAddr_ = paramAddr;

	u32 size = GRead32(paramAddr);
	memset(&params_, 0, sizeof(params_));
	mem::ReadBytes(&params_, paramAddr, size);

	if (params_.options & ~OPTION_SUPPORTED) LOGW("UTIL", "MsgDialog: unsupported options %08x", params_.options);

	flag_ = 0;
	framesRun_ = 0;
	scrollPos_ = 0.0f;
	framesUpHeld_ = framesDownHeld_ = 0;

	// Check request invalidity.
	if (params_.type == 0 && !(params_.errorNum & 0x80000000)) {
		flag_ |= DS_ERROR;
		params_.result = (s32)SCE_ERROR_UTILITY_MSGDIALOG_ERRORCODEINVALID;
	} else if (size == SIZE_V2 && params_.type == 1) {
		u32 validOp = OPTION_TEXTSOUND | OPTION_YESNO | OPTION_DEFAULT_NO;
		if (((params_.options | validOp) ^ validOp) != 0) {
			flag_ |= DS_ERROR;
			params_.result = (s32)SCE_ERROR_UTILITY_MSGDIALOG_BADOPTION;
		}
	} else if (size == SIZE_V3) {
		if ((params_.options & OPTION_DEFAULT_NO) && !(params_.options & OPTION_YESNO)) {
			flag_ |= DS_ERROR;
			params_.result = (s32)SCE_ERROR_UTILITY_MSGDIALOG_BADOPTION;
		}
		if (params_.options & ~OPTION_SUPPORTED) {
			flag_ |= DS_ERROR;
			params_.result = (s32)SCE_ERROR_UTILITY_MSGDIALOG_BADOPTION;
		}
	}

	if (flag_ == 0) {
		yesnoChoice_ = 1;
		if (params_.type == 1) flag_ |= DS_MSG;
		if (params_.type == 0) flag_ |= DS_ERRORMSG;
		if ((params_.options & OPTION_YESNO) && (size == SIZE_V3 || (size == SIZE_V2 && params_.type == 1)))
			flag_ |= DS_YESNO;
		if (params_.options & OPTION_DEFAULT_NO) {
			yesnoChoice_ = 0;
			flag_ |= DS_DEFNO;
		}
		if ((params_.options & OPTION_OK) && size == SIZE_V3) {
			yesnoChoice_ = 1;
			flag_ |= DS_OK;
		}
		if ((flag_ & DS_YESNO) || (flag_ & DS_OK)) flag_ |= DS_VALIDBUTTON;
		if (!((params_.options & OPTION_NOCANCEL) && size == SIZE_V3)) flag_ |= DS_CANCELBUTTON;
		if (params_.options & OPTION_NOSOUND) flag_ |= DS_NOSOUND;
	}

	if (flag_ & DS_ERRORMSG) {
		FormatErrorCode(params_.errorNum);
	} else {
		CopyCString(msgText_, sizeof(msgText_), params_.string, sizeof(params_.string));
	}
	LOGI("UTIL", "MsgDialog: \"%s\" (options %08x)", msgText_, params_.options);

	const DialogCommon *common = GetCommonParam();
	ChangeStatusInit(common && common->accessThread == common->graphicsThread ? MSG_INIT_SHARED_DELAY_US
	                                                                          : MSG_INIT_DELAY_US);
	UpdateButtons();
	InitCommon();
	StartFade(true);
	return 0;
}

void MsgDialog::FormatErrorCode(u32 code) {
	switch (code) {
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN:
		snprintf(msgText_, sizeof(msgText_), "Save data was corrupt. (%08x)", code);
		break;
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_MS:
	case SCE_UTILITY_SAVEDATA_ERROR_RW_NO_MEMSTICK:
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_NO_MS:
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_MS:
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_MS:
		snprintf(msgText_, sizeof(msgText_), "Memory stick not inserted. (%08x)", code);
		break;
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA:
	case SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA:
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA:
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA:
		snprintf(msgText_, sizeof(msgText_), "Warning: no save data was found. (%08x)", code);
		break;
	case SCE_UTILITY_SAVEDATA_ERROR_RW_MEMSTICK_FULL:
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE:
		snprintf(msgText_, sizeof(msgText_), "Memory stick full. Check your storage space. (%08x)", code);
		break;
	default:
		snprintf(msgText_, sizeof(msgText_), "Error code: %08x", code);
	}
}

void MsgDialog::DisplayMessage(const std::string &text, bool hasYesNo, bool hasOK) {
	ppge::Style buttonStyle = FadedStyle(ppge::BOX_CENTER, FONT_SCALE);
	ppge::Style messageStyle = FadedStyle(ppge::BOX_HCENTER, FONT_SCALE);

	// Without the scrollbar, we have 390 total pixels.
	float WRAP_WIDTH = 340.0f;
	if ((size_t)NonAsciiCount(text) >= text.size() / 4) {
		WRAP_WIDTH = 376.0f;
		if (text.size() > 12) messageStyle.scale = 0.6f;
	}

	float totalHeight = 0.0f;
	ppge::MeasureString(nullptr, &totalHeight, text, FONT_SCALE, ppge::LINE_WRAP_WORD, WRAP_WIDTH);
	// The PSP normally only shows about 8 lines at a time. For improved UX, we intentionally show
	// part of the next line.
	float visibleHeight = std::min(totalHeight, 175.0f);
	float h2 = visibleHeight / 2.0f;

	float centerY = 135.0f;
	float sy = centerY - h2 - 15.0f;
	float ey = centerY + h2 + 20.0f;
	float buttonY = centerY + h2 + 5.0f;

	auto drawSelectionBoxAndAdjust = [&](float x) {
		// Box has a fixed size.
		float w = 15.0f;
		float h = 8.0f;
		ppge::DrawRect(x - w, buttonY - h, x + w, buttonY + h, CalcFadedColor(0x6DCFCFCF));
		centerY -= h + 5.0f;
		sy -= h + 5.0f;
		ey = buttonY + h * 2.0f + 5.0f;
	};

	if (hasYesNo) {
		drawSelectionBoxAndAdjust(yesnoChoice_ == 1 ? 204.0f : 273.0f);
		ppge::DrawString("Yes", 203.0f, buttonY - 1.0f, buttonStyle);
		ppge::DrawString("No", 272.0f, buttonY - 1.0f, buttonStyle);
		if (IsButtonPressed(CTRL_LEFT) && yesnoChoice_ == 0) {
			yesnoChoice_ = 1;
		} else if (IsButtonPressed(CTRL_RIGHT) && yesnoChoice_ == 1) {
			yesnoChoice_ = 0;
		}
		buttonY += 8.0f + 5.0f;
	}

	if (hasOK) {
		drawSelectionBoxAndAdjust(240.0f);
		ppge::DrawString("OK", 239.0f, buttonY - 1.0f, buttonStyle);
		buttonY += 8.0f + 5.0f;
	}

	ppge::Scissor(0, (int)(centerY - h2 - 2), 480, (int)(centerY + h2 + 2));
	ppge::DrawStringWrapped(text, 240.0f, centerY - h2 - scrollPos_, WRAP_WIDTH, 0, messageStyle);
	ppge::ScissorReset();

	// Do we need a scrollbar?
	if (visibleHeight < totalHeight) {
		float scrollSpeed = 5.0f;
		float scrollMax = totalHeight - visibleHeight;

		float bobHeight = (visibleHeight / totalHeight) * visibleHeight;
		float bobOffset = (scrollPos_ / scrollMax) * (visibleHeight - bobHeight);
		float bobY1 = centerY - h2 + bobOffset;
		ppge::DrawRect(435.0f, bobY1, 440.0f, bobY1 + bobHeight, CalcFadedColor(0xFFCCCCCC));

		auto buttonDown = [this](u32 btn, int &held) {
			if (IsButtonPressed(btn)) {
				held = 0;
				return true;
			}
			return IsButtonHeld(btn, held, 1, 1);
		};
		if (buttonDown(CTRL_DOWN, framesDownHeld_) && scrollPos_ < scrollMax) scrollPos_ = std::min(scrollMax, scrollPos_ + scrollSpeed);
		if (buttonDown(CTRL_UP, framesUpHeld_) && scrollPos_ > 0.0f) scrollPos_ = std::max(0.0f, scrollPos_ - scrollSpeed);
	}

	ppge::DrawRect(40.0f, sy, 440.0f, sy + 1.0f, CalcFadedColor(0xFFFFFFFF));
	ppge::DrawRect(40.0f, ey, 440.0f, ey + 1.0f, CalcFadedColor(0xFFFFFFFF));
}

// The player made a choice: cancel/back, or the button under yesnoChoice_.
void MsgDialog::Choose(bool cancel) {
	const u32 size = params_.common.size;
	if (cancel) {
		params_.buttonPressed = (size == SIZE_V3 || (size == SIZE_V2 && (flag_ & DS_YESNO))) ? 3 : 0;
	} else {
		params_.buttonPressed = yesnoChoice_ == 0 ? 2 : 1;
	}
	StartFade(false);
}

int MsgDialog::Update(int animSpeed) {
	if (GetStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;

	// On a PSP an Abort only takes effect from the 8th Update, whatever animSpeed is
	// (utility/dialog/abort), and a bad request finishes at the 9th (utility/msgdialog/dialog).
	framesRun_++;
	if (((flag_ & DS_ERROR) && framesRun_ >= 9) || ((flag_ & DS_ABORT) && framesRun_ >= 8)) {
		ChangeStatus(STATUS_FINISHED, 0);
	} else if (flag_ & DS_ERROR) {
		// Nothing to show; the error result is written when it finishes.
		return 0;
	} else {
		UpdateButtons();
		UpdateCommon();
		const bool wasFadingOut = IsFadingOut();
		UpdateFade(animSpeed);

		const bool canCancel = (flag_ & DS_CANCELBUTTON) != 0;
		const bool canConfirm = (flag_ & DS_VALIDBUTTON) != 0;
		if (Headless()) {
			if (!wasFadingOut && !IsFadingOut() && framesRun_ >= HEADLESS_ANSWER_UPDATES) {
				LOGI("UTIL", "MsgDialog: answering by itself (headless)");
				Choose(!canConfirm && canCancel);
			}
		} else {
			// As the PSP shows it (PPSSPP's layout): the message between two lines over a tinted
			// screen, the Yes/No or OK choice, and the button guide.
			StartDraw();
			ppge::DrawRect(0, 0, 480, 272, CalcFadedColor(0xC0C8B2AC));
			if ((flag_ & DS_MSG) || (flag_ & DS_ERRORMSG)) DisplayMessage(msgText_, (flag_ & DS_YESNO) != 0, (flag_ & DS_OK) != 0);
			const bool v3 = params_.common.size == SIZE_V3;
			if (flag_ & (DS_OK | DS_VALIDBUTTON))
				DisplayButtons(DS_BUTTON_OK, v3 ? FixedField(params_.okayButton, sizeof(params_.okayButton)) : std::string_view());
			if (flag_ & DS_CANCELBUTTON)
				DisplayButtons(DS_BUTTON_CANCEL, v3 ? FixedField(params_.cancelButton, sizeof(params_.cancelButton)) : std::string_view());
			if (!wasFadingOut && !IsFadingOut()) {
				if (IsButtonPressed(cancelButtonFlag_) && canCancel) {
					Choose(true);
				} else if (IsButtonPressed(okButtonFlag_) && canConfirm) {
					Choose(false);
				}
			}
			EndDraw();
		}
		params_.result = 0;
	}

	mem::WriteBytes(paramsAddr_, &params_, params_.common.size);
	return 0;
}

int MsgDialog::Abort() {
	if (GetStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	// The status changes in a later Update.
	flag_ |= DS_ABORT;
	return 0;
}

int MsgDialog::Shutdown(bool force) {
	if (GetStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(MSG_SHUTDOWN_DELAY_US);
	return 0;
}

}  // namespace utility
