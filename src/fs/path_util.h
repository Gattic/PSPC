// SPDX-License-Identifier: GPL-2.0-or-later
// PSP path handling shared by the HLE and the file systems.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/common.h"

namespace vfs {

// Splits "ms0:/PSP/GAME" into device "ms0:" (colon included) and rest "/PSP/GAME". A device is a
// non-empty prefix ending in ':' that comes before any '/' or '\\'. Returns false (device empty,
// rest = whole path) when there is none.
bool SplitPspDevice(std::string_view path, std::string_view *device, std::string_view *rest);

// Cleans a PSP path. A "device:" prefix is kept verbatim; the rest becomes absolute and
// '/'-separated: '\\' turns into '/', empty and "." components drop, ".." removes the previous
// component and is ignored at the root, so a path never climbs above its device root. There is no
// trailing slash except for the root itself. An empty remainder stays empty ("umd0:" -> "umd0:",
// "" -> ""), since the PSP tells the device itself apart from its root directory ("umd0:/").
//   "ms0:PSP//GAME/./x/../" -> "ms0:/PSP/GAME"     "a\\b" -> "/a/b"     "/../.." -> "/"
std::string NormalizePspPath(std::string_view path);

// Resolves `path` against a current directory, as sceIoOpen and friends do with the per-thread
// cwd. `cwd` is a full path such as "ms0:/PSP/GAME" (or device-relative). A path with a device
// ignores cwd; one starting with '/' keeps only cwd's device; anything else is appended to cwd.
// An empty path yields cwd. The result is normalized.
std::string NormalizePspPath(std::string_view cwd, std::string_view path);

// The cleaned components of a device-relative path ("/a/../b/c" -> {"b", "c"}). Same rules as
// NormalizePspPath, so ".." can never leave the root.
std::vector<std::string> SplitPspPathComponents(std::string_view path);

// ASCII case-insensitive equality, as the PSP's FAT and ISO9660 drivers compare names.
bool EqualsNoCase(std::string_view a, std::string_view b);

}  // namespace vfs
