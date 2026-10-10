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
#include "runtime/hle/modules/net_hle.h"
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

// netAction values.
constexpr s32 NETCONF_CONNECT_APNET = 0, NETCONF_STATUS_APNET = 1, NETCONF_CONNECT_ADHOC = 2,
              NETCONF_CONNECT_APNET_LAST = 3, NETCONF_CREATE_ADHOC = 4, NETCONF_JOIN_ADHOC = 5;
// Give up on a connection that isn't up after this many updates (about 30 s at 60 per second).
constexpr int NETCONF_TIMEOUT_UPDATES = 1800;

// The connection the dialog is making (one Netconf dialog runs at a time). connResult: 1 = not
// asked yet, else what sceNetApctlConnect / sceNetAdhocctl* returned.
struct NetconfFlow {
	int connResult = 1;
	std::string error;
};
NetconfFlow g_netconfFlow;
}  // namespace

int NetconfDialog::Init(u32 paramAddr) {
	if (ReadStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {0x38, 0x40, 0x44});
	if (check < 0) return check;
	if (!ReadRequest(paramAddr, &request_)) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	requestAddr_ = paramAddr;
	LOGI("UTIL", "Netconf dialog (action %d)%s", request_.netAction, net::Enabled() ? "" : ": offline, the WLAN switch is off");
	g_netconfFlow = NetconfFlow{};
	request_.common.result = 0;
	ChangeStatusInit(NET_INIT_DELAY_US);
	InitCommon();
	UpdateButtons();
	framesRun_ = 0;
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
		// Bring the connection up (the virtual access point, or an ad hoc group through the ad hoc
		// server) and finish once it is; on an error the player can only back out.
		NetconfFlow &flow = g_netconfFlow;
		const s32 action = request_.netAction;
		const bool infra = action == NETCONF_CONNECT_APNET || action == NETCONF_STATUS_APNET || action == NETCONF_CONNECT_APNET_LAST;
		const bool adhoc = action == NETCONF_CONNECT_ADHOC || action == NETCONF_CREATE_ADHOC || action == NETCONF_JOIN_ADHOC;
		bool connected = false;
		if (flow.error.empty()) {
			char msg[160];
			if (!net::Enabled()) {
				flow.error = "The WLAN switch is off (network=0 in pspc.ini).";
			} else if (infra) {
				int state = net::ApctlGetState();
				if (state == net::APCTL_STATE_GOT_IP) {
					connected = true;
				} else if (state == net::APCTL_STATE_DISCONNECTED) {
					if (flow.connResult > 0) {
						flow.connResult = net::ApctlConnect(1);
						if (flow.connResult < 0) {
							snprintf(msg, sizeof(msg), "Could not connect to the access point (%08X).", (u32)flow.connResult);
							flow.error = msg;
						}
					} else if (flow.connResult == 0) {
						flow.error = "The connection to the access point was lost.";
					}
				}
			} else if (adhoc) {
				if (net::AdhocctlGetState() == net::ADHOCCTL_STATE_CONNECTED) {
					connected = true;
				} else if (flow.connResult > 0) {
					// netconfData -> SceUtilityNetconfData { char groupName[8]; u32 timeout; }.
					u32 group = request_.netconfData;
					flow.connResult = action == NETCONF_CREATE_ADHOC ? net::AdhocctlCreate(group) : net::AdhocctlConnect(group);
					if (flow.connResult < 0) {
						snprintf(msg, sizeof(msg), "Could not connect to the ad hoc network (%08X).", (u32)flow.connResult);
						flow.error = msg;
					}
				}
			} else {
				snprintf(msg, sizeof(msg), "Unknown network action %d.", action);
				flow.error = msg;
			}
			if (!connected && flow.error.empty() && framesRun_ > NETCONF_TIMEOUT_UPDATES)
				flow.error = "The connection timed out.";
			if (!flow.error.empty()) {
				LOGI("UTIL", "Netconf dialog: %s", flow.error.c_str());
				// Don't leave a half-made connection behind.
				if (flow.connResult == 0) {
					if (infra) net::ApctlDisconnect();
					else if (adhoc) net::AdhocctlDisconnect();
				}
			}
		}
		bool done = false;
		bool cancelled = false;
		if (connected) {
			LOGI("UTIL", "Netconf dialog: connected");
			done = true;
		} else if (!flow.error.empty()) {
			if (Headless()) {
				done = framesRun_ >= HEADLESS_ANSWER_UPDATES;
			} else {
				done = IsButtonPressed(cancelButtonFlag_);
			}
			cancelled = done;
		} else if (!Headless() && IsButtonPressed(cancelButtonFlag_)) {
			// Backing out while the connection is being made.
			if (infra) net::ApctlDisconnect();
			else if (adhoc) net::AdhocctlDisconnect();
			done = cancelled = true;
		}
		if (done) {
			StartFade(false);
			ChangeStatus(STATUS_FINISHED, NET_SHUTDOWN_DELAY_US);
			request_.common.result = cancelled ? SCE_UTILITY_DIALOG_RESULT_ABORT : 0;
		}
	}
	if (!Headless()) {
		// What the connection is doing, or what went wrong (the player backs out with the back button).
		const s32 action = request_.netAction;
		const bool adhocAction = action == NETCONF_CONNECT_ADHOC || action == NETCONF_CREATE_ADHOC || action == NETCONF_JOIN_ADHOC;
		if (!g_netconfFlow.error.empty())
			DisplayStatusScreen("Network Connection", "A connection error has occurred.\n" + g_netconfFlow.error, DS_BUTTON_CANCEL);
		else
			DisplayStatusScreen("Network Connection",
			                    adhocAction ? "Connecting.\nPlease wait..." : "Connecting to the access point.\nPlease wait...",
			                    DS_BUTTON_CANCEL);
	}
	if (ReadStatus() == STATUS_FINISHED || pendingStatus_ == STATUS_FINISHED)
		mem::WriteBytes(requestAddr_, &request_, std::min<u32>(request_.common.size, sizeof(request_)));
	return 0;
}

int NetconfDialog::Shutdown(bool force) {
	if (ReadStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(NET_SHUTDOWN_DELAY_US);
	return 0;
}

// ---------------------------------------------------------------- NP sign-in

namespace {
constexpr int NP_INIT_DELAY_US = 200000;
constexpr int NP_SHUTDOWN_DELAY_US = 501000;
constexpr s32 NP_SIGNIN_STATUS_NONE = 0;
constexpr s32 NP_SIGNIN_STATUS_SUCCESS = 1;
constexpr s32 NP_SIGNIN_STATUS_CANCELED = 2;
constexpr s32 NP_SIGNIN_STATUS_FAILED = 3;
}  // namespace

int NpSigninDialog::Init(u32 paramAddr) {
	if (ReadStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {0x40});
	if (check < 0) return check;
	if (!ReadRequest(paramAddr, &request_)) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	requestAddr_ = paramAddr;
	// An output; Update only acts while it's NONE.
	request_.npSigninStatus = NP_SIGNIN_STATUS_NONE;
	framesRun_ = 0;
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
	framesRun_++;
	// PSN sign-in goes to PSPC Network (net::NpStartSignIn): the server checks the account the
	// launcher signed in, or with no server set the player signs in locally.
	std::string message;
	net::NpSigninState state = net::NpGetSignInState(&message);
	if (pendingStatus_ != STATUS_FINISHED && request_.npSigninStatus == NP_SIGNIN_STATUS_NONE) {
		if (state == net::NpSigninState::Idle) {
			net::NpStartSignIn();
			state = net::NpGetSignInState(&message);
		}
		bool done = false;
		s32 status = NP_SIGNIN_STATUS_NONE;
		if (state == net::NpSigninState::SignedIn) {
			LOGI("UTIL", "NP sign-in: signed in as %s", net::NpOnlineId().c_str());
			done = true;
			status = NP_SIGNIN_STATUS_SUCCESS;
		} else if (state == net::NpSigninState::Failed) {
			done = Headless() ? framesRun_ >= HEADLESS_ANSWER_UPDATES : IsButtonPressed(cancelButtonFlag_);
			status = NP_SIGNIN_STATUS_FAILED;
			if (done) {
				LOGI("UTIL", "NP sign-in failed: %s", message.c_str());
				net::NpResetSignIn();
			}
		} else if (!Headless() && IsButtonPressed(cancelButtonFlag_)) {
			done = true;
			status = NP_SIGNIN_STATUS_CANCELED;
		}
		if (done) {
			StartFade(false);
			ChangeStatus(STATUS_FINISHED, NP_SHUTDOWN_DELAY_US);
			request_.common.result = status == NP_SIGNIN_STATUS_SUCCESS ? 0 : SCE_UTILITY_DIALOG_RESULT_ABORT;
			request_.npSigninStatus = status;
			mem::WriteBytes(requestAddr_, &request_, std::min<u32>(request_.common.size, sizeof(request_)));
		}
	}
	if (!Headless()) {
		if (state == net::NpSigninState::Failed)
			DisplayStatusScreen("PSPC Network", "Could not sign in.\n" + message, DS_BUTTON_CANCEL);
		else if (state == net::NpSigninState::SignedIn)
			DisplayStatusScreen("PSPC Network", "Signed in as " + net::NpOnlineId() + ".", DS_BUTTON_NONE);
		else
			DisplayStatusScreen("PSPC Network", "Signing in.\nPlease wait...", DS_BUTTON_CANCEL);
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
	if (!Headless()) StartFade(true);
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
	if (!Headless()) {
		UpdateFade(animSpeed);
		DisplayStatusScreen("Game Data Installation", "Installing game data.\nPlease wait...", DS_BUTTON_NONE,
		                    totalSize_ ? (float)copiedSize_ / (float)totalSize_ : 1.0f);
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
	if (!Headless()) DisplayStatusScreen("Internet Browser", "The Internet browser is not available.", DS_BUTTON_NONE);
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
