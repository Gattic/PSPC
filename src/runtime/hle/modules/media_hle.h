// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared plumbing for the media HLE libraries (sceMpeg, sceMpegbase, scePsmf, scePsmfPlayer,
// sceVideocodec, sceJpeg, sceMp4). Register_MediaLibraries() registers all of them.
#pragma once

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

void Register_sceMpeg();
void Register_sceMpegbase();
void Register_scePsmf();
void Register_scePsmfPlayer();
void Register_sceVideocodec();
void Register_sceJpeg();
void Register_sceMp4();

}  // namespace mediahle

void Register_MediaLibraries();
