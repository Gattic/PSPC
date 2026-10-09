// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPSaveDialog.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/dialogs/save_dialog.h"

#include <algorithm>
#include <cstring>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"

namespace utility {

namespace {

// Rough: at least 100 ms to init on a PSP; shutdown depends on the dialog threads.
constexpr int SAVEDATA_INIT_DELAY_US = 200000;
constexpr int SAVEDATA_SHUTDOWN_DELAY_US = 2000;

constexpr u32 SAVEDATA_DIALOG_SIZE_V1 = 1480;
constexpr u32 SAVEDATA_DIALOG_SIZE_V2 = 1500;
constexpr u32 SAVEDATA_DIALOG_SIZE_V3 = 1536;

// Headless: how many Updates a visible step stays up before it answers by itself.
constexpr int HEADLESS_ANSWER_UPDATES = 3;

const char *const kModeNames[] = {
	"AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE", "LISTDELETE", "LISTALLDELETE",
	"SIZES", "AUTODELETE", "DELETE", "LIST", "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE",
	"READDATA", "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA", "GETSIZE",
};

bool HasPathTraversal(const char *s, size_t maxLen) {
	std::string v(s, strnlen(s, maxLen));
	if (v.find('/') != std::string::npos || v.find('\\') != std::string::npos) return true;
	return v == "." || v == "..";
}

}  // namespace

DialogCommon *SaveDialog::GetCommonParam() { return param_.GetPspParam() ? &param_.GetPspParam()->common : nullptr; }

int SaveDialog::Init(u32 paramAddr) {
	if (GetStatus() != STATUS_NONE) {
		LOGE("UTIL", "A save request is already running, not starting a new one");
		return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	}
	ioStatus_ = IO_NONE;
	requestAddr_ = 0;
	const int check = CheckRequest(paramAddr, {SAVEDATA_DIALOG_SIZE_V1, SAVEDATA_DIALOG_SIZE_V2, SAVEDATA_DIALOG_SIZE_V3});
	if (check < 0) return check;
	if (paramAddr & 3) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	requestAddr_ = paramAddr;

	u32 size = GRead32(paramAddr);
	memset(&request_, 0, sizeof(request_));
	mem::ReadBytes(&request_, paramAddr, size);
	memcpy(&originalRequest_, &request_, sizeof(request_));

	// The names become parts of host paths.
	if (HasPathTraversal(request_.gameName, sizeof(request_.gameName)) ||
	    HasPathTraversal(request_.saveName, sizeof(request_.saveName)) ||
	    HasPathTraversal(request_.fileName, sizeof(request_.fileName))) {
		LOGE("UTIL", "sceUtilitySavedataInitStart: path separator in a name");
		return (int)SCE_ERROR_UTILITY_INVALID_PARAM_SIZE;
	}

	Savedata::Init();
	int retval = param_.SetPspParam(&request_);

	const u32 mode = (u32)request_.mode;
	LOGI("UTIL", "sceUtilitySavedataInitStart(%08x): %s (%d) game=%.13s save=%.20s file=%.13s", paramAddr,
	     mode < sizeof(kModeNames) / sizeof(kModeNames[0]) ? kModeNames[mode] : "UNKNOWN", mode, request_.gameName,
	     request_.saveName, request_.fileName);

	yesnoChoice_ = 1;
	switch (request_.focus) {
	case FOCUS_NAME: currentSelectedSave_ = param_.GetSaveNameIndex(&request_); break;
	case FOCUS_FIRSTLIST: currentSelectedSave_ = param_.GetFirstListSave(); break;
	case FOCUS_LASTLIST: currentSelectedSave_ = param_.GetLastListSave(); break;
	case FOCUS_LATEST: currentSelectedSave_ = param_.GetLatestSave(); break;
	case FOCUS_OLDEST: currentSelectedSave_ = param_.GetOldestSave(); break;
	case FOCUS_FIRSTDATA: currentSelectedSave_ = param_.GetFirstDataSave(); break;
	case FOCUS_LASTDATA: currentSelectedSave_ = param_.GetLastDataSave(); break;
	case FOCUS_FIRSTEMPTY: currentSelectedSave_ = param_.GetFirstEmptySave(); break;
	case FOCUS_LASTEMPTY: currentSelectedSave_ = param_.GetLastEmptySave(); break;
	default:
		LOGW("UTIL", "Unknown save list focus option: %d", request_.focus);
		currentSelectedSave_ = 0;
		break;
	}
	if (!param_.WouldHaveMultiSaveName(&request_)) currentSelectedSave_ = 0;
	if (currentSelectedSave_ < 0) currentSelectedSave_ = 0;

	switch (request_.mode) {
	case SAVEDATA_LOAD:
		// A save without PARAM.SFO is broken even when it's empty (utility/savedata/loadbroken).
		if (param_.GetFilenameCount() > 0 && param_.GetFileInfo(0).broken) {
			request_.common.result = (s32)SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN;
			display_ = DS_LOAD_FAILED;
		} else if (param_.GetFileInfo(0).size != 0) {
			display_ = DS_LOAD_CONFIRM;
		} else {
			display_ = DS_LOAD_NODATA;
		}
		break;
	case SAVEDATA_AUTOLOAD: display_ = DS_NONE; break;
	case SAVEDATA_LISTLOAD: display_ = param_.GetFilenameCount() == 0 ? DS_LOAD_NODATA : DS_LOAD_LIST_CHOICE; break;
	case SAVEDATA_SAVE:
		if (param_.GetFileInfo(0).size != 0) {
			yesnoChoice_ = 0;
			display_ = DS_SAVE_CONFIRM_OVERWRITE;
		} else {
			display_ = DS_SAVE_CONFIRM;
		}
		break;
	case SAVEDATA_AUTOSAVE: display_ = DS_NONE; break;
	case SAVEDATA_LISTSAVE: display_ = DS_SAVE_LIST_CHOICE; break;
	case SAVEDATA_LISTALLDELETE:
	case SAVEDATA_LISTDELETE:
		display_ = param_.GetFilenameCount() == 0 ? DS_DELETE_NODATA : DS_DELETE_LIST_CHOICE;
		break;
	case SAVEDATA_SIZES:
	case SAVEDATA_LIST:
	case SAVEDATA_FILES:
	case SAVEDATA_GETSIZE:
	case SAVEDATA_MAKEDATASECURE:
	case SAVEDATA_MAKEDATA:
	case SAVEDATA_WRITEDATASECURE:
	case SAVEDATA_WRITEDATA:
	case SAVEDATA_READDATASECURE:
	case SAVEDATA_READDATA:
	case SAVEDATA_ERASESECURE:
	case SAVEDATA_ERASE:
	case SAVEDATA_DELETEDATA:
	case SAVEDATA_AUTODELETE:
		display_ = DS_NONE;
		break;
	case SAVEDATA_DELETE:
		// An existing save, even an empty one (utility/savedata/deletebroken).
		if (param_.GetFilenameCount() > 0) {
			yesnoChoice_ = 0;
			display_ = DS_DELETE_CONFIRM;
		} else {
			display_ = DS_DELETE_NODATA;
		}
		break;
	default:
		LOGE("UTIL", "Savedata mode %d not implemented", request_.mode);
		request_.common.result = 0;
		ChangeStatusInit(SAVEDATA_INIT_DELAY_US);
		display_ = DS_NONE;
		return 0;
	}

	if (retval < 0) {
		ChangeStatusShutdown(SAVEDATA_SHUTDOWN_DELAY_US);
	} else {
		ChangeStatusInit(SAVEDATA_INIT_DELAY_US);
	}
	InitCommon();
	UpdateButtons();
	StartFade(true);
	ResetAsk();
	actionDone_ = false;
	return retval;
}

std::string SaveDialog::GetSelectedSaveDirName() const {
	switch (request_.mode) {
	case SAVEDATA_LOAD:
	case SAVEDATA_AUTOLOAD:
	case SAVEDATA_SAVE:
	case SAVEDATA_AUTOSAVE:
	case SAVEDATA_AUTODELETE:
	case SAVEDATA_DELETE:
	case SAVEDATA_MAKEDATASECURE:
	case SAVEDATA_MAKEDATA:
	case SAVEDATA_READDATASECURE:
	case SAVEDATA_READDATA:
	case SAVEDATA_WRITEDATASECURE:
	case SAVEDATA_WRITEDATA:
	case SAVEDATA_ERASESECURE:
	case SAVEDATA_ERASE:
	case SAVEDATA_DELETEDATA:
		return param_.GetSaveDirName(param_.GetPspParam());
	default:
		return param_.GetSaveDirName(param_.GetPspParam(), currentSelectedSave_);
	}
}

int SaveDialog::Ask(const std::string &text, bool yesNo, bool defaultYes) {
	if (Headless()) {
		if (++askFrames_ < HEADLESS_ANSWER_UPDATES) return -1;
		askFrames_ = 0;
		LOGI("UTIL", "Savedata: \"%s\" -> %s (headless)", text.c_str(), defaultYes ? "yes" : "no");
		return defaultYes ? 1 : 0;
	}
	if (!prompt_) prompt_ = prompt::ShowMessage("PSP savedata", text, yesNo ? prompt::Buttons::YesNo : prompt::Buttons::Ok);
	int a = prompt_->answer.load();
	if (a == prompt::PENDING) return -1;
	prompt_.reset();
	return (a == prompt::YES || a == prompt::OK) ? 1 : 0;
}

int SaveDialog::Update(int animSpeed) {
	if (GetStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	if (!param_.GetPspParam()) {
		ChangeStatusShutdown(SAVEDATA_SHUTDOWN_DELAY_US);
		return 0;
	}
	if (pendingStatus_ != STATUS_RUNNING) {
		// Done, just waiting to tell the game.
		return 0;
	}

	// The game may have changed the request ("Where Is My Heart?"): reload it.
	u32 size = std::min<u32>((u32)sizeof(originalRequest_), GRead32(requestAddr_));
	if (mem::IsValidRange(requestAddr_, size) && memcmp(GPtr(requestAddr_), &originalRequest_, size) != 0) {
		memset(&request_, 0, sizeof(request_));
		mem::ReadBytes(&request_, requestAddr_, size);
		memcpy(&originalRequest_, &request_, size);
		param_.SetPspParam(&request_);
	}

	UpdateButtons();
	UpdateFade(animSpeed);
	UpdateCommon();

	SavedataRequest *p = param_.GetPspParam();
	const bool fading = IsFadingOut();
	auto finish = [&](s32 result, bool setResult) {
		if (setResult) p->common.result = result;
		ResetAsk();
		StartFade(false);
	};
	const std::string slot = param_.GetFileInfo(currentSelectedSave_).saveName;
	int a;

	switch (display_) {
	case DS_SAVE_LIST_CHOICE:
		if (fading) break;
		a = Ask("Save to slot \"" + slot + "\"?", true, !actionDone_);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			if (param_.GetFileInfo(currentSelectedSave_).size > 0) {
				yesnoChoice_ = 0;
				display_ = DS_SAVE_CONFIRM_OVERWRITE;
			} else {
				display_ = DS_SAVE_SAVING;
				StartIO();
			}
		}
		break;
	case DS_SAVE_CONFIRM:
		if (fading) break;
		a = Ask("Do you want to save this data?", true, true);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			display_ = DS_SAVE_SAVING;
			StartIO();
		}
		break;
	case DS_SAVE_CONFIRM_OVERWRITE:
		if (fading) break;
		a = Ask("Do you want to overwrite the data?", true, true);
		if (a == 0) {
			if (p->mode != SAVEDATA_SAVE) {
				display_ = DS_SAVE_LIST_CHOICE;
			} else {
				finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
			}
		} else if (a == 1) {
			display_ = DS_SAVE_SAVING;
			StartIO();
		}
		break;
	case DS_SAVE_SAVING:
		FinishIO();
		break;
	case DS_SAVE_FAILED:
		if (fading) break;
		LOGE("UTIL", "Savedata: unable to save");
		finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		break;
	case DS_SAVE_DONE:
		if (fading) break;
		param_.SetSelectedSave(param_.GetFileInfo(currentSelectedSave_).idx);
		finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS, true);
		break;

	case DS_LOAD_LIST_CHOICE:
		if (fading) break;
		a = Ask("Load save \"" + slot + "\"?", true, !actionDone_);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			display_ = DS_LOAD_LOADING;
			StartIO();
		}
		break;
	case DS_LOAD_CONFIRM:
		if (fading) break;
		a = Ask("Load this data?", true, true);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			display_ = DS_LOAD_LOADING;
			StartIO();
		}
		break;
	case DS_LOAD_LOADING:
		FinishIO();
		break;
	case DS_LOAD_FAILED:
		if (fading) break;
		LOGE("UTIL", "Savedata: load failed (%08x)", p->common.result);
		finish(0, false);
		break;
	case DS_LOAD_DONE:
		if (fading) break;
		param_.SetSelectedSave(param_.GetFileInfo(currentSelectedSave_).idx);
		finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS, true);
		break;
	case DS_LOAD_NODATA:
		if (fading) break;
		if (Ask("There is no data.", false, true) >= 0) finish((s32)SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA, true);
		break;

	case DS_DELETE_LIST_CHOICE:
		if (fading) break;
		if (param_.GetFilenameCount() == 0 && !Headless()) {
			display_ = DS_DELETE_NODATA;
			break;
		}
		// Headless answers like the player who recorded pspautotests: yes once, then back out.
		a = Ask("Delete save \"" + slot + "\"?", true, !actionDone_);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			yesnoChoice_ = 0;
			display_ = DS_DELETE_CONFIRM;
		}
		break;
	case DS_DELETE_CONFIRM:
		if (fading) break;
		a = Ask("This save data will be deleted.\nAre you sure you want to continue?", true, true);
		if (a == 0) {
			if (p->mode == SAVEDATA_LISTDELETE || p->mode == SAVEDATA_LISTALLDELETE) {
				display_ = DS_DELETE_LIST_CHOICE;
			} else {
				finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
			}
		} else if (a == 1) {
			display_ = DS_DELETE_DELETING;
			StartIO();
		}
		break;
	case DS_DELETE_DELETING:
		FinishIO();
		break;
	case DS_DELETE_FAILED:
		if (fading) break;
		finish(0, false);
		break;
	case DS_DELETE_DONE:
		if (fading) break;
		if (p->mode == SAVEDATA_DELETE) {
			// The single save is gone: success, not "no data" (utility/savedata/deletedata).
			finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS, true);
		} else if (p->mode == SAVEDATA_LISTALLDELETE) {
			// Back to the list of every save on the memory stick (which on a PSP is never empty).
			if (currentSelectedSave_ > param_.GetFilenameCount() - 1) currentSelectedSave_ = std::max(0, param_.GetFilenameCount() - 1);
			display_ = DS_DELETE_LIST_CHOICE;
		} else if (param_.GetFilenameCount() == 0) {
			display_ = DS_DELETE_NODATA;
		} else if (p->mode == SAVEDATA_LISTDELETE) {
			if (currentSelectedSave_ > param_.GetFilenameCount() - 1) currentSelectedSave_ = param_.GetFilenameCount() - 1;
			display_ = DS_DELETE_LIST_CHOICE;
		} else {
			finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS, true);
		}
		break;
	case DS_DELETE_NODATA:
		if (fading) break;
		if (Ask("There is no data.", false, true) >= 0) finish((s32)SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA, true);
		break;

	case DS_NONE:  // the modes that show nothing
		if (ioStatus_ == IO_NONE) {
			StartIO();
		} else if (FinishIO()) {
			ChangeStatus(STATUS_FINISHED, 0);
		}
		break;
	default:
		ChangeStatus(STATUS_FINISHED, 0);
		break;
	}

	if (ReadStatus() == STATUS_FINISHED || pendingStatus_ == STATUS_FINISHED)
		mem::WriteBytes(requestAddr_, &request_, request_.common.size);
	return 0;
}

void SaveDialog::StartIO() {
	ioAction_ = display_;
	ioRequestStart_ = request_;
	ioSaveId_ = currentSelectedSave_;
	ioSaveDirName_ = GetSelectedSaveDirName();
	ioListSaveDirName_.clear();
	const s32 mode = request_.mode;
	if (display_ == DS_NONE && mode == SAVEDATA_FILES) ioListSaveDirName_ = param_.GetSaveDirName(&request_, 0);
	ioDeleteDir_.clear();
	if (display_ == DS_DELETE_DELETING) {
		ioDeleteDir_ = param_.GetSaveDir(currentSelectedSave_);
	} else if (display_ == DS_NONE && (mode == SAVEDATA_DELETEDATA || mode == SAVEDATA_AUTODELETE || mode == SAVEDATA_DELETE)) {
		ioDeleteDir_ = param_.GetSaveDir(param_.GetSelectedSave());
	}
	ioStatus_ = IO_PENDING;
	// PPSSPP runs this on a host thread; doing it right away keeps the same Update timing (the
	// results are taken by the next Update).
	ExecuteIOAction();
}

void SaveDialog::ExecuteIOAction() {
	Savedata io;
	SavedataRequest req = ioRequestStart_;
	s32 &result = req.common.result;
	ioDisplay_ = ioAction_;
	switch (ioAction_) {
	case DS_LOAD_LOADING:
		result = io.Load(&req, ioSaveDirName_, ioSaveId_);
		ioDisplay_ = result == 0 ? DS_LOAD_DONE : DS_LOAD_FAILED;
		break;
	case DS_SAVE_SAVING:
		ioDisplay_ = io.Save(&req, ioSaveDirName_) == 0 ? DS_SAVE_DONE : DS_SAVE_FAILED;
		break;
	case DS_DELETE_DELETING:
		if (io.Delete(&req, ioDeleteDir_)) {
			result = 0;
			ioDisplay_ = DS_DELETE_DONE;
		} else {
			ioDisplay_ = DS_DELETE_FAILED;
		}
		break;
	case DS_NONE: {
		switch (req.mode) {
		case SAVEDATA_LOAD:
		case SAVEDATA_AUTOLOAD:
			result = io.Load(&req, ioSaveDirName_, ioSaveId_);
			break;
		case SAVEDATA_SAVE:
		case SAVEDATA_AUTOSAVE:
			result = io.Save(&req, ioSaveDirName_);
			break;
		case SAVEDATA_SIZES:
			result = io.GetSizes(&req);
			break;
		case SAVEDATA_LIST:
			io.GetList(&req);
			result = 0;
			break;
		case SAVEDATA_FILES:
			result = io.GetFilesList(&req, requestAddr_, ioListSaveDirName_);
			break;
		case SAVEDATA_GETSIZE:
			result = io.GetSize(&req) ? 0 : (s32)SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
			break;
		case SAVEDATA_DELETEDATA:
			result = io.Delete(&req, ioDeleteDir_) ? 0 : (s32)SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
			break;
		case SAVEDATA_AUTODELETE:
		case SAVEDATA_DELETE:
			result = io.Delete(&req, ioDeleteDir_) ? 0 : (s32)SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA;
			break;
		case SAVEDATA_MAKEDATA:
		case SAVEDATA_MAKEDATASECURE:
			result = io.Save(&req, ioSaveDirName_, req.mode == SAVEDATA_MAKEDATASECURE);
			if (result == (s32)SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE) result = (s32)SCE_UTILITY_SAVEDATA_ERROR_RW_MEMSTICK_FULL;
			break;
		case SAVEDATA_WRITEDATA:
		case SAVEDATA_WRITEDATASECURE:
			result = io.Save(&req, ioSaveDirName_, req.mode == SAVEDATA_WRITEDATASECURE);
			break;
		case SAVEDATA_READDATA:
		case SAVEDATA_READDATASECURE:
			result = io.Load(&req, ioSaveDirName_, ioSaveId_, req.mode == SAVEDATA_READDATASECURE);
			if (result == (s32)SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN) result = (s32)SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN;
			if (result == (s32)SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA) result = (s32)SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
			break;
		case SAVEDATA_ERASE:
		case SAVEDATA_ERASESECURE:
			result = io.DeleteData(&req);
			break;
		default:
			break;
		}
		break;
	}
	default:
		break;
	}
	io.MoveLateWritesTo(param_);
	ioResult_ = req;
	ioStatus_ = IO_READY;
}

bool SaveDialog::FinishIO() {
	if (ioStatus_ != IO_READY) return ioStatus_ != IO_PENDING;
	// Only what the IO changed: the game may have changed its request meanwhile.
	const u8 *before = (const u8 *)&ioRequestStart_;
	const u8 *after = (const u8 *)&ioResult_;
	u8 *live = (u8 *)&request_;
	for (size_t i = 0; i < sizeof(request_); ++i) {
		if (after[i] != before[i]) live[i] = after[i];
	}
	param_.ApplyLateWrites();
	if (ioAction_ != DS_NONE) {
		actionDone_ = true;
		display_ = ioDisplay_;
		// Show what was saved or deleted in the list.
		if (display_ == DS_SAVE_DONE || display_ == DS_DELETE_DONE) param_.SetPspParam(param_.GetPspParam());
	}
	ioStatus_ = IO_DONE;
	return true;
}

int SaveDialog::Shutdown(bool force) {
	if (GetStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	ioStatus_ = IO_NONE;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(SAVEDATA_SHUTDOWN_DELAY_US);
	param_.SetPspParam(nullptr);
	ResetAsk();
	return 0;
}

}  // namespace utility
