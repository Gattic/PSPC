// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPSaveDialog.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/dialogs/save_dialog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/dialogs/save_fs.h"
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

// With overlays: the IO is done at once, but "Saving / Please wait..." stays up at least this
// many Updates, so the player sees it.
constexpr int MIN_PROGRESS_UPDATES = 30;

constexpr float FONT_SCALE = 0.55f;

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
	upFramesHeld_ = downFramesHeld_ = 0;
	icons_.clear();
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

int SaveDialog::Ask(const std::string &text, bool defaultYes) {
	if (++askFrames_ < HEADLESS_ANSWER_UPDATES) return -1;
	askFrames_ = 0;
	LOGI("UTIL", "Savedata: \"%s\" -> %s (headless)", text.c_str(), defaultYes ? "yes" : "no");
	return defaultYes ? 1 : 0;
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

	if (Headless()) {
		UpdateAuto();
	} else {
		UpdateInteractive();
	}

	if (ReadStatus() == STATUS_FINISHED || pendingStatus_ == STATUS_FINISHED)
		mem::WriteBytes(requestAddr_, &request_, request_.common.size);
	return 0;
}

void SaveDialog::UpdateAuto() {
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
		a = Ask("Save to slot \"" + slot + "\"?", !actionDone_);
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
		a = Ask("Do you want to save this data?", true);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			display_ = DS_SAVE_SAVING;
			StartIO();
		}
		break;
	case DS_SAVE_CONFIRM_OVERWRITE:
		if (fading) break;
		a = Ask("Do you want to overwrite the data?", true);
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
		a = Ask("Load save \"" + slot + "\"?", !actionDone_);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			display_ = DS_LOAD_LOADING;
			StartIO();
		}
		break;
	case DS_LOAD_CONFIRM:
		if (fading) break;
		a = Ask("Load this data?", true);
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
		if (Ask("There is no data.", true) >= 0) finish((s32)SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA, true);
		break;

	case DS_DELETE_LIST_CHOICE:
		if (fading) break;
		// Headless answers like the player who recorded pspautotests: yes once, then back out.
		a = Ask("Delete save \"" + slot + "\"?", !actionDone_);
		if (a == 0) {
			finish(SCE_UTILITY_DIALOG_RESULT_CANCEL, true);
		} else if (a == 1) {
			yesnoChoice_ = 0;
			display_ = DS_DELETE_CONFIRM;
		}
		break;
	case DS_DELETE_CONFIRM:
		if (fading) break;
		a = Ask("This save data will be deleted.\nAre you sure you want to continue?", true);
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
		if (Ask("There is no data.", true) >= 0) finish((s32)SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA, true);
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
}

// ---------------------------------------------------------------- with overlays

namespace {

std::string FixedString(const char *s, size_t maxLen) { return std::string(s, strnlen(s, maxLen)); }

std::string FormatHourMin(int hour, int minute) {
	char buf[32];
	if (SystemTimeFormat() == 1) {
		const char *amPm = hour >= 12 ? "PM" : "AM";
		int h = hour % 12;
		if (h == 0) h = 12;
		snprintf(buf, sizeof(buf), "%02d:%02d %s", h, minute, amPm);
	} else {
		snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
	}
	return buf;
}

std::string FormatDate(int year, int month, int day) {
	char buf[32];
	switch (SystemDateFormat()) {
	case 1: snprintf(buf, sizeof(buf), "%02d/%02d/%04d", month, day, year); break;
	case 2: snprintf(buf, sizeof(buf), "%02d/%02d/%04d", day, month, year); break;
	default: snprintf(buf, sizeof(buf), "%04d/%02d/%02d", year, month, day); break;
	}
	return buf;
}

}  // namespace

void SaveDialog::Finish(s32 result) {
	param_.GetPspParam()->common.result = result;
	StartFade(false);
}

std::shared_ptr<const ppge::Texture> SaveDialog::SaveIcon(int idx) {
	const SaveFileInfo &info = param_.GetFileInfo(idx);
	std::string key;
	std::vector<u8> png;
	if (info.size > 0 && !info.saveDir.empty()) {
		key = std::string(Savedata::kSavePath) + info.saveDir + "/ICON0.PNG";
		auto it = icons_.find(key);
		if (it != icons_.end()) return it->second;
		savefs::ReadAll(key, png);
	} else {
		// A slot without data shows the icon of the data the game wants to save.
		const SavedataFileData &f = param_.GetPspParam()->icon0FileData;
		const u32 size = f.size ? f.size : f.bufSize;
		char buf[48];
		snprintf(buf, sizeof(buf), "new:%08x:%08x", f.buf, size);
		key = buf;
		auto it = icons_.find(key);
		if (it != icons_.end()) return it->second;
		if (f.buf && size && size < 0x100000 && mem::IsValidRange(f.buf, size)) {
			png.resize(size);
			mem::ReadBytes(png.data(), f.buf, size);
		}
	}
	auto tex = png.empty() ? nullptr : ppge::DecodeTexture(png.data(), png.size());
	icons_[key] = tex;
	return tex;
}

void SaveDialog::DisplayBanner(Banner which) {
	ppge::DrawRect(0, 0, 480, 23, CalcFadedColor(0x65636358));
	ppge::Style textStyle = FadedStyle(ppge::BOX_VCENTER, 0.6f);
	textStyle.hasShadow = false;
	const char *title = which == DB_SAVE ? "Save" : which == DB_LOAD ? "Load" : which == DB_DELETE ? "Delete" : "";
	ppge::DrawIcon(ppge::Icon::Hexagon, 10, 6, 12.0f, 12.0f, FadedImageStyle());
	ppge::DrawString(title, 30, 11, textStyle);
}

void SaveDialog::DisplaySaveList(bool canMove) {
	for (int displayCount = 0; displayCount < param_.GetFilenameCount(); displayCount++) {
		u32 color = CalcFadedColor(0xFFFFFFFF);
		const SaveFileInfo &fileInfo = param_.GetFileInfo(displayCount);
		auto texture = SaveIcon(displayCount);
		if (fileInfo.size == 0 && texture) color = CalcFadedColor(0xFF777777);

		// The save's place on screen.
		float w, h, x;
		float y = 97;
		if (displayCount != currentSelectedSave_) {
			w = 81;
			h = 45;
			x = 58.5f;
		} else {
			w = 144;
			h = 80;
			x = 27;
		}
		if (displayCount < currentSelectedSave_)
			y -= 13 + 45 * (currentSelectedSave_ - displayCount);
		else if (displayCount > currentSelectedSave_)
			y += 48 + 45 * (displayCount - currentSelectedSave_);
		// Skip if it's well outside the screen.
		if (y > 472.0f || y < -200.0f) continue;

		float pad = 0;
		if (texture) {
			const float scale = h / (float)texture->height;
			const float scaledW = (float)(int)(texture->width * scale);
			pad = (float)(int)((w - scaledW) / 2);
			w = scaledW;
			// The PSP draws save icons over black, so their transparent parts come out black.
			ppge::DrawRect(x + pad, y, x + pad + w, y + h, CalcFadedColor(0xFF000000));
			ppge::DrawTexture(texture, x + pad, y, w, h, color);
		} else {
			ppge::DrawRect(x, y, x + w, y + h, CalcFadedColor(0x88666666));
		}
		if (displayCount == currentSelectedSave_) {
			const float b = 1.2f;
			const u32 bc = CalcFadedColor(0xD0FFFFFF);
			ppge::DrawRect(x + pad - b, y - b, x + pad + w + b, y, bc);      // top
			ppge::DrawRect(x + pad - b, y, x + pad, y + h, bc);              // left
			ppge::DrawRect(x + pad - b, y + h, x + pad + w + b, y + h + b, bc);  // bottom
			ppge::DrawRect(x + pad + w, y, x + pad + w + b, y + h, bc);      // right
		}
	}

	if (canMove) {
		if ((IsButtonPressed(CTRL_UP) || IsButtonHeld(CTRL_UP, upFramesHeld_)) && currentSelectedSave_ > 0)
			currentSelectedSave_--;
		else if ((IsButtonPressed(CTRL_DOWN) || IsButtonHeld(CTRL_DOWN, downFramesHeld_)) &&
		         currentSelectedSave_ < (param_.GetFilenameCount() - 1))
			currentSelectedSave_++;
	}
}

void SaveDialog::DisplaySaveIcon(bool checkExists) {
	const SaveFileInfo &curSave = param_.GetFileInfo(currentSelectedSave_);
	u32 color = CalcFadedColor(0xFFFFFFFF);
	if (curSave.size == 0 && checkExists) color = CalcFadedColor(0xFF777777);

	float w = 144, h = 80, x = 27, y = 97;
	auto texture = SaveIcon(currentSelectedSave_);
	if (texture) {
		const float scale = h / (float)texture->height;
		const float scaledW = (float)(int)(texture->width * scale);
		x += (float)(int)((w - scaledW) / 2);
		w = scaledW;
		// Over black, as in DisplaySaveList.
		ppge::DrawRect(x, y, x + w, y + h, CalcFadedColor(0xFF000000));
		ppge::DrawTexture(texture, x, y, w, h, color);
	} else {
		// No icon: a plain box, as PPSSPP draws it.
		ppge::DrawRect(x, y, x + w, y + h, color);
	}
}

void SaveDialog::DisplaySaveDataInfo1() {
	const SaveFileInfo &saveInfo = param_.GetFileInfo(currentSelectedSave_);
	const ppge::Style saveTitleStyle = FadedStyle(ppge::BOX_LEFT, 0.55f);
	if (saveInfo.broken) {
		ppge::DrawString("Corrupted Data", 180, 136, FadedStyle(ppge::BOX_VCENTER, 0.6f));
		ppge::DrawString(FixedString(saveInfo.title, sizeof(saveInfo.title)), 175, 159, saveTitleStyle);
	} else if (saveInfo.size == 0) {
		ppge::DrawString("NEW DATA", 180, 136, FadedStyle(ppge::BOX_VCENTER, 0.6f));
	} else {
		const vfs::ScePspDateTime &t = saveInfo.modifTime;
		char timeTxt[96];
		snprintf(timeTxt, sizeof(timeTxt), "%s   %s  %lld KB", FormatDate(t.year, t.month, t.day).c_str(),
		         FormatHourMin(t.hour, t.minute).c_str(), (long long)(saveInfo.size / 1024));

		ppge::DrawRect(180, 136, 480, 137, CalcFadedColor(0xFFFFFFFF));
		ppge::Style titleStyle = FadedStyle(ppge::BOX_BOTTOM, 0.6f);
		titleStyle.color = CalcFadedColor(0xFFC0C0C0);
		const ppge::Style textStyle = FadedStyle(ppge::BOX_LEFT, 0.5f);

		ppge::DrawString(FixedString(saveInfo.title, sizeof(saveInfo.title)), 180, 136, titleStyle);
		ppge::DrawString(timeTxt, 180, 137, textStyle);
		ppge::DrawString(FixedString(saveInfo.saveTitle, sizeof(saveInfo.saveTitle)), 175, 159, saveTitleStyle);
		ppge::DrawStringWrapped(FixedString(saveInfo.saveDetail, sizeof(saveInfo.saveDetail)), 175, 181, 480 - 175, 250 - 181, textStyle);
	}
}

void SaveDialog::DisplaySaveDataInfo2(bool showNewData) {
	const SaveFileInfo &info = param_.GetFileInfo(currentSelectedSave_);
	std::string saveTitle;
	s64 dataSize;
	int year, month, day, hour, minute;
	if (showNewData || info.size == 0) {
		time_t now = time(nullptr);
		tm t{};
		localtime_s(&t, &now);
		year = t.tm_year + 1900, month = t.tm_mon + 1, day = t.tm_mday, hour = t.tm_hour, minute = t.tm_min;
		saveTitle = FixedString(param_.GetPspParam()->sfoParam.savedataTitle, sizeof(param_.GetPspParam()->sfoParam.savedataTitle));
		dataSize = param_.GetPspParam()->dataSize;
	} else {
		const vfs::ScePspDateTime &t = info.modifTime;
		year = t.year, month = t.month, day = t.day, hour = t.hour, minute = t.minute;
		saveTitle = FixedString(info.saveTitle, sizeof(info.saveTitle));
		dataSize = info.size;
	}
	const std::string text = saveTitle + "\n" + FormatDate(year, month, day) + "  " + FormatHourMin(hour, minute) + "\n" +
	                         std::to_string((long long)(dataSize / 1024)) + " KB";
	ppge::DrawString(text, 8, 200, FadedStyle(ppge::BOX_LEFT, 0.5f));
}

void SaveDialog::DisplayMessage(std::string_view text, bool hasYesNo) {
	const ppge::Style textStyle = FadedStyle(ppge::BOX_CENTER, FONT_SCALE);
	const float WRAP_WIDTH = 254.0f;
	float y = 136.0f, h;
	ppge::MeasureString(nullptr, &h, text, FONT_SCALE, ppge::LINE_WRAP_WORD, WRAP_WIDTH);
	float h2 = h / 2.0f;
	if (hasYesNo) {
		const char *choiceText;
		float x, w;
		if (yesnoChoice_ == 1) {
			choiceText = "Yes";
			x = 302.0f;
		} else {
			choiceText = "No";
			x = 366.0f;
		}
		ppge::MeasureString(&w, &h, choiceText, FONT_SCALE);
		w = w / 2.0f + 5.5f;
		h /= 2.0f;
		const float y2 = y + h2 + 4.0f;
		h2 += h + 4.0f;
		y = 132.0f - h;
		ppge::DrawRect(x - w, y2 - h, x + w, y2 + h, CalcFadedColor(0x40C0C0C0));
		ppge::DrawString("Yes", 302.0f, y2, textStyle);
		ppge::DrawString("No", 366.0f, y2, textStyle);
		if (IsButtonPressed(CTRL_LEFT) && yesnoChoice_ == 0) {
			yesnoChoice_ = 1;
		} else if (IsButtonPressed(CTRL_RIGHT) && yesnoChoice_ == 1) {
			yesnoChoice_ = 0;
		}
	}
	ppge::DrawStringWrapped(text, 334.0f, y, WRAP_WIDTH, 0, textStyle);
	const float sy = 122.0f - h2, ey = 150.0f + h2;
	ppge::DrawRect(202.0f, sy, 466.0f, sy + 1.0f, CalcFadedColor(0xFFFFFFFF));
	ppge::DrawRect(202.0f, ey, 466.0f, ey + 1.0f, CalcFadedColor(0xFFFFFFFF));
}

void SaveDialog::UpdateInteractive() {
	SavedataRequest *p = param_.GetPspParam();
	// The IO is done by now; the progress screens stay up a moment before showing the result.
	auto progress = [&]() {
		if (++ioShownUpdates_ >= MIN_PROGRESS_UPDATES) FinishIO();
	};
	const bool okPressed = IsButtonPressed(okButtonFlag_), cancelPressed = IsButtonPressed(cancelButtonFlag_);
	const bool listMode = p->mode == SAVEDATA_LISTDELETE || p->mode == SAVEDATA_LISTALLDELETE;

	switch (display_) {
	case DS_SAVE_LIST_CHOICE:
		StartDraw();
		DisplaySaveList();
		DisplaySaveDataInfo1();
		DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
		DisplayBanner(DB_SAVE);
		if (cancelPressed) {
			Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
		} else if (okPressed) {
			// An existing save: ask first.
			if (param_.GetFileInfo(currentSelectedSave_).size > 0) {
				yesnoChoice_ = 0;
				display_ = DS_SAVE_CONFIRM_OVERWRITE;
			} else {
				display_ = DS_SAVE_SAVING;
				StartIO();
			}
		}
		EndDraw();
		break;
	case DS_SAVE_CONFIRM:
		StartDraw();
		DisplaySaveIcon(false);
		DisplaySaveDataInfo2(true);
		DisplayMessage("Do you want to save this data?", true);
		DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
		DisplayBanner(DB_SAVE);
		if (cancelPressed || (okPressed && yesnoChoice_ == 0)) {
			Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
		} else if (okPressed) {
			display_ = DS_SAVE_SAVING;
			StartIO();
		}
		EndDraw();
		break;
	case DS_SAVE_CONFIRM_OVERWRITE:
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2();
		DisplayMessage("Do you want to overwrite the data?", true);
		DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
		DisplayBanner(DB_SAVE);
		if (cancelPressed || (okPressed && yesnoChoice_ == 0)) {
			if (p->mode != SAVEDATA_SAVE) {
				display_ = DS_SAVE_LIST_CHOICE;
			} else {
				Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
			}
		} else if (okPressed) {
			display_ = DS_SAVE_SAVING;
			StartIO();
		}
		EndDraw();
		break;
	case DS_SAVE_SAVING:
		progress();
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2(true);
		DisplayMessage("Saving\nPlease Wait...");
		DisplayBanner(DB_SAVE);
		EndDraw();
		break;
	case DS_SAVE_FAILED:
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2(true);
		DisplayMessage("Unable to save data.");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_SAVE);
		if (cancelPressed) {
			LOGE("UTIL", "Savedata: unable to save");
			// Back to the list to try again.
			if (p->mode != SAVEDATA_SAVE) {
				display_ = DS_SAVE_LIST_CHOICE;
			} else {
				Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
			}
		}
		EndDraw();
		break;
	case DS_SAVE_DONE:
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2();
		DisplayMessage("Save completed");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_SAVE);
		if (cancelPressed) {
			// The save autosave and autoload use.
			param_.SetSelectedSave(param_.GetFileInfo(currentSelectedSave_).idx);
			Finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS);
		}
		EndDraw();
		break;

	case DS_LOAD_LIST_CHOICE:
		StartDraw();
		DisplaySaveList();
		DisplaySaveDataInfo1();
		DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
		DisplayBanner(DB_LOAD);
		if (cancelPressed) {
			Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
		} else if (okPressed) {
			display_ = DS_LOAD_LOADING;
			StartIO();
		}
		EndDraw();
		break;
	case DS_LOAD_CONFIRM:
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2();
		DisplayMessage("Load this data?", true);
		DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
		DisplayBanner(DB_LOAD);
		if (cancelPressed || (okPressed && yesnoChoice_ == 0)) {
			Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
		} else if (okPressed) {
			display_ = DS_LOAD_LOADING;
			StartIO();
		}
		EndDraw();
		break;
	case DS_LOAD_LOADING:
		progress();
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2();
		DisplayMessage("Loading\nPlease Wait...");
		DisplayBanner(DB_LOAD);
		EndDraw();
		break;
	case DS_LOAD_FAILED:
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2();
		DisplayMessage("Load failed\nThe data is corrupted.");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_LOAD);
		if (cancelPressed) {
			LOGE("UTIL", "Savedata: load failed (%08x)", p->common.result);
			// Back to the list to try again; the error stays the result otherwise.
			if (p->mode != SAVEDATA_LOAD) {
				display_ = DS_LOAD_LIST_CHOICE;
			} else {
				StartFade(false);
			}
		}
		EndDraw();
		break;
	case DS_LOAD_DONE:
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2();
		DisplayMessage("Load completed");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_LOAD);
		// The PSP only takes back here; PPSSPP takes OK as well, which gets players into the game
		// sooner.
		if (cancelPressed || okPressed) {
			param_.SetSelectedSave(param_.GetFileInfo(currentSelectedSave_).idx);
			Finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS);
		}
		EndDraw();
		break;
	case DS_LOAD_NODATA:
		StartDraw();
		DisplayMessage("There is no data");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_LOAD);
		if (cancelPressed) Finish((s32)SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA);
		EndDraw();
		break;

	case DS_DELETE_LIST_CHOICE:
		StartDraw();
		DisplaySaveList();
		DisplaySaveDataInfo1();
		DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
		DisplayBanner(DB_DELETE);
		if (cancelPressed) {
			Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
		} else if (okPressed && param_.GetFilenameCount() > 0) {
			yesnoChoice_ = 0;
			display_ = DS_DELETE_CONFIRM;
		}
		EndDraw();
		break;
	case DS_DELETE_CONFIRM:
		StartDraw();
		DisplaySaveIcon(true);
		DisplaySaveDataInfo2();
		DisplayMessage("This save data will be deleted.\nAre you sure you want to continue?", true);
		DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
		DisplayBanner(DB_DELETE);
		if (cancelPressed || (okPressed && yesnoChoice_ == 0)) {
			if (listMode) {
				display_ = DS_DELETE_LIST_CHOICE;
			} else {
				Finish(SCE_UTILITY_DIALOG_RESULT_CANCEL);
			}
		} else if (okPressed) {
			display_ = DS_DELETE_DELETING;
			StartIO();
		}
		EndDraw();
		break;
	case DS_DELETE_DELETING:
		progress();
		StartDraw();
		DisplayMessage("Deleting\nPlease Wait...");
		DisplayBanner(DB_DELETE);
		EndDraw();
		break;
	case DS_DELETE_FAILED:
		StartDraw();
		DisplayMessage("Unable to delete data.");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_DELETE);
		if (cancelPressed) {
			if (listMode) {
				display_ = DS_DELETE_LIST_CHOICE;
			} else {
				StartFade(false);
			}
		}
		EndDraw();
		break;
	case DS_DELETE_DONE:
		StartDraw();
		DisplayMessage("Delete completed");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_DELETE);
		if (cancelPressed) {
			if (p->mode == SAVEDATA_DELETE) {
				// The single save is gone: success, not "no data" (utility/savedata/deletedata).
				Finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS);
			} else if (p->mode == SAVEDATA_LISTALLDELETE) {
				// Back to the list of every save on the memory stick.
				currentSelectedSave_ = std::max(0, std::min(currentSelectedSave_, param_.GetFilenameCount() - 1));
				display_ = DS_DELETE_LIST_CHOICE;
			} else if (param_.GetFilenameCount() == 0) {
				display_ = DS_DELETE_NODATA;
			} else if (p->mode == SAVEDATA_LISTDELETE) {
				currentSelectedSave_ = std::min(currentSelectedSave_, param_.GetFilenameCount() - 1);
				display_ = DS_DELETE_LIST_CHOICE;
			} else {
				Finish(SCE_UTILITY_DIALOG_RESULT_SUCCESS);
			}
		}
		EndDraw();
		break;
	case DS_DELETE_NODATA:
		StartDraw();
		DisplayMessage("There is no data");
		DisplayButtons(DS_BUTTON_CANCEL);
		DisplayBanner(DB_DELETE);
		if (cancelPressed) Finish((s32)SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA);
		EndDraw();
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
	ioShownUpdates_ = 0;
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
