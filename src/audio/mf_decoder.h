// SPDX-License-Identifier: GPL-2.0-or-later
//
// MP3 and AAC frame decoding through the Media Foundation decoder transforms that ship with
// Windows (the MP3 decoder MFT and the Microsoft AAC decoder MFT). Synchronous: each call feeds
// one frame and drains whatever PCM the transform has ready.
#pragma once

#include <vector>

#include "common/common.h"

namespace audio {

class MFDecoder {
public:
	enum class Format { MP3, AAC_ADTS, AAC_RAW };

	MFDecoder() = default;
	~MFDecoder();
	MFDecoder(const MFDecoder &) = delete;
	MFDecoder &operator=(const MFDecoder &) = delete;

	// asc is the AudioSpecificConfig for raw AAC (ignored otherwise).
	bool Open(Format fmt, int sampleRate, int channels, const u8 *asc = nullptr, int ascSize = 0);
	void Close();
	bool IsOpen() const { return transform_ != nullptr; }
	int Channels() const { return outChannels_; }
	int SampleRate() const { return sampleRate_; }

	// Feeds one compressed frame and appends the decoded interleaved s16 PCM (Channels()
	// channels) to out. Returns false if the transform rejected the frame.
	bool Decode(const u8 *data, int size, std::vector<s16> &out);
	void Flush();

private:
	bool Drain(std::vector<s16> &out);

	void *transform_ = nullptr;  // IMFTransform*
	int sampleRate_ = 0;
	int channels_ = 0;
	int outChannels_ = 0;
	u32 outBufferSize_ = 0;
	bool providesSamples_ = false;
	s64 time_ = 0;
};

// True once Media Foundation could be started on this machine.
bool MFAvailable();

}  // namespace audio
