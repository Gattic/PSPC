// SPDX-License-Identifier: GPL-2.0-or-later
// sceUtilitySavedata. Ported from PPSSPP (Core/Dialog/PSPSaveDialog.cpp), Copyright (c) 2012-
// PPSSPP Project, GPL-2.0-or-later. The IO runs synchronously (PPSSPP uses a host thread) but keeps
// PPSSPP's Update timing: the first Update starts it, the next one takes the results.
//
// With overlays the visible modes are PPSSPP's screens: the save list with icons and details, the
// save/load/delete confirmations, progress and done screens, answered with the PSP buttons.
// Without (headless) they draw nothing and answer by themselves.
#pragma once

#include <map>
#include <memory>
#include <string>

#include "runtime/dialogs/dialog.h"
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
	enum Banner { DB_NONE, DB_SAVE, DB_LOAD, DB_DELETE };

	std::string GetSelectedSaveDirName() const;
	void StartIO();
	bool FinishIO();
	void ExecuteIOAction();
	// Headless: the visible modes answer by themselves. 1 = yes/OK, 0 = no/back, -1 = not yet.
	int Ask(const std::string &text, bool defaultYes);
	void ResetAsk() { askFrames_ = 0; }
	void UpdateAuto();
	// With overlays: PPSSPP's screens and the player's answers.
	void UpdateInteractive();

	// Drawing (PPSSPP's PSPSaveDialog).
	void DisplayBanner(Banner which);
	void DisplaySaveList(bool canMove = true);
	void DisplaySaveIcon(bool checkExists);
	void DisplaySaveDataInfo1();
	void DisplaySaveDataInfo2(bool showNewData = false);
	void DisplayMessage(std::string_view text, bool hasYesNo = false);
	std::shared_ptr<const ppge::Texture> SaveIcon(int idx);
	void Finish(s32 result);

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
	int ioShownUpdates_ = 0;  // how long the progress screen has been up

	int askFrames_ = 0;
	// A visible mode has done its save/load/delete (headless then backs out of any list).
	bool actionDone_ = false;

	int upFramesHeld_ = 0, downFramesHeld_ = 0;
	std::map<std::string, std::shared_ptr<const ppge::Texture>> icons_;
};

}  // namespace utility
