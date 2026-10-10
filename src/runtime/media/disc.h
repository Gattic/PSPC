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
// Host path of the mounted disc image ("" if none).
std::string DiscPath();
std::shared_ptr<vfs::IsoFileSystem> Disc();
// What the disc holds, as sceUmdGetDiscInfo and the drive report it: 0x10 a game (PSP_GAME),
// 0x20 UMD Video (UMD_VIDEO), 0x40 UMD Audio (UMD_AUDIO), or a combination. 0x10 without a disc.
unsigned DiscType();

}  // namespace media
