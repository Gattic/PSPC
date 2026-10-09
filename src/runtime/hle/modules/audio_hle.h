// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared plumbing for the audio HLE libraries (sceAudio, sceVaudio, sceSasCore, sceAtrac3plus,
// sceMp3, sceAudiocodec, sceAac). Register_sceAudio() in audio.cpp registers all of them.
#pragma once

#include "audio/audio_common.h"
#include "common/common.h"

namespace audiohle {

// Starts the 44.1kHz output mixer and installs the guest memory accessor for pspc_audio.
// Idempotent; called by every audio entry point that needs it.
void EnsureInit();

// hleDelayResult: block the calling thread for usec, then return result. Skipped (returns at
// once) in an interrupt or with dispatch disabled, as on hardware nothing can wait there.
u32 DelayResult(u32 result, int usec);
// hleEatMicro: the call takes this long without letting other threads run.
void EatUs(int usec);
// The Media Engine runs one job at a time. Queues a job of `us` and returns how long from now
// until it finishes.
int MEScheduleJob(int us);

// sceKernelGetCompiledSdkVersion() of the running program.
u32 CompiledSdkVersion();

// ME time at the default clock for ATRAC (shared by sceAtrac and sceAudiocodec).
int AudioCodecInitUs(int codec, bool monoAt3Plus);
int AudioCodecDecodeUs(int codec, int channels, int frameBytes);
bool IsAtrac3StreamJointStereo(int codecType, int bytesPerFrame, int channels);
// The channel count the ATRAC3 decoder for a track works with, from libatrac3plus's table.
bool Atrac3DecoderChannels(int bytesPerFrame, bool jointStereo, int *channels);

// The context the Media Engine works on. Games allocate it, so the layout is fixed (128 bytes).
// Layout documented in PPSSPP's Core/HLE/sceAudiocodec.h.
struct SceAudiocodecCodec {
	u32 magic;            // 0x00
	s32 unk4;             // 0x04
	s32 err;              // 0x08
	u32 edramAddr;        // 0x0c
	s32 neededMem;        // 0x10
	s32 inited;           // 0x14
	u32 inBuf;            // 0x18
	s32 srcBytesRead;     // 0x1c
	u32 outBuf;           // 0x20
	s32 dstBytesWritten;  // 0x24
	union {
		struct {
			u8 formatByte1;  // 0x28
			u8 formatByte2;  // 0x29
			u8 unk2a;
			u8 unk2b;
			u32 unk2c;
			u32 at3Related;  // 0x30
			s32 unk34;
			s32 unk38[12];
		} at3;
		struct {
			u32 maxFrameBytes;  // 0x28
			u32 unk2c;
			s32 unk30;
			s32 unk34;
			s32 version;  // 0x38: 0 = MPEG2, 1 = MPEG1, 2 = MPEG2.5, 9999 = unknown
			s32 unk3c;
			s32 unk40;
			s32 bitrateIndex;     // 0x44
			s32 sampleRateIndex;  // 0x48
			s32 unk4c;
			s32 unk50;
			s32 channelConfig;  // 0x54
			s32 unk58;
			s32 unk5c;
			s32 unk60;
			s32 unk64;
		} mp3;
		struct {
			u32 sampleRate;  // 0x28
			u8 unk2c;
			u8 unk2d;
			u8 unk2e;
			u8 unk2f;
			s32 unk30[14];
		} aac;
		u8 raw[0x40];
	} fmt;
	u32 allocMem;  // 0x68
	u8 unk[0x14];
};
static_assert(sizeof(SceAudiocodecCodec) == 128, "SceAudiocodecCodec layout");

// Largest n <= size such that [addr, addr+n) is valid guest memory (0 if addr is invalid).
u32 ClampValidSize(u32 addr, u32 size);

// For sceUtilityLoadModule(PSP_MODULE_AV_ATRAC3PLUS): the module's bss holds the atrac
// contexts on hardware. Optional - without it the contexts live in kernel memory.
void AtracNotifyLoadModule(int version, u32 crc, u32 bssAddr, int bssSize);
void AtracNotifyUnloadModule();

void Register_sceVaudio();
void Register_sceSasCore();
void Register_sceAtrac3plus();
void Register_sceMp3();
void Register_sceAudiocodec();
void Register_sceAac();

}  // namespace audiohle
