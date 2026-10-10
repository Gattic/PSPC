// SPDX-License-Identifier: GPL-2.0-or-later
//
// The HOME menu: HOME (Esc, or the controller's guide button) during a game brings up the PSP's
// "Quit the game?" screen over it, drawn with ppge on the system layer. The game keeps running
// underneath but sees no input while it is up. Yes quits as the PSP does: the game's exit callback
// runs, and its sceKernelExitGame ends the run (or goes back to the XMB when the XMB launched the
// game, --return-to-vsh); a game without one quits at once. No, the back button or HOME again
// resume the game.
//
// Without overlays (headless runs) there is no menu: HOME quits at once a game the XMB launched
// and does nothing otherwise, as before the menu existed.
#pragma once

#include "common/common.h"

namespace homemenu {

// HOME opens the menu (games; not the XMB, which handles HOME itself).
void Install(bool enable);
// sceCtrl calls this when HOME goes down.
void OnHomePressed();
// At every VBlank, before the frame is presented: the menu's input and drawing.
void OnVblank();
bool IsOpen();

// What the game (sceCtrl) and the sceUtility dialogs get of the controller: nothing while the menu
// is up and, after it closes, not the buttons still held from it until they are released.
u32 FilterButtons(u32 raw);
// True while the menu is up (the analog stick reads centered).
bool InputBlocked();

}  // namespace homemenu
