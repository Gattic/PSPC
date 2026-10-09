// SPDX-License-Identifier: GPL-2.0-or-later
// Libraries whose implementation hasn't landed yet. Each Register_* here is replaced by a real
// module file; until then their imports fall through to the "unimplemented" stub.
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"

#ifndef PSPC_HAVE_GE_MODULE
void Register_sceGe_user() {}
#endif
#ifndef PSPC_HAVE_AUDIO_MODULE
void Register_sceAudio() {}
#endif
#ifndef PSPC_HAVE_UTILITY_MODULE
void Register_sceUtility() {}
#endif
#ifndef PSPC_HAVE_MISC_MODULE
void Register_Misc() {}
#endif
