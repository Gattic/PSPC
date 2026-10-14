// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/fileutil.h"

// Linux uses per-user XDG roots, independently of the install and working directory.
// Windows retains its per-user, executable-relative installation layout.
namespace apppaths {
fs::path ConfigRoot();
fs::path DataRoot();
fs::path CacheRoot();
fs::path StateRoot();
// Relative configured game/plugin folders are relative to the user's data root.
fs::path DataPath(const std::string &path);
// Installed data/nids.csv identifies a complete resource root. Ancestors allow source builds.
fs::path ResourceRoot(const fs::path &exeDir = ExecutableDir());
} // namespace apppaths
