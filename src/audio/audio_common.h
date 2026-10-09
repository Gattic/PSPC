// SPDX-License-Identifier: GPL-2.0-or-later
// Shared definitions for the PSP audio codecs and mixers.
#pragma once

#include "common/common.h"

namespace audio {

// sceAudiocodec codec ids ("audioType"). Every entry point validates codec - 0x1000 < 6.
enum CodecType : int {
	PSP_CODEC_AT3PLUS = 0x1000,
	PSP_CODEC_AT3 = 0x1001,
	PSP_CODEC_MP3 = 0x1002,
	PSP_CODEC_AAC = 0x1003,
	PSP_CODEC_UNKNOWN_1004 = 0x1004,
	PSP_CODEC_WMA = 0x1005,
};

inline s16 ClampS16(int v) {
	return (s16)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

const char *CodecName(int codec);
inline bool IsValidCodec(int codec) { return codec >= PSP_CODEC_AT3PLUS && codec <= PSP_CODEC_AAC; }

}  // namespace audio
