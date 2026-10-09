// SPDX-License-Identifier: GPL-2.0-or-later
// sceUtilitySavedata. Ported from PPSSPP (Core/Dialog/PSPSaveDialog.cpp), Copyright (c) 2012-
// PPSSPP Project, GPL-2.0-or-later. The IO runs synchronously (PPSSPP uses a host thread) but keeps
// PPSSPP's Update timing: the first Update starts it, the next one takes the results. Nothing is
// drawn; the visible modes answer by themselves (headless) or through host prompts (windowed).
#pragma once

#include <memory>
#include <string>

#include "runtime/dialogs/dialog.h"
#include "runtime/dialogs/host_prompt.h"
#include "runtime/dialogs/savedata.h"

namespace utility {

class SaveDialog : public Dialog {
public:
	SaveDialog() : Dialog(DialogType::SAVEDATA) {}

	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	DialogCommon *GetCommonParam() override;

protected:
	bool UseAutoStatus() override { return false; }

private:
	enum DisplayState {
		DS_NONE,
		DS_SAVE_LIST_CHOICE,
		DS_SAVE_CONFIRM,
		DS_SAVE_CONFIRM_OVERWRITE,
		DS_SAVE_SAVING,
		DS_SAVE_DONE,
		DS_LOAD_LIST_CHOICE,
		DS_LOAD_CONFIRM,
		DS_LOAD_LOADING,
		DS_LOAD_DONE,
		DS_LOAD_NODATA,
		DS_DELETE_LIST_CHOICE,
		DS_DELETE_CONFIRM,
		DS_DELETE_DELETING,
		DS_DELETE_DONE,
		DS_DELETE_NODATA,
		DS_SAVE_FAILED,
		DS_LOAD_FAILED,
		DS_DELETE_FAILED,
	};
	enum IoStatus { IO_NONE, IO_PENDING, IO_DONE, IO_READY };

	std::string GetSelectedSaveDirName() const;
	void StartIO();
	bool FinishIO();
	void ExecuteIOAction();
	void ExecuteNotVisibleIOAction();
	// For the visible modes: the player's answer, if there is one yet. 1 = yes/OK, 0 = no/back.
	int Ask(const std::string &text, bool yesNo, bool defaultYes);
	void ResetAsk() { prompt_.reset(), askFrames_ = 0; }

	DisplayState display_ = DS_NONE;
	Savedata param_;
	SavedataRequest request_{};
	SavedataRequest originalRequest_{};
	u32 requestAddr_ = 0;
	int currentSelectedSave_ = 0;

	IoStatus ioStatus_ = IO_NONE;
	DisplayState ioAction_ = DS_NONE;
	DisplayState ioDisplay_ = DS_NONE;
	SavedataRequest ioRequestStart_{};
	SavedataRequest ioResult_{};
	int ioSaveId_ = 0;
	std::string ioSaveDirName_, ioListSaveDirName_, ioDeleteDir_;

	std::shared_ptr<prompt::State> prompt_;
	int askFrames_ = 0;
	// A visible mode has done its save/load/delete (headless then backs out of any list).
	bool actionDone_ = false;
};

}  // namespace utility
