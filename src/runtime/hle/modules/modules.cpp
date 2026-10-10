// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/hle/modules/modules.h"

namespace hle {

void RegisterAllModules() {
	Register_ThreadManForUser();
	Register_SysMemUserForUser();
	Register_ModuleMgrForUser();
	Register_LoadExecForUser();
	Register_Kernel_Library();
	Register_InterruptManager();
	Register_IoFileMgrForUser();
	Register_StdioForUser();
	Register_UtilsForUser();
	Register_sceDisplay();
	Register_sceCtrl();
	Register_scePower();
	Register_sceRtc();
	Register_sceSuspendForUser();
	Register_sceUmdUser();
	Register_sceImpose();
	Register_sceDmac();
	Register_sceHprm();
	Register_sceGe_user();
	Register_sceAudio();
	Register_sceUtility();
	Register_Misc();
	Register_M33();
	Register_VshLibraries();
}

}  // namespace hle
