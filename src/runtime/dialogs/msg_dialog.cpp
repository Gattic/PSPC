// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPMsgDialog.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/dialogs/msg_dialog.h"

#include <cstdio>
#include <cstring>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"

namespace utility {

namespace {

// Rough: on a PSP this takes ~0.55-0.65 s (utility/msgdialog/dialog, workshare).
constexpr int MSG_INIT_DELAY_US = 560000;
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

void CopyCString(char *dst, size_t dstSize, const char *src, size_t srcMax) {
	size_t n = strnlen(src, srcMax);
	if (n >= dstSize) n = dstSize - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
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
	prompt_.reset();

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

	ChangeStatusInit(MSG_INIT_DELAY_US);
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
	// (utility/dialog/abort).
	framesRun_++;
	if ((flag_ & DS_ERROR) || ((flag_ & DS_ABORT) && framesRun_ >= 8)) {
		ChangeStatus(STATUS_FINISHED, 0);
	} else {
		UpdateButtons();
		UpdateCommon();
		const bool wasFadingOut = IsFadingOut();
		UpdateFade(animSpeed);

		if (!wasFadingOut && !IsFadingOut()) {
			const bool canCancel = (flag_ & DS_CANCELBUTTON) != 0;
			const bool canConfirm = (flag_ & DS_VALIDBUTTON) != 0;
			if (Headless()) {
				if (framesRun_ >= HEADLESS_ANSWER_UPDATES) {
					LOGI("UTIL", "MsgDialog: answering by itself (headless)");
					Choose(!canConfirm && canCancel);
				}
			} else {
				if (!prompt_) {
					prompt::Buttons b = (flag_ & DS_YESNO) ? prompt::Buttons::YesNo
					                    : canConfirm && canCancel ? prompt::Buttons::OkCancel
					                                              : prompt::Buttons::Ok;
					prompt_ = prompt::ShowMessage("PSP", msgText_, b);
				}
				int a = prompt_->answer.load();
				if (a == prompt::YES || a == prompt::OK) {
					yesnoChoice_ = 1;
					Choose(!canConfirm && canCancel);
				} else if (a == prompt::NO) {
					yesnoChoice_ = 0;
					Choose(false);
				} else if (a == prompt::CANCEL) {
					Choose(canCancel || !canConfirm);
				} else if (IsButtonPressed(cancelButtonFlag_) && canCancel) {
					// The PSP buttons work too.
					Choose(true);
				} else if (IsButtonPressed(okButtonFlag_) && canConfirm) {
					Choose(false);
				}
			}
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
	prompt_.reset();
	return 0;
}

}  // namespace utility
