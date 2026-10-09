// SPDX-License-Identifier: GPL-2.0-or-later
//
// The sceUtility dialogs that PSPC answers without real UI: network configuration (offline: the
// WLAN switch is off), NP sign-in (cancelled), screenshot, game data install, HTML viewer, and
// placeholders (game sharing, netplay, PS3 scan, store checkout) that report "cancelled".
// Ported from PPSSPP (Core/Dialog/PSPNetconfDialog.cpp, PSPNpSigninDialog.cpp,
// PSPScreenshotDialog.cpp, PSPGamedataInstallDialog.cpp, PSPHtmlViewerDialog.cpp,
// PSPPlaceholderDialog.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "runtime/dialogs/dialog.h"
#include "runtime/dialogs/host_prompt.h"
#include "runtime/dialogs/savedata.h"

namespace utility {

#pragma pack(push, 4)
struct NetconfParams {
	DialogCommon common;
	s32 netAction;
	u32 netconfData;
	s32 netHotspot;
	s32 netHotspotConnected;
	s32 netWifiSpot;
};
struct NpSigninParams {
	DialogCommon common;
	s32 npSigninStatus;
	s32 unknown1;
	s32 unknown2;
	s32 unknown3;
};
struct GamedataInstallParams {
	DialogCommon common;
	s32 mode;
	char gameName[13];
	char ignore1[3];
	char dataName[20];
	SavedataSfoParam sfoParam;
	s32 progress;
	u32 unknownResult1;
	u32 unknownResult2;
	char ignore3[48];
};
#pragma pack(pop)

class NetconfDialog : public Dialog {
public:
	NetconfDialog() : Dialog(DialogType::NET) {}
	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	DialogCommon *GetCommonParam() override { return &request_.common; }

protected:
	bool UseAutoStatus() override { return false; }

private:
	NetconfParams request_{};
	u32 requestAddr_ = 0;
	int framesRun_ = 0;
	std::shared_ptr<prompt::State> prompt_;
};

class NpSigninDialog : public Dialog {
public:
	NpSigninDialog() : Dialog(DialogType::NPSIGNIN) {}
	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	DialogCommon *GetCommonParam() override { return &request_.common; }

protected:
	bool UseAutoStatus() override { return false; }

private:
	NpSigninParams request_{};
	u32 requestAddr_ = 0;
};

class ScreenshotDialog : public Dialog {
public:
	ScreenshotDialog() : Dialog(DialogType::SCREENSHOT) {}
	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	int ContStart();
	DialogCommon *GetCommonParam() override;

protected:
	bool UseAutoStatus() override { return true; }

private:
	u32 paramsAddr_ = 0;
	int mode_ = 0;
};

class GamedataInstallDialog : public Dialog {
public:
	GamedataInstallDialog() : Dialog(DialogType::GAMEDATAINSTALL) {}
	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	int Abort();
	DialogCommon *GetCommonParam() override { return &request_.common; }

protected:
	bool UseAutoStatus() override { return true; }

private:
	std::string OutputPath(const std::string &file);
	void WriteSfo();
	void UpdateProgress();

	GamedataInstallParams request_{};
	u32 paramAddr_ = 0;
	std::vector<std::string> files_;
	size_t nextFile_ = 0;
	u64 totalSize_ = 0, copiedSize_ = 0;
};

class HtmlViewerDialog : public Dialog {
public:
	HtmlViewerDialog() : Dialog(DialogType::HTMLVIEWER) {}
	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	void ResetState() override;
	DialogCommon *GetCommonParam() override;
	// Until one has started, all the HtmlViewer calls return WRONG_TYPE.
	bool HasStarted() const { return started_; }

protected:
	bool UseAutoStatus() override { return false; }
	bool LocksVolatileMemory() const override { return false; }

private:
	u32 paramsAddr_ = 0;
	u32 workMemory_ = 0;
	bool started_ = false;
	std::string url_;
	int framesRun_ = 0;
};

// Runs the normal lifecycle and reports that the user cancelled (GameSharing checks the request
// sizes a PSP accepts).
class PlaceholderDialog : public Dialog {
public:
	explicit PlaceholderDialog(DialogType type) : Dialog(type) {}
	int Init(u32 paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	DialogCommon *GetCommonParam() override;

protected:
	bool UseAutoStatus() override { return false; }

private:
	u32 paramsAddr_ = 0;
};

}  // namespace utility
