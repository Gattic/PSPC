// SPDX-License-Identifier: GPL-2.0-or-later
// UMD disc images (ISO/CSO/ZSO/DAX) mounted as disc0: (ISO9660 files) and umd0:/umd1:/umd:
// (the whole disc in 2048-byte sectors).
#pragma once

#include <memory>
#include <string>

namespace vfs {
class IsoFileSystem;
}

namespace media {

bool MountDisc(const std::string &hostPath);
void UnmountDisc();
// io::Init() clears mounts; call after it to mount the already-opened disc again.
void RemountDiscIfAny();
bool HaveDisc();
std::string DiscId();
std::shared_ptr<vfs::IsoFileSystem> Disc();

}  // namespace media
