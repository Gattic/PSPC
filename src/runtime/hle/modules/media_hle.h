// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared plumbing for the media HLE libraries (sceMpeg, sceMpegbase, scePsmf, scePsmfPlayer,
// sceVideocodec, sceJpeg, sceMp4). Register_MediaLibraries() registers all of them.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"

namespace mediahle {

// Installs the guest memory accessor for pspc_media. Idempotent.
void EnsureInit();

// sceMpegBasePESpacketCopy hands the scatter-gather list it just walked to the PMP path.
void MpegSetPmpVideoSource(u32 addr, int blocks);
// The mpeg library version the game loaded (sceMpeg_library's module version, e.g. 0x0105).
void MpegNotifyLoadModule(int version);
// The psmfplayer library's devkit version, as PPSSPP's __PsmfPlayerLoadModule.
void PsmfPlayerNotifyLoadModule(int devkitVersion);

// The Media Engine's own memory (sceVideocodec EDRAM and frame buffers), separate from the
// Allegrex's. Null if the range isn't inside it.
u8 *MEGetPointerRange(u32 addr, u32 size);
// Media Engine address, or failing that guest memory.
const u8 *MEOrGuestPointer(u32 addr, u32 size);
// A block of Media Engine memory (64-byte aligned), 0 if there is no room; and freeing one.
u32 MEAllocate(u32 size);
void MERelease(u32 addr);

void Register_sceMpeg();
void Register_sceMpegbase();
void Register_scePsmf();
void Register_scePsmfPlayer();
void Register_sceVideocodec();
void Register_sceJpeg();
void Register_sceMp4();

}  // namespace mediahle

void Register_MediaLibraries();

// The module loader calls this for a game-shipped module it replaces natively, so the media
// libraries can follow the version the game was built against (as PPSSPP's __MpegLoadModule and
// __PsmfPlayerLoadModule do).
void Media_NotifyNativeModuleLoad(const std::string &name, const std::vector<u8> &data);
