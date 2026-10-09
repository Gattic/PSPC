// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPNetconfDialog.cpp, PSPNpSigninDialog.cpp,
// PSPScreenshotDialog.cpp, PSPGamedataInstallDialog.cpp, PSPHtmlViewerDialog.cpp,
// PSPPlaceholderDialog.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include "runtime/dialogs/simple_dialogs.h"

#include <algorithm>
#include <cstring>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "fs/param_sfo.h"
#include "runtime/core/memory.h"
#include "runtime/dialogs/save_fs.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/sysmem.h"

namespace utility {

namespace {

std::string FixedString(const char *s, size_t maxLen) { return std::string(s, strnlen(s, maxLen)); }

// Copies min(size field, sizeof(T)) bytes of a variable-sized request.
template <typename T>
bool ReadRequest(u32 addr, T *out) {
	memset(out, 0, sizeof(T));
	if (!mem::IsValidRange(addr, 4)) return false;
	u32 size = std::min<u32>(GRead32(addr), sizeof(T));
	if (!mem::IsValidRange(addr, size)) return false;
	mem::ReadBytes(out, addr, size);
	return true;
}

// Headless answers come after this many Updates.
constexpr int HEADLESS_ANSWER_UPDATES = 30;

}  // namespace

// ---------------------------------------------------------------- Netconf

namespace {
constexpr int NET_INIT_DELAY_US = 200000;
constexpr int NET_SHUTDOWN_DELAY_US = 200000;
}  // namespace

int NetconfDialog::Init(u32 paramAddr) {
	if (ReadStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {0x38, 0x40, 0x44});
	if (check < 0) return check;
	if (!ReadRequest(paramAddr, &request_)) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	requestAddr_ = paramAddr;
	LOGI("UTIL", "Netconf dialog (action %d): offline, the WLAN switch is off", request_.netAction);
	request_.common.result = 0;
	ChangeStatusInit(NET_INIT_DELAY_US);
	InitCommon();
	UpdateButtons();
	framesRun_ = 0;
	prompt_.reset();
	StartFade(true);
	return 0;
}

int NetconfDialog::Update(int animSpeed) {
	if (ReadStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	UpdateButtons();
	UpdateCommon();
	UpdateFade(animSpeed);
	framesRun_++;
	if (pendingStatus_ != STATUS_FINISHED) {
		// "The WLAN switch is off": the player can only back out.
		bool done = false;
		if (Headless()) {
			done = framesRun_ >= HEADLESS_ANSWER_UPDATES;
		} else {
			if (!prompt_)
				prompt_ = prompt::ShowMessage("Network connection",
				                              "A connection error has occurred.\nThe WLAN switch is off (PSPC runs offline).",
				                              prompt::Buttons::Ok);
			done = prompt_->answer.load() != prompt::PENDING || IsButtonPressed(cancelButtonFlag_);
		}
		if (done) {
			StartFade(false);
			ChangeStatus(STATUS_FINISHED, NET_SHUTDOWN_DELAY_US);
			request_.common.result = SCE_UTILITY_DIALOG_RESULT_ABORT;
		}
	}
	if (ReadStatus() == STATUS_FINISHED || pendingStatus_ == STATUS_FINISHED)
		mem::WriteBytes(requestAddr_, &request_, std::min<u32>(request_.common.size, sizeof(request_)));
	return 0;
}

int NetconfDialog::Shutdown(bool force) {
	if (ReadStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(NET_SHUTDOWN_DELAY_US);
	prompt_.reset();
	return 0;
}

// ---------------------------------------------------------------- NP sign-in

namespace {
constexpr int NP_INIT_DELAY_US = 200000;
constexpr int NP_SHUTDOWN_DELAY_US = 501000;
constexpr s32 NP_SIGNIN_STATUS_NONE = 0;
constexpr s32 NP_SIGNIN_STATUS_CANCELED = 3;
}  // namespace

int NpSigninDialog::Init(u32 paramAddr) {
	if (ReadStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {0x40});
	if (check < 0) return check;
	if (!ReadRequest(paramAddr, &request_)) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	requestAddr_ = paramAddr;
	// An output; Update only acts while it's NONE.
	request_.npSigninStatus = NP_SIGNIN_STATUS_NONE;
	ChangeStatusInit(NP_INIT_DELAY_US);
	UpdateButtons();
	InitCommon();
	StartFade(true);
	return 0;
}

int NpSigninDialog::Update(int animSpeed) {
	if (ReadStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	UpdateButtons();
	UpdateCommon();
	UpdateFade(animSpeed);
	if (request_.npSigninStatus == NP_SIGNIN_STATUS_NONE) {
		// Offline: signing in to the PlayStation Network can't succeed.
		LOGI("UTIL", "NP sign-in: offline, cancelled");
		StartFade(false);
		ChangeStatus(STATUS_FINISHED, NP_SHUTDOWN_DELAY_US);
		request_.common.result = SCE_UTILITY_DIALOG_RESULT_ABORT;
		request_.npSigninStatus = NP_SIGNIN_STATUS_CANCELED;
	}
	return 0;
}

int NpSigninDialog::Shutdown(bool force) {
	if (ReadStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(NP_SHUTDOWN_DELAY_US);
	if (mem::IsValidRange(requestAddr_, sizeof(request_)))
		mem::WriteBytes(requestAddr_, &request_, std::min<u32>(request_.common.size, sizeof(request_)));
	return 0;
}

// ---------------------------------------------------------------- Screenshot

namespace {
constexpr u32 SCREENSHOT_SIZE_V1 = 436, SCREENSHOT_SIZE_V2 = 928, SCREENSHOT_SIZE_V3 = 932;
constexpr int SCREENSHOT_TYPE_CONT_START = 100;
}  // namespace

DialogCommon *ScreenshotDialog::GetCommonParam() {
	if (!paramsAddr_ || !mem::IsValidRange(paramsAddr_, sizeof(DialogCommon))) return nullptr;
	return (DialogCommon *)GPtr(paramsAddr_);
}

int ScreenshotDialog::Init(u32 paramAddr) {
	if (ReadStatus() != STATUS_NONE && ReadStatus() != STATUS_SHUTDOWN) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {SCREENSHOT_SIZE_V1, SCREENSHOT_SIZE_V2, SCREENSHOT_SIZE_V3});
	if (check < 0) return check;
	paramsAddr_ = paramAddr;
	mode_ = (int)GRead32(paramAddr + sizeof(DialogCommon));
	ChangeStatus(STATUS_INITIALIZE, 0);
	InitCommon();
	return 0;
}

int ScreenshotDialog::Update(int animSpeed) {
	if (ReadStatus() == STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	UpdateCommon();
	if (ReadStatus() == STATUS_INITIALIZE) {
		ChangeStatus(STATUS_RUNNING, 0);
	} else if (ReadStatus() == STATUS_RUNNING) {
		ChangeStatus(mode_ == SCREENSHOT_TYPE_CONT_START ? STATUS_SCREENSHOT_UNKNOWN : STATUS_FINISHED, 0);
	} else if (ReadStatus() == STATUS_FINISHED) {
		ChangeStatus(STATUS_SHUTDOWN, 0);
	}
	return 0;
}

int ScreenshotDialog::Shutdown(bool force) {
	if (!force && ReadStatus() == STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	return Dialog::Shutdown(force);
}

int ScreenshotDialog::ContStart() {
	if (ReadStatus() != STATUS_SCREENSHOT_UNKNOWN) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	ChangeStatus(STATUS_FINISHED, 0);
	return 0;
}

// ---------------------------------------------------------------- Game data install

namespace {
constexpr int GAMEDATA_INIT_DELAY_US = 200000;
constexpr u32 GAMEDATA_BYTES_PER_UPDATE = 32768 * 20;
const char *const kInstallDir = "disc0:/PSP_GAME/INSDIR";
}  // namespace

int GamedataInstallDialog::Init(u32 paramAddr) {
	if (GetStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {1424, 1432});
	if (check < 0) return check;
	paramAddr_ = paramAddr;
	files_.clear();
	nextFile_ = 0;
	totalSize_ = copiedSize_ = 0;
	for (const auto &f : savefs::List(kInstallDir)) {
		if (f.isDir) continue;
		files_.push_back(f.name);
		totalSize_ += (u64)f.size;
	}
	if (totalSize_ == 0) {
		LOGE("UTIL", "Game data install with no files");
		return -1;
	}
	ReadRequest(paramAddr, &request_);
	InitCommon();
	ChangeStatusInit(GAMEDATA_INIT_DELAY_US);
	return 0;
}

std::string GamedataInstallDialog::OutputPath(const std::string &file) {
	std::string dir = std::string(Savedata::kSavePath) + FixedString(request_.gameName, sizeof(request_.gameName)) +
	                  FixedString(request_.dataName, sizeof(request_.dataName));
	if (!savefs::Exists(dir)) {
		Savedata::Init();
		savefs::Mkdir(dir);
	}
	return dir + "/" + file;
}

void GamedataInstallDialog::UpdateProgress() {
	s32 progress = totalSize_ ? (s32)(copiedSize_ * 100 / totalSize_) : 100;
	GWrite32(paramAddr_ + offsetof(GamedataInstallParams, progress), (u32)progress);
}

void GamedataInstallDialog::WriteSfo() {
	vfs::ParamSfo sfo;
	std::string path = OutputPath("PARAM.SFO");
	std::vector<u8> old;
	if (savefs::ReadAll(path, old)) sfo.Parse(old);
	sfo.SetString("TITLE", FixedString(request_.sfoParam.title, sizeof(request_.sfoParam.title)), 128);
	sfo.SetString("SAVEDATA_TITLE", FixedString(request_.sfoParam.savedataTitle, sizeof(request_.sfoParam.savedataTitle)), 128);
	sfo.SetString("SAVEDATA_DETAIL", FixedString(request_.sfoParam.detail, sizeof(request_.sfoParam.detail)), 1024);
	sfo.SetInt("PARENTAL_LEVEL", request_.sfoParam.parentalLevel);
	sfo.SetString("CATEGORY", "MS", 4);
	sfo.SetString("SAVEDATA_DIRECTORY",
	              FixedString(request_.gameName, sizeof(request_.gameName)) + FixedString(request_.dataName, sizeof(request_.dataName)), 64);
	std::vector<u8> out = sfo.Serialize();
	savefs::WriteAll(path, out.data(), out.size());
}

int GamedataInstallDialog::Update(int animSpeed) {
	if (GetStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	s32 mode = (s32)GRead32(paramAddr_ + offsetof(GamedataInstallParams, mode));
	if (mode >= 2) {
		GWrite32(paramAddr_ + offsetof(DialogCommon, result), SCE_ERROR_UTILITY_GAMEDATA_INVALID_MODE);
		ChangeStatus(STATUS_FINISHED, 0);
		return 0;
	}
	UpdateCommon();
	if (nextFile_ < files_.size()) {
		// One file per Update (PPSSPP copies 640 KB per Update; whole small files are close enough).
		const std::string &name = files_[nextFile_++];
		std::vector<u8> data;
		if (savefs::ReadAll(std::string(kInstallDir) + "/" + name, data)) {
			if (!savefs::WriteAll(OutputPath(name), data.data(), data.size()))
				LOGE("UTIL", "Game data install: can't write %s", name.c_str());
			copiedSize_ += data.size();
		} else {
			LOGE("UTIL", "Game data install: can't read %s", name.c_str());
		}
		UpdateProgress();
	} else {
		WriteSfo();
		GWrite32(paramAddr_ + offsetof(GamedataInstallParams, unknownResult1), (u32)files_.size());
		GWrite32(paramAddr_ + offsetof(GamedataInstallParams, unknownResult2), (u32)files_.size());
		ChangeStatus(STATUS_FINISHED, 0);
	}
	(void)GAMEDATA_BYTES_PER_UPDATE;
	return 0;
}

int GamedataInstallDialog::Abort() {
	const Status status = ReadStatus();
	if (status == STATUS_NONE || status == STATUS_SHUTDOWN) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	if (mem::IsValidRange(paramAddr_, sizeof(DialogCommon))) GWrite32(paramAddr_ + offsetof(DialogCommon, result), 1);
	return Dialog::Shutdown();
}

int GamedataInstallDialog::Shutdown(bool force) {
	if (GetStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	return Dialog::Shutdown(force);
}

// ---------------------------------------------------------------- HTML viewer

namespace {
constexpr int HTMLVIEWER_INIT_DELAY_US = 200000;
constexpr int HTMLVIEWER_SHUTDOWN_DELAY_US = 40000;
// SceUtilityHtmlViewerParam: initialUrl, options and homeUrl (the smallest request is 0x70 bytes).
constexpr u32 kHtmlInitialUrlOffset = 0x40, kHtmlOptionsOffset = 0x4C, kHtmlHomeUrlOffset = 0x68;

std::string ReadUrl(u32 addr) {
	if (!addr) return "";
	const char *s = mem::GetCString(addr, 2048);
	return s ? s : "";
}
}  // namespace

DialogCommon *HtmlViewerDialog::GetCommonParam() {
	if (!paramsAddr_ || !mem::IsValidRange(paramsAddr_, sizeof(DialogCommon))) return nullptr;
	return (DialogCommon *)GPtr(paramsAddr_);
}

int HtmlViewerDialog::Init(u32 paramAddr) {
	if (GetStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {0x70, 0x78, 0x80, 0x98, 0xA4, 0xA8});
	if (check < 0) return check;
	const u32 size = GRead32(paramAddr);
	const u32 options = GRead32(paramAddr + kHtmlOptionsOffset);
	const u32 workSize = size >= 0x98 && (options & 0x400) ? 0x480000 : 0x380000;
	u32 addr = kernel::g_userMem.Alloc(workSize, false, "HtmlViewer");
	if (!addr) return (int)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
	workMemory_ = addr;
	paramsAddr_ = paramAddr;
	url_ = ReadUrl(GRead32(paramAddr + kHtmlInitialUrlOffset));
	if (url_.empty()) url_ = ReadUrl(GRead32(paramAddr + kHtmlHomeUrlOffset));
	started_ = true;
	framesRun_ = 0;
	LOGI("UTIL", "HtmlViewer: %s (offline, closing)", url_.c_str());
	ChangeStatus(STATUS_INITIALIZE, 0);
	ChangeStatus(STATUS_RUNNING, HTMLVIEWER_INIT_DELAY_US);
	InitCommon();
	UpdateButtons();
	StartFade(true);
	return 0;
}

int HtmlViewerDialog::Update(int animSpeed) {
	if (GetStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	UpdateButtons();
	UpdateCommon();
	UpdateFade(animSpeed);
	framesRun_++;
	// Offline: as if the player had looked and left the browser.
	if (framesRun_ == 2 || (framesRun_ > 2 && IsButtonPressed(CTRL_CIRCLE))) {
		if (DialogCommon *c = GetCommonParam()) c->result = 0;
		StartFade(false);
	}
	return 0;
}

int HtmlViewerDialog::Shutdown(bool force) {
	if (ReadStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	if (force) {
		workMemory_ = 0;
		Dialog::Shutdown(true);
	} else {
		if (workMemory_) kernel::g_userMem.Free(workMemory_);
		workMemory_ = 0;
		ChangeStatus(STATUS_SHUTDOWN, 0);
		ChangeStatus(STATUS_NONE, HTMLVIEWER_SHUTDOWN_DELAY_US);
	}
	return 0;
}

void HtmlViewerDialog::ResetState() {
	Dialog::ResetState();
	workMemory_ = 0;
	started_ = false;
	paramsAddr_ = 0;
}

// ---------------------------------------------------------------- Placeholders

namespace {
constexpr int PLACEHOLDER_INIT_DELAY_US = 200000;
constexpr int PLACEHOLDER_SHUTDOWN_DELAY_US = 2000;
}  // namespace

DialogCommon *PlaceholderDialog::GetCommonParam() {
	if (!paramsAddr_ || !mem::IsValidRange(paramsAddr_, sizeof(DialogCommon))) return nullptr;
	return (DialogCommon *)GPtr(paramsAddr_);
}

int PlaceholderDialog::Init(u32 paramAddr) {
	if (ReadStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	// The request sizes a PSP accepts for GameSharing.
	const int check = Type() == DialogType::GAMESHARING ? CheckRequest(paramAddr, {0x50, 0x54, 0x64}) : 0;
	if (check < 0) return check;
	if (!mem::IsValidRange(paramAddr, sizeof(DialogCommon))) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	paramsAddr_ = paramAddr;
	LOGI("UTIL", "Utility dialog %d isn't supported: it will report \"cancelled\"", (int)Type());
	ChangeStatusInit(PLACEHOLDER_INIT_DELAY_US);
	InitCommon();
	return 0;
}

int PlaceholderDialog::Update(int animSpeed) {
	if (ReadStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	if (DialogCommon *c = GetCommonParam()) c->result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
	ChangeStatus(STATUS_FINISHED, 0);
	return 0;
}

int PlaceholderDialog::Shutdown(bool force) {
	if (ReadStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(PLACEHOLDER_SHUTDOWN_DELAY_US);
	return 0;
}

}  // namespace utility
