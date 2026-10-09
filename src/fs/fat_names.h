// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/FileSystem.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// FAT 8.3 short names, which sceIoDread reports through d_private on the memory stick and which
// games may open files by.
#pragma once

#include <string>
#include <vector>

namespace vfs {

// One short name per entry of a directory listing, in listing order. "." and ".." are their own
// short names. Collisions get ~N suffixes in listing order, so the listing order must be stable
// (creation order on a real FAT volume).
std::vector<std::string> GenerateFatShortNames(const std::vector<std::string> &names);

}  // namespace vfs
