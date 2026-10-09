// SPDX-License-Identifier: GPL-2.0-or-later
//
// Packet-by-packet audio decoders: the core of what the Media Engine does for sceAudiocodec,
// sceAtrac, sceMp3 and sceAac. Interface modelled on PPSSPP's Core/HW/SimpleAudioDec.h
// (https://github.com/hrydgard/ppsspp, GPL-2.0-or-later).
//
// ATRAC3/ATRAC3+ use the FFmpeg-derived standalone decoders (third_party/at3_standalone, LGPL).
// MP3 and AAC use the Media Foundation decoders that ship with Windows.
#pragma once

#include <cstddef>

#include "audio/audio_common.h"

namespace audio {

class AudioDecoder {
public:
	virtual ~AudioDecoder() {}

	virtual CodecType GetCodecType() const = 0;

	// Decodes one packet. inbytesConsumed can include skipped junk before a frame. outSamples is
	// in sample frames (multiply by 2 * outputChannels for bytes). For ATRAC, a nonzero *outSamples
	// on entry caps the output. Returns false if the frame did not decode.
	virtual bool Decode(const u8 *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, s16 *outbuf,
	                    int *outSamples) = 0;
	virtual bool IsOK() const = 0;
	// Only meaningful for ATRAC3+ before the first decode (sceSas mono voices).
	virtual void SetChannels(int channels) {}
	virtual void FlushBuffers() {}

	void SetCtxPtr(u32 ptr) { ctxPtr_ = ptr; }
	u32 GetCtxPtr() const { return ctxPtr_; }

private:
	u32 ctxPtr_ = 0xFFFFFFFF;
};

AudioDecoder *CreateAtrac3Audio(int channels, int blockAlign, const u8 *extraData, int extraDataSize);
AudioDecoder *CreateAtrac3PlusAudio(int channels, int blockAlign);
AudioDecoder *CreateMp3Audio();
// adtsStream: input is an ADTS byte stream (sceAac), so junk before a sync is skipped. Otherwise
// frames are raw access units (sceAudiocodec), or single ADTS frames.
AudioDecoder *CreateAacAudio(int sampleRateHz, int channels, bool adtsStream = false);
AudioDecoder *CreateAudioDecoder(CodecType type, int sampleRateHz = 44100, int channels = 2, int blockAlign = 0,
                                 const u8 *extraData = nullptr, int extraDataSize = 0);

// ---- MPEG audio frame headers ----
struct Mp3FrameInfo {
	int versionBits = 0;  // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5, 1 = reserved
	int layerBits = 0;    // 1 = Layer III, 2 = II, 3 = I
	int bitrateKbps = 0;
	int sampleRate = 0;
	int channels = 0;
	int channelMode = 0;
	int padding = 0;
	int frameBytes = 0;       // including the header; 0 if free-format / invalid
	int samplesPerFrame = 0;  // 384, 576 or 1152
};
// Parses the 4-byte header at p. Returns false if it isn't a valid MPEG audio header.
bool ParseMp3Header(const u8 *p, Mp3FrameInfo *info);

}  // namespace audio
