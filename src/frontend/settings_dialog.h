// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher's Settings dialog: edits pspc.ini (common/settings.h).
#pragma once

#include <windows.h>

#include "common/fileutil.h"

// Shows the modal dialog for the settings file `ini`. Returns true if the user saved changes.
bool ShowSettingsDialog(HWND owner, const fs::path &ini);
