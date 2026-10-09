// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceUtility dialogs: the shared status state machine (NONE -> INIT -> RUNNING -> FINISHED ->
// SHUTDOWN -> NONE), volatile memory handling and button helpers.
// Ported from PPSSPP (Core/Dialog/PSPDialog.cpp/.h), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later. Drawing (PPGe) is not ported: dialogs draw nothing; see host_prompt.h for the
// windowed-mode prompts.
#pragma once

#include <initializer_list>
#include <string>

#include "common/common.h"

namespace utility {

enum class DialogType {
	NONE = 0,
	SAVEDATA,
	MSG,
	OSK,
	NET,
	SCREENSHOT,
	GAMESHARING,
	GAMEDATAINSTALL,
	NPSIGNIN,
	HTMLVIEWER,
	NETPLAY,
	PS3SCAN,
	STORECHECKOUT,
};

constexpr int SCE_UTILITY_DIALOG_RESULT_SUCCESS = 0;
constexpr int SCE_UTILITY_DIALOG_RESULT_CANCEL = 1;
constexpr int SCE_UTILITY_DIALOG_RESULT_ABORT = 2;

#pragma pack(push, 4)
struct DialogCommon {
	u32 size;
	s32 language;
	s32 buttonSwap;
	s32 graphicsThread;
	s32 accessThread;
	s32 fontThread;
	s32 soundThread;
	s32 result;
	s32 reserved[4];
};
#pragma pack(pop)
static_assert(sizeof(DialogCommon) == 48, "pspUtilityDialogCommon");

// PSP_CTRL_* bits.
enum : u32 {
	CTRL_SELECT = 0x0001,
	CTRL_START = 0x0008,
	CTRL_UP = 0x0010,
	CTRL_RIGHT = 0x0020,
	CTRL_DOWN = 0x0040,
	CTRL_LEFT = 0x0080,
	CTRL_LTRIGGER = 0x0100,
	CTRL_RTRIGGER = 0x0200,
	CTRL_TRIANGLE = 0x1000,
	CTRL_CIRCLE = 0x2000,
	CTRL_CROSS = 0x4000,
	CTRL_SQUARE = 0x8000,
};

class Dialog {
public:
	enum Status {
		STATUS_NONE = 0,
		STATUS_INITIALIZE = 1,
		STATUS_RUNNING = 2,
		STATUS_FINISHED = 3,
		STATUS_SHUTDOWN = 4,
		STATUS_SCREENSHOT_UNKNOWN = 5,
	};

	explicit Dialog(DialogType type) : type_(type) {}
	virtual ~Dialog() = default;

	virtual int Update(int animSpeed) = 0;
	virtual int Shutdown(bool force = false);
	// Back to NONE without any of Shutdown's side effects.
	virtual void ResetState();
	virtual DialogCommon *GetCommonParam() { return nullptr; }

	Status GetStatus();
	// Whether it keeps another dialog from starting. Applies a status change that's due, but doesn't
	// use up the one-time reports of auto-status dialogs.
	bool IsBusy();
	// An auto-status dialog in SHUTDOWN only waits for the game to see that.
	void FinishAutoShutdown();
	DialogType Type() const { return type_; }

	void FinishVolatile();
	int FinishInit(bool volatileLocked);
	int FinishShutdown();

protected:
	void InitCommon();
	void UpdateCommon();
	void UpdateButtons();
	bool IsButtonPressed(u32 button) const;
	void ChangeStatus(Status newStatus, int delayUs);
	void UpdatePendingStatus();
	void ChangeStatusInit(int delayUs);
	void ChangeStatusShutdown(int delayUs);
	Status ReadStatus() const { return status_; }

	virtual bool UseAutoStatus() = 0;
	virtual bool LocksVolatileMemory() const { return true; }

	static u32 GetConfirmButton();
	static u32 GetCancelButton();
	// What every InitStart checks first: the address, then the size against the ones that type
	// accepts (utility/dialog/sizes), then the rest of the range.
	static int CheckRequest(u32 addr, std::initializer_list<u32> sizes);

	void StartFade(bool fadeIn);
	void UpdateFade(int animSpeed);
	bool IsFadingOut() const { return isFading_ && !fadeIn_; }
	virtual void FinishFadeOut();

	Status pendingStatus_ = STATUS_NONE;
	u64 pendingStatusUs_ = 0;

	u32 lastButtons_ = 0;
	u32 buttons_ = 0;
	u32 okButtonFlag_ = CTRL_CIRCLE;
	u32 cancelButtonFlag_ = CTRL_CROSS;

	float fadeTimer_ = 0.0f;
	bool isFading_ = false;
	bool fadeIn_ = false;
	u32 fadeValue_ = 0;

	int yesnoChoice_ = 0;

private:
	Status status_ = STATUS_NONE;
	DialogType type_;
	bool volatileLocked_ = false;
};

const char *DialogStatusName(int status);

// ---- Provided by sceUtility (src/runtime/hle/modules/utility.cpp) ----

// Starts the dialog thread that runs the init or shutdown phases (see utility.cpp).
void DialogThreadInit(DialogType type, int delayUs, int accessPriority, int graphicsPriority);
void DialogThreadShutdown(DialogType type, int delayUs, int accessPriority, int graphicsPriority);
// Volatile memory (sceKernelVolatileMemLock/Unlock, non-blocking). 0 on success.
int VolatileMemTryLock();
int VolatileMemUnlock();
// System settings.
int SystemLanguage();
int SystemButtonPreference();  // 0 = O confirms, 1 = X confirms
// Firmware/SDK version the game was built with (sceKernelGetCompiledSdkVersion).
u32 CompiledSdkVersion();
// True when there is no window: dialogs answer by themselves.
bool Headless();

}  // namespace utility
