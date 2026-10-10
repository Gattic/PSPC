// SPDX-License-Identifier: GPL-2.0-or-later
//
// Booting the real 4.01 XMB (pspc --vsh): the firmware's VSH user modules run as guest code on
// top of the native kernel. See docs/vsh-boot.md.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"

namespace vsh {

// True when this run boots the XMB rather than a game.
bool Active();
void SetActive(bool active);

// Loads and starts the VSH the way init.prx does for the VSH boot mode of pspbtcnf: every module
// of the list in order, each module_start waited for before the next loads. Kernel modules of
// that list are native (HLE) and skipped. Returns 0 or a negative error (missing firmware).
// `recovery` (or R held at boot, as M33 checks it): M33's recovery menu
// (flash0:/vsh/module/recovery.prx) boots instead of the XMB.
int Boot(bool recovery = false);

// The vshmain_args block the XMB passed to the game it launched (LoadExecVSH*), handed back to
// vshmain's module_start when the game exits to the XMB, as LoadExec does: the XMB then knows it
// isn't a cold boot (no UMD auto-start) and returns to the item it launched.
void SetVshmainArgs(std::vector<u8> args);

// The firmware's VSH modules aren't installed (no flash0:/vsh/module/vshmain.prx).
bool FirmwareMissing(std::string *why);

// A game the XMB launched: quitting it (sceKernelExitGame, or HOME, which runs the game's exit
// callback as the PSP's "Quit game" does) goes back to the XMB.
void SetReturnToXmb(bool enable);
bool ReturnToXmb();

}  // namespace vsh
