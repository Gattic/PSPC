// SPDX-License-Identifier: GPL-2.0-or-later
//
// Register_Misc(): the system libraries that aren't part of the kernel or sceUtility.
#include "runtime/hle/modules/misc_libs.h"
#include "runtime/hle/modules/modules.h"

void Register_Misc() {
	Register_NetLibraries();
	Register_CccLibrary();
	Register_RegAndSystemLibraries();
	Register_FontLibraries();
	Register_MediaLibraries();
	Register_ChnnlsvLibraries();
}
