// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher's Network dialog: the PSPC Network account games sign in to instead of the
// PlayStation Network (server, sign in / create account / sign out, friends), or a local online
// ID with no server. Writes np_server, np_online_id and np_token to pspc.ini.
#pragma once

#include <windows.h>

#include "common/fileutil.h"

// Returns true if anything was saved.
bool ShowNetworkDialog(HWND owner, const fs::path &ini);
