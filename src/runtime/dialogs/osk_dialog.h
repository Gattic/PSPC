// SPDX-License-Identifier: GPL-2.0-or-later
// sceUtilityOsk (on-screen keyboard). Ported from PPSSPP (Core/Dialog/PSPOskDialog.cpp,
// PSPOskConstants.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later: with overlays, its
// keyboard grid (Latin, kana, Hangul, Cyrillic, full-width Latin) driven by the d-pad; without
// (headless), the --osk-text answer or the initial text.
#pragma once

#include <string>

#include "runtime/dialogs/dialog.h"

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
	s32 FieldInputType();
	void WriteOutput(s32 fieldResult);

	// The keyboard (overlays).
	void UpdateKeyboard();
	void RenderKeyboard();
	std::u16string CombinationString(bool isInput);  // Japanese, Korean
	std::u16string CombinationKorean(bool isInput);
	void RemoveKorean();
	int Cols() const;
	int Rows() const;

	u32 paramsAddr_ = 0;
	OskParams params_{};
	std::u16string inputChars_;
	std::string desc_;
	int framesRun_ = 0;

	int selectedChar_ = 0;
	int currentKeyboard_ = 0;          // OskKeyboardDisplay
	int currentKeyboardLanguage_ = 0;  // OskKeyboardLanguage
	bool isCombinated_ = false;
	int iLevel_ = 0;        // Hangul composition state
	int iValue_[3] = {};
	int held_[6] = {};      // frames each repeating button has been held
};

// Headless runs have nobody to type: the on-screen keyboard returns this text (pspc --osk-text)
// instead of the initial text when it is not empty.
void SetHeadlessOskText(const std::string &text);

}  // namespace utility
