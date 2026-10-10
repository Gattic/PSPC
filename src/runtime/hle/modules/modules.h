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
// kernel_clib.cpp: SysclibForKernel, StdioForKernel, LoadCoreForKernel (for kernel plugins).
void Register_KernelClib();
// vsh_bridge.cpp: sceVshBridge and sceResmgr (the XMB's kernel bridge). Registered last: it
// forwards to functions of the other libraries.
void Register_VshLibraries();

namespace hle {
void RegisterAllModules();
}
