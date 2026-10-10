// SPDX-License-Identifier: GPL-2.0-or-later
// sceUtilityOsk (on-screen keyboard). Ported in part from PPSSPP (Core/Dialog/PSPOskDialog.cpp),
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. Without a window the default text is
// returned; with one, a host text input box replaces the PSP keyboard.
#pragma once

#include <memory>
#include <string>

#include "runtime/dialogs/dialog.h"
#include "runtime/dialogs/host_prompt.h"

namespace utility {

#pragma pack(push, 4)
struct OskData {
	s32 unk_00;
	s32 unk_04;
	s32 language;
	s32 unk_12;
	s32 inputtype;
	s32 lines;
	s32 unk_24;
	u32 desc;    // u16 *
	u32 intext;  // u16 *
	u32 outtextlength;  // in u16s, including the terminator
	u32 outtext;        // u16 *
	s32 result;
	u32 outtextlimit;
};
struct OskParams {
	DialogCommon base;
	s32 fieldCount;
	u32 fields;  // OskData *
	s32 state;
	s32 unk_60;
};
#pragma pack(pop)
static_assert(sizeof(OskData) == 52, "osk data");
static_assert(sizeof(OskParams) == 64, "osk params");

class OskDialog : public Dialog {
public:
	OskDialog() : Dialog(DialogType::OSK) {}

	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	DialogCommon *GetCommonParam() override;

protected:
	bool UseAutoStatus() override { return false; }

private:
	u32 FieldMaxLength();
	void WriteOutput(s32 fieldResult);

	u32 paramsAddr_ = 0;
	OskParams params_{};
	std::u16string inputChars_;
	int framesRun_ = 0;
	std::shared_ptr<prompt::State> prompt_;
};

// Headless runs have nobody to type: the on-screen keyboard returns this text (pspc --osk-text)
// instead of the initial text when it is not empty.
void SetHeadlessOskText(const std::string &text);

}  // namespace utility
