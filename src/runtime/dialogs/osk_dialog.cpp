// SPDX-License-Identifier: GPL-2.0-or-later
// Ported in part from PPSSPP (Core/Dialog/PSPOskDialog.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/dialogs/osk_dialog.h"

#include <cstddef>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"

namespace utility {

namespace {

std::string g_headlessOskText;  // --osk-text

constexpr int OSK_INIT_DELAY_US = 300000;
constexpr int OSK_SHUTDOWN_DELAY_US = 40000;
constexpr s32 OSK_RESULT_CHANGED = 2;

std::u16string ReadU16String(u32 addr, u32 maxChars = 4096) {
	std::u16string s;
	if (!addr) return s;
	for (u32 i = 0; i < maxChars && mem::IsValidRange(addr + i * 2, 2); i++) {
		u16 c = GRead16(addr + i * 2);
		if (!c) break;
		s.push_back((char16_t)c);
	}
	return s;
}

std::string Utf16ToUtf8(const std::u16string &in) {
	std::string out;
	for (size_t i = 0; i < in.size(); i++) {
		u32 c = in[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
			c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
			i++;
		}
		if (c < 0x80) {
			out.push_back((char)c);
		} else if (c < 0x800) {
			out.push_back((char)(0xC0 | (c >> 6)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		} else if (c < 0x10000) {
			out.push_back((char)(0xE0 | (c >> 12)));
			out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		} else {
			out.push_back((char)(0xF0 | (c >> 18)));
			out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
			out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

std::u16string Utf8ToUtf16(const std::string &in) {
	std::u16string out;
	for (size_t i = 0; i < in.size();) {
		u8 b = (u8)in[i];
		u32 c;
		int n;
		if (b < 0x80) {
			c = b;
			n = 1;
		} else if ((b & 0xE0) == 0xC0) {
			c = b & 0x1F;
			n = 2;
		} else if ((b & 0xF0) == 0xE0) {
			c = b & 0x0F;
			n = 3;
		} else {
			c = b & 0x07;
			n = 4;
		}
		for (int k = 1; k < n && i + k < in.size(); k++) c = (c << 6) | ((u8)in[i + k] & 0x3F);
		i += n;
		if (c >= 0x10000) {
			c -= 0x10000;
			out.push_back((char16_t)(0xD800 + (c >> 10)));
			out.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
		} else {
			out.push_back((char16_t)c);
		}
	}
	return out;
}

}  // namespace

DialogCommon *OskDialog::GetCommonParam() {
	if (!paramsAddr_ || !mem::IsValidRange(paramsAddr_, sizeof(DialogCommon))) return nullptr;
	return (DialogCommon *)GPtr(paramsAddr_);
}

int OskDialog::Init(u32 paramAddr) {
	if (GetStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {0x40, 0x44});
	if (check < 0) return check;
	mem::ReadBytes(&params_, paramAddr, sizeof(params_));
	if (!mem::IsValidRange(params_.fields, sizeof(OskData))) {
		LOGE("UTIL", "sceUtilityOskInitStart: invalid field data (%08x)", params_.fields);
		return -1;
	}
	paramsAddr_ = paramAddr;
	if (params_.fieldCount != 1) LOGW("UTIL", "sceUtilityOskInitStart: unsupported field count %d", params_.fieldCount);

	ChangeStatusInit(OSK_INIT_DELAY_US);
	OskData field;
	mem::ReadBytes(&field, params_.fields, sizeof(field));
	inputChars_ = ReadU16String(field.intext);
	framesRun_ = 0;
	prompt_.reset();
	LOGI("UTIL", "OSK: \"%s\" (initial text \"%s\")", Utf16ToUtf8(ReadU16String(field.desc)).c_str(),
	     Utf16ToUtf8(inputChars_).c_str());

	UpdateButtons();
	InitCommon();
	StartFade(true);
	return 0;
}

u32 OskDialog::FieldMaxLength() {
	const u32 length = GRead32(params_.fields + offsetof(OskData, outtextlength));
	const u32 limit = GRead32(params_.fields + offsetof(OskData, outtextlimit));
	if (length == 0) return 0;
	if (limit > length - 1 || limit == 0) return length - 1;
	return limit;
}

// Only writes the text and its terminator, not the rest of the buffer.
void OskDialog::WriteOutput(s32 fieldResult) {
	OskData field;
	mem::ReadBytes(&field, params_.fields, sizeof(field));
	size_t end = field.outtextlength;
	if (end > inputChars_.size()) end = inputChars_.size() + 1;
	if (end != 0 && mem::IsValidRange(field.outtext, (u32)(end * 2))) {
		u32 maxLen = FieldMaxLength();
		for (size_t i = 0; i < end; ++i) {
			u16 v = (i < maxLen && i < inputChars_.size()) ? (u16)inputChars_[i] : 0;
			GWrite16(field.outtext + (u32)i * 2, v);
		}
	}
	if (DialogCommon *c = GetCommonParam()) c->result = 0;
	GWrite32(params_.fields + offsetof(OskData, result), (u32)fieldResult);
}

int OskDialog::Update(int animSpeed) {
	if (GetStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	UpdateButtons();
	UpdateCommon();
	framesRun_++;

	if (Headless()) {
		// Nobody to type: the --osk-text answer if one was given, else the initial text.
		if (framesRun_ >= 2 && !IsFadingOut() && pendingStatus_ != STATUS_FINISHED) {
			if (!g_headlessOskText.empty()) {
				inputChars_ = Utf8ToUtf16(g_headlessOskText);
				u32 maxLen = FieldMaxLength();
				if (inputChars_.size() > maxLen) inputChars_.resize(maxLen);
				LOGI("UTIL", "OSK: returning \"%s\" (headless, --osk-text)", g_headlessOskText.c_str());
			} else {
				LOGI("UTIL", "OSK: returning the initial text (headless)");
			}
			ChangeStatus(STATUS_FINISHED, 0);
		}
	} else {
		if (!prompt_) {
			OskData field;
			mem::ReadBytes(&field, params_.fields, sizeof(field));
			std::string desc = Utf16ToUtf8(ReadU16String(field.desc));
			prompt_ = prompt::AskText("PSP keyboard", desc.empty() ? "Enter text:" : desc, Utf16ToUtf8(inputChars_));
		}
		int a = prompt_->answer.load();
		if (a == prompt::OK) {
			inputChars_ = Utf8ToUtf16(prompt_->text);
			u32 maxLen = FieldMaxLength();
			if (inputChars_.size() > maxLen) inputChars_.resize(maxLen);
			ChangeStatus(STATUS_FINISHED, 0);
		} else if (a != prompt::PENDING) {
			ChangeStatus(STATUS_FINISHED, 0);
		}
	}
	WriteOutput(OSK_RESULT_CHANGED);
	return 0;
}

int OskDialog::Shutdown(bool force) {
	if (GetStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(OSK_SHUTDOWN_DELAY_US);
	prompt_.reset();
	return 0;
}

void SetHeadlessOskText(const std::string &text) { g_headlessOskText = text; }

}  // namespace utility
