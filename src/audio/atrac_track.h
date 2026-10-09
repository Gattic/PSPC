// SPDX-License-Identifier: GPL-2.0-or-later
//
// ATRAC3/ATRAC3+ container parsing (RIFF WAVE .at3 and OMA/AA3).
// Ported from PPSSPP's Core/Util/AtracTrack.{h,cpp} (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#pragma once

#include <string>
#include <vector>

#include "audio/audio_common.h"

namespace audio {

constexpr u32 ATRAC3_MAX_SAMPLES = 0x400;
constexpr u32 ATRAC3PLUS_MAX_SAMPLES = 0x800;

struct AtracLoopInfo {
	int cuePointID;
	int type;
	int startSample;
	int endSample;
	int fraction;
	int playCount;
};

// Mostly constant info, once a track has been loaded.
struct Track {
	u32 codecType = 0;
	u32 fileSize = 0;
	u32 bitrate = 64;
	int jointStereo = 0;
	u16 channels = 2;
	u16 bytesPerFrame = 0;
	int dataByteOffset = 0;
	int firstSampleOffset = 0;
	int endSample = -1;
	std::vector<AtracLoopInfo> loopinfo;
	int loopStartSample = -1;
	int loopEndSample = -1;

	int FirstOffsetExtra() const { return codecType == PSP_CODEC_AT3PLUS ? 0x170 : 0x45; }
	int FirstSampleOffsetFull() const { return FirstOffsetExtra() + firstSampleOffset; }
	int SamplesPerFrame() const { return codecType == PSP_CODEC_AT3PLUS ? ATRAC3PLUS_MAX_SAMPLES : ATRAC3_MAX_SAMPLES; }
};

// maxReadable: how many bytes at buffer are mapped (0 = assume `size`).
int AnalyzeAtracTrack(const u8 *buffer, u32 size, u32 maxReadable, Track *track, std::string *error);
int AnalyzeAA3Track(const u8 *buffer, u32 size, u32 filesize, Track *track, std::string *error);

struct TrackInfo {
	u16 numChans;
	u16 blockAlign;
	u8 sampleSizeMaybe;
	u8 tailFlag;
	u8 unused[2];
	u32 dataOff;
	u32 endSample;
	u32 waveDataSize;
	u32 firstSampleOffset;
	u32 loopStart;
	u32 loopEnd;
};

int ParseWaveAT3(const u8 *data, u32 length, TrackInfo *track);
int ParseAA3(const u8 *data, u32 readSize, u32 fileSize, TrackInfo *track);

}  // namespace audio
