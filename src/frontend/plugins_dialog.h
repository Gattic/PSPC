// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher's Plugins dialog: the M33 plugins in pspc's plugins folder (seplugins/game.txt,
// vsh.txt, pops.txt), each switched on or off with a checkbox, as M33's recovery menu does on a
// PSP; adding a PRX; and the "load plugins" setting in pspc.ini.
#pragma once

#include <windows.h>

#include "common/fileutil.h"

// `pluginsDir` is the folder laid over the memory stick (created if missing). Returns true if
// anything was saved.
bool ShowPluginsDialog(HWND owner, const fs::path &ini, const fs::path &pluginsDir);
