// SPDX-License-Identifier: GPL-2.0-or-later
// sceUtilityMsgDialog. Ported from PPSSPP (Core/Dialog/PSPMsgDialog.cpp), Copyright (c) 2012-
// PPSSPP Project, GPL-2.0-or-later.
#pragma once

#include <string>

#include "runtime/dialogs/dialog.h"

namespace utility {

#pragma pack(push, 4)
struct MsgDialogParams {
	DialogCommon common;
	s32 result;
	s32 type;
	u32 errorNum;
	char string[512];
	// End of request V1 (572 bytes)
	u32 options;
	u32 buttonPressed;
	// End of request V2 (580 bytes)
	char okayButton[64];
	char cancelButton[64];
	// End of request V3 (708 bytes)
};
#pragma pack(pop)
static_assert(sizeof(MsgDialogParams) == 708, "msg dialog params");

class MsgDialog : public Dialog {
public:
	MsgDialog() : Dialog(DialogType::MSG) {}

	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	DialogCommon *GetCommonParam() override { return &params_.common; }
	int Abort();

protected:
	bool UseAutoStatus() override { return false; }

private:
	void FormatErrorCode(u32 code);
	void Choose(bool cancel);
	void DisplayMessage(const std::string &text, bool hasYesNo, bool hasOK);

	enum Flags : u32 {
		DS_MSG = 0x1,
		DS_ERRORMSG = 0x2,
		DS_YESNO = 0x4,
		DS_DEFNO = 0x8,
		DS_OK = 0x10,
		DS_VALIDBUTTON = 0x20,
		DS_CANCELBUTTON = 0x40,
		DS_NOSOUND = 0x80,
		DS_ERROR = 0x100,
		DS_ABORT = 0x200,
	};

	u32 flag_ = 0;
	int framesRun_ = 0;
	MsgDialogParams params_{};
	u32 paramsAddr_ = 0;
	char msgText_[512]{};
	float scrollPos_ = 0.0f;
	int framesUpHeld_ = 0, framesDownHeld_ = 0;
};

}  // namespace utility
