// SPDX-License-Identifier: GPL-2.0-or-later
// Registration entry points of every HLE library. Called once from hle::RegisterAllModules().
#pragma once

void Register_ThreadManForUser();
void Register_SysMemUserForUser();
void Register_ModuleMgrForUser();
void Register_LoadExecForUser();
void Register_Kernel_Library();
void Register_InterruptManager();
void Register_IoFileMgrForUser();
void Register_StdioForUser();
void Register_UtilsForUser();
void Register_sceDisplay();
void Register_sceCtrl();
void Register_scePower();
void Register_sceRtc();
void Register_sceSuspendForUser();
void Register_sceUmdUser();
void Register_sceImpose();
void Register_sceDmac();
void Register_sceGe_user();
void Register_sceAudio();
void Register_sceUtility();
void Register_sceHprm();
void Register_Misc();
void Register_M33();

namespace hle {
void RegisterAllModules();
}
