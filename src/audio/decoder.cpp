// SPDX-License-Identifier: GPL-2.0-or-later
//
// The ATRAC3/ATRAC3+ wrapper is ported from PPSSPP's Core/HW/Atrac3Standalone.cpp
// (https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include "audio/decoder.h"

#include <algorithm>
#include <vector>

#include "audio/mf_decoder.h"
#include "audio/third_party/at3_standalone/at3_decoders.h"
#include "common/log.h"

namespace audio {

namespace {

inline s16 Clamp16(float f) {
	if (f >= 1.0f) return 32767;
	if (f <= -1.0f) return -32767;
	return (s16)(int)(f * 32767);
}

class Atrac3Audio : public AudioDecoder {
public:
	Atrac3Audio(CodecType type, int channels, int blockAlign, const u8 *extraData, int extraDataSize)
		: type_(type), channels_(channels), blockAlign_(blockAlign) {
		if (type_ == PSP_CODEC_AT3) {
			at3Ctx_ = atrac3_alloc(channels, &blockAlign_, extraData, extraDataSize);
			if (at3Ctx_) {
				codecOpen_ = true;
			} else {
				LOGE("ATRAC", "Failed to open atrac3 context (channels=%d blockAlign=%d)", channels, blockAlign);
				codecFailed_ = true;
			}
		}
		for (int i = 0; i < 2; i++) buffers_[i] = new float[4096];
	}
	~Atrac3Audio() override {
		if (at3Ctx_) atrac3_free(at3Ctx_);
		if (at3pCtx_) atrac3p_free(at3pCtx_);
		for (int i = 0; i < 2; i++) delete[] buffers_[i];
	}

	CodecType GetCodecType() const override { return type_; }
	bool IsOK() const override { return codecOpen_; }

	void FlushBuffers() override {
		if (at3Ctx_) atrac3_flush_buffers(at3Ctx_);
		if (at3pCtx_) atrac3p_flush_buffers(at3pCtx_);
	}

	void SetChannels(int channels) override {
		if (type_ == PSP_CODEC_AT3PLUS && !at3pCtx_) channels_ = channels;
	}

	bool Decode(const u8 *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, s16 *outbuf,
	            int *outSamples) override {
		const int cap = outSamples ? *outSamples : 0;
		if (outSamples) *outSamples = 0;
		if (inbytesConsumed) *inbytesConsumed = 0;
		if (!codecOpen_) {
			// ATRAC3+ opens on the first decode, so a SetChannels from sceSas can take effect.
			if (codecFailed_) return false;
			if (type_ == PSP_CODEC_AT3PLUS) {
				at3pCtx_ = atrac3p_alloc(channels_, &blockAlign_);
				if (at3pCtx_) {
					codecOpen_ = true;
				} else {
					LOGE("ATRAC", "Failed to open atrac3+ context (channels=%d blockAlign=%d)", channels_, blockAlign_);
					codecFailed_ = true;
				}
			}
			if (!codecOpen_) return false;
		}
		blockAlign_ = inbytes;
		int nbSamples = 0;
		int result;
		if (type_ == PSP_CODEC_AT3PLUS) {
			result = atrac3p_decode_frame(at3pCtx_, buffers_, &nbSamples, inbuf, inbytes);
		} else {
			result = atrac3_decode_frame(at3Ctx_, buffers_, &nbSamples, inbuf, inbytes);
		}
		if (cap != 0) nbSamples = std::min(cap, nbSamples);
		if (result < 0) {
			// Report the regular packet size so a bad packet isn't retried forever.
			if (inbytesConsumed) *inbytesConsumed = inbytes;
			if (outSamples) *outSamples = nbSamples;
			return false;
		}
		if (inbytesConsumed) *inbytesConsumed = result;
		if (outSamples) *outSamples = nbSamples;
		if (nbSamples > 0 && outbuf) {
			const float *left = buffers_[0];
			if (outputChannels == 2) {
				const float *right = channels_ == 2 ? buffers_[1] : buffers_[0];
				for (int i = 0; i < nbSamples; i++) {
					outbuf[i * 2] = Clamp16(left[i]);
					outbuf[i * 2 + 1] = Clamp16(right[i]);
				}
			} else {
				for (int i = 0; i < nbSamples; i++) outbuf[i] = Clamp16(left[i]);
			}
		}
		return true;
	}

private:
	CodecType type_;
	ATRAC3PContext *at3pCtx_ = nullptr;
	ATRAC3Context *at3Ctx_ = nullptr;
	int channels_ = 0;
	int blockAlign_ = 0;
	float *buffers_[2]{};
	bool codecOpen_ = false;
	bool codecFailed_ = false;
};

// Writes `frames` sample frames of `inCh` channel PCM as `outCh` channels.
void ConvertChannels(const s16 *in, int inCh, s16 *out, int outCh, int frames) {
	for (int i = 0; i < frames; i++) {
		s16 l = in[i * inCh];
		s16 r = inCh >= 2 ? in[i * inCh + 1] : l;
		if (outCh == 2) {
			out[i * 2] = l;
			out[i * 2 + 1] = r;
		} else {
			out[i] = (s16)(((int)l + r) / 2);
		}
	}
}

// MP3, one frame per call. The frame is found and measured here from its header, so the caller
// always gets exactly one frame consumed and that frame's worth of samples back.
//
// The PSP (like FFmpeg) outputs the raw synthesis, decoder delay included, frame-aligned. The
// Media Foundation decoder instead trims the 528-sample decoder delay at stream start and holds
// back the first frame until the second arrives. Prepending the delay, and dropping from what
// arrives later the samples that had to be made up for the first frame, restores the PSP's
// alignment from the second frame on (only frame 0's made-up tail, inside the decoder delay
// anyway, differs).
class Mp3Audio : public AudioDecoder {
public:
	CodecType GetCodecType() const override { return PSP_CODEC_MP3; }
	bool IsOK() const override { return true; }
	void FlushBuffers() override {
		if (mf_.IsOpen()) mf_.Flush();
		StartStream();
	}

	bool Decode(const u8 *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, s16 *outbuf,
	            int *outSamples) override {
		if (outSamples) *outSamples = 0;
		if (inbytesConsumed) *inbytesConsumed = 0;
		if (!inbuf || inbytes < 4) return false;
		int start = -1;
		Mp3FrameInfo info;
		for (int i = 0; i + 4 <= inbytes; i++) {
			if (inbuf[i] == 0xFF && (inbuf[i + 1] & 0xE0) == 0xE0 && ParseMp3Header(inbuf + i, &info) && info.frameBytes > 0) {
				start = i;
				break;
			}
		}
		if (start < 0) return false;
		// Like FFmpeg, an incomplete frame is not decoded.
		if (info.frameBytes > inbytes - start) return false;
		const int frameBytes = info.frameBytes;

		if (info.layerBits == 1 && !mfFailed_) {
			if (!mf_.IsOpen() || rate_ != info.sampleRate || channels_ != info.channels) {
				StartStream();
				rate_ = info.sampleRate;
				channels_ = info.channels;
				if (!mf_.Open(MFDecoder::Format::MP3, info.sampleRate, info.channels)) mfFailed_ = true;
			}
		}
		if (mf_.IsOpen() && info.layerBits == 1) {
			pcm_.clear();
			mf_.Decode(inbuf + start, frameBytes, pcm_);
			const int mfCh = std::max(1, mf_.Channels());
			size_t frames = pcm_.size() / mfCh;
			const size_t skip = std::min<size_t>(frames, (size_t)debt_);
			debt_ -= (int)skip;
			frames -= skip;
			const size_t old = fifo_.size();
			fifo_.resize(old + frames * 2);
			ConvertChannels(pcm_.data() + skip * mfCh, mfCh, fifo_.data() + old, 2, (int)frames);
			// Never let it run away if the decoder bursts.
			const size_t maxFifo = (size_t)info.samplesPerFrame * 2 * 4;
			if (fifo_.size() > maxFifo) fifo_.erase(fifo_.begin(), fifo_.begin() + (fifo_.size() - maxFifo));
		}

		const int n = info.samplesPerFrame;
		const int have = (int)std::min<size_t>(fifo_.size() / 2, (size_t)n);
		if (outbuf) {
			if (outputChannels == 2) {
				memcpy(outbuf, fifo_.data(), have * 4);
				memset(outbuf + have * 2, 0, (n - have) * 4);
			} else {
				ConvertChannels(fifo_.data(), 2, outbuf, 1, have);
				memset(outbuf + have, 0, (n - have) * 2);
			}
		}
		fifo_.erase(fifo_.begin(), fifo_.begin() + have * 2);
		if (mf_.IsOpen()) debt_ += n - have;
		if (inbytesConsumed) *inbytesConsumed = start + frameBytes;
		if (outSamples) *outSamples = n;
		return true;
	}

private:
	static constexpr int kMfTrimmedDelay = 528;
	void StartStream() {
		fifo_.assign(kMfTrimmedDelay * 2, 0);
		debt_ = 0;
	}

	MFDecoder mf_;
	bool mfFailed_ = false;
	int rate_ = 0, channels_ = 0;
	int debt_ = 0;
	std::vector<s16> pcm_, fifo_;
};

const int kAacRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};

class AacAudio : public AudioDecoder {
public:
	AacAudio(int sampleRate, int channels, bool adtsStream) : adtsStream_(adtsStream), rate_(sampleRate), channels_(channels) {}
	CodecType GetCodecType() const override { return PSP_CODEC_AAC; }
	bool IsOK() const override { return true; }
	void FlushBuffers() override {
		if (mf_.IsOpen()) mf_.Flush();
	}

	bool Decode(const u8 *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, s16 *outbuf,
	            int *outSamples) override {
		if (outSamples) *outSamples = 0;
		if (inbytesConsumed) *inbytesConsumed = 0;
		if (!inbuf || inbytes <= 0) return false;
		int consumed = inbytes;
		bool adts = inbytes >= 7 && inbuf[0] == 0xFF && (inbuf[1] & 0xF6) == 0xF0;
		int start = 0;
		if (!adts) {
			// Streams through sceAac are ADTS; skip junk up to the next sync if there is one.
			for (int i = 1; i + 7 <= inbytes && i < 4096; i++) {
				if (adtsStream_ && inbuf[i] == 0xFF && (inbuf[i + 1] & 0xF6) == 0xF0) {
					start = i;
					adts = true;
					break;
				}
			}
		}
		if (adts) {
			const u8 *h = inbuf + start;
			int frameLen = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
			if (frameLen < 7) return false;
			const int rate = kAacRates[(h[2] >> 2) & 0xF];
			const int ch = ((h[2] & 1) << 2) | (h[3] >> 6);
			if (mode_ != Mode::Adts || !mf_.IsOpen() || rate != rate_ || ch != channels_) {
				rate_ = rate;
				channels_ = ch ? ch : 2;
				mode_ = Mode::Adts;
				const int profile = ((h[2] >> 6) & 3) + 1;
				const int fi = (h[2] >> 2) & 0xF;
				u8 asc[2] = {(u8)((profile << 3) | (fi >> 1)), (u8)(((fi & 1) << 7) | (channels_ << 3))};
				if (!mfFailed_ && !mf_.Open(MFDecoder::Format::AAC_ADTS, rate_, channels_, asc, 2)) mfFailed_ = true;
			}
			consumed = start + std::min(frameLen, inbytes - start);
			pcm_.clear();
			if (mf_.IsOpen()) mf_.Decode(h, std::min(frameLen, inbytes - start), pcm_);
		} else {
			if (mode_ != Mode::Raw || !mf_.IsOpen()) {
				mode_ = Mode::Raw;
				int fi = 4;
				for (int i = 0; i < 13; i++) {
					if (kAacRates[i] == rate_) fi = i;
				}
				const int ch = channels_ > 0 && channels_ <= 7 ? channels_ : 2;
				u8 asc[2] = {(u8)((2 << 3) | (fi >> 1)), (u8)(((fi & 1) << 7) | (ch << 3))};
				if (!mfFailed_ && !mf_.Open(MFDecoder::Format::AAC_RAW, rate_, ch, asc, 2)) mfFailed_ = true;
			}
			pcm_.clear();
			if (mf_.IsOpen()) mf_.Decode(inbuf, inbytes, pcm_);
		}
		const int mfCh = std::max(1, mf_.IsOpen() ? mf_.Channels() : 2);
		int frames = (int)(pcm_.size() / mfCh);
		if (!mf_.IsOpen()) frames = 1024;  // no decoder: silence of the right length
		frames = std::min(frames, 2048);
		if (outbuf) {
			if (mf_.IsOpen()) {
				ConvertChannels(pcm_.data(), mfCh, outbuf, outputChannels, frames);
			} else {
				memset(outbuf, 0, frames * 2 * outputChannels);
			}
		}
		if (inbytesConsumed) *inbytesConsumed = consumed;
		if (outSamples) *outSamples = frames;
		return true;
	}

private:
	enum class Mode { None, Adts, Raw };
	bool adtsStream_;
	MFDecoder mf_;
	bool mfFailed_ = false;
	Mode mode_ = Mode::None;
	int rate_, channels_;
	std::vector<s16> pcm_;
};

}  // namespace

AudioDecoder *CreateAtrac3Audio(int channels, int blockAlign, const u8 *extraData, int extraDataSize) {
	return new Atrac3Audio(PSP_CODEC_AT3, channels, blockAlign, extraData, extraDataSize);
}

AudioDecoder *CreateAtrac3PlusAudio(int channels, int blockAlign) {
	return new Atrac3Audio(PSP_CODEC_AT3PLUS, channels, blockAlign, nullptr, 0);
}

AudioDecoder *CreateMp3Audio() { return new Mp3Audio(); }

AudioDecoder *CreateAacAudio(int sampleRateHz, int channels, bool adtsStream) { return new AacAudio(sampleRateHz, channels, adtsStream); }

AudioDecoder *CreateAudioDecoder(CodecType type, int sampleRateHz, int channels, int blockAlign, const u8 *extraData,
                                 int extraDataSize) {
	switch (type) {
	case PSP_CODEC_AT3: return CreateAtrac3Audio(channels, blockAlign, extraData, extraDataSize);
	case PSP_CODEC_AT3PLUS: return CreateAtrac3PlusAudio(channels, blockAlign);
	case PSP_CODEC_MP3: return CreateMp3Audio();
	case PSP_CODEC_AAC:
	default: return CreateAacAudio(sampleRateHz, channels);
	}
}

bool ParseMp3Header(const u8 *p, Mp3FrameInfo *info) {
	if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
	const int version = (p[1] >> 3) & 3;
	const int layer = (p[1] >> 1) & 3;
	const int brIndex = (p[2] >> 4) & 0xF;
	const int srIndex = (p[2] >> 2) & 3;
	if (version == 1 || layer == 0 || brIndex == 15 || srIndex == 3) return false;
	static const int rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
	static const int brV1[3][16] = {
		{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},      // Layer III
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},     // Layer II
		{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},  // Layer I
	};
	static const int brV2[3][16] = {
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
	};
	info->versionBits = version;
	info->layerBits = layer;
	info->sampleRate = rates[version][srIndex];
	info->bitrateKbps = version == 3 ? brV1[layer - 1][brIndex] : brV2[layer - 1][brIndex];
	info->channelMode = (p[3] >> 6) & 3;
	info->channels = info->channelMode == 3 ? 1 : 2;
	info->padding = (p[2] >> 1) & 1;
	if (layer == 3) {
		info->samplesPerFrame = 384;
	} else if (layer == 2 || version == 3) {
		info->samplesPerFrame = 1152;
	} else {
		info->samplesPerFrame = 576;
	}
	if (info->bitrateKbps == 0) {
		info->frameBytes = 0;
	} else if (layer == 3) {
		info->frameBytes = (12 * info->bitrateKbps * 1000 / info->sampleRate + info->padding) * 4;
	} else {
		const int coef = (layer == 1 && version != 3) ? 72 : 144;
		info->frameBytes = coef * info->bitrateKbps * 1000 / info->sampleRate + info->padding;
	}
	return true;
}

}  // namespace audio
