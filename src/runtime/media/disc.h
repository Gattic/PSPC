// SPDX-License-Identifier: GPL-2.0-or-later
// UMD disc images (ISO/CSO/ZSO/DAX) mounted as disc0: (files) and umd0: (sectors).
#pragma once

#include <memory>
#include <string>

#include "runtime/io/io.h"

namespace vfs {
class FileSystem;
}

namespace media {

bool MountDisc(const std::string &hostPath);
// io::Init() clears mounts; call after it to mount the already-opened disc again.
void RemountDiscIfAny();
bool HaveDisc();
std::string DiscId();

// Wraps a vfs::FileSystem as an io::Device.
std::shared_ptr<io::Device> MakeVfsDevice(std::shared_ptr<vfs::FileSystem> fs, bool sectorMode, u32 devType);

}  // namespace media
