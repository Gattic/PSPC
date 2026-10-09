// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceAudiocodec: frame-by-frame ATRAC3/ATRAC3+/MP3/AAC decoding on a game-owned codec context
// (what libatrac3plus, libmp3, mpeg.prx and libmp4 sit on).
// Ported from PPSSPP's Core/HLE/sceAudiocodec.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later), which documents the context fields,
// plus what pspautotests audio/audiocodec/context and at3errors show the hardware writing back
// (bitrate, sample rate, channel and frame-size words; MP3 header fields; ATRAC3 mono layouts).
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "audio/decoder.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/kernel/errors.h"

using namespace audio;

namespace audiohle {

namespace {

struct DecoderEntry {
	std::unique_ptr<AudioDecoder> decoder;
	int at3PlusFrameBytes = 0;
	// Successfully decoded frames that will still produce no output (ATRAC3+ drops 1, AAC 2).
	int primingFrames = 0;
};

std::map<u32, DecoderEntry> g_decoders;

SceAudiocodecCodec *CodecCtx(u32 ptr) { return (SceAudiocodecCodec *)mem::GetPointer(ptr, sizeof(SceAudiocodecCodec)); }

// A context word by byte offset (the per-codec union is mostly unnamed fields).
u32 &Word(SceAudiocodecCodec *ctx, int off) { return *(u32 *)((u8 *)ctx + off); }

// ---- ATRAC3+ ----

struct At3PlusLayout {
	int channels;    // channel configuration (bits 2-4 of byte 1)
	int rateIndex;   // bits 5-7 of byte 1: 0 = 32kHz, 1 = 44.1kHz, 2 = 48kHz
	int frameBytes;  // ((byte 1 & 3) << 8 | byte 2) * 8 + 8
};

At3PlusLayout At3PlusFormat(u8 formatByte1, u8 formatByte2) {
	At3PlusLayout l;
	l.channels = (formatByte1 >> 2) & 7;
	l.rateIndex = formatByte1 >> 5;
	l.frameBytes = (((formatByte1 & 3) << 8) | formatByte2) * 8 + 8;
	return l;
}

int At3PlusRate(int rateIndex) {
	static const int rates[8] = {32000, 44100, 48000, 0, 0, 0, 0, 0};
	return rates[rateIndex & 7];
}

// Output channels per channel configuration (5, 6, 7 are 5.1, 6.1 and 7.1).
int At3PlusOutputChannels(int config) {
	// Mono (1) is decoded to stereo unless InitMono asked for mono.
	static const int out[8] = {0, 2, 2, 3, 4, 6, 7, 8};
	return out[config & 7];
}

// For a decode (headerBytes non-null) also handles the 0x0FD0 frame header and returns the ME
// error for a frame that doesn't match the context, or 0.
int CalculateInputBytesAndChannelsAt3Plus(const SceAudiocodecCodec *ctx, int *inputBytes, int *channels, int *headerBytes = nullptr) {
	At3PlusLayout l = At3PlusFormat(ctx->fmt.at3.formatByte1, ctx->fmt.at3.formatByte2);
	*inputBytes = l.frameBytes;
	*channels = l.channels;
	if (!headerBytes) return 0;
	*headerBytes = 0;
	if (ctx->fmt.at3.at3Related == 0) return 0;
	const u8 *frame = mem::GetPointer(ctx->inBuf, 8);
	if (!frame || frame[0] != 0x0F || frame[1] != 0xD0) return 0x211;
	At3PlusLayout h = At3PlusFormat(frame[2], frame[3]);
	*inputBytes = h.frameBytes;
	if (h.channels != *channels) return 0x213;
	*headerBytes = 8;
	return 0;
}

// ---- ATRAC3 ----

struct At3Param {
	u8 param;
	u16 bytes;
	u8 channels;
	u8 jointStereo;
	u16 perChannel;  // what Init writes back at 0x30
};

// The parameter at 0x28 as the ME decodes it. 04/06/0B/0E/0F were decoded on hardware; the others
// are known to init (and their per-channel sizes are what it writes back), the layout is a guess.
const At3Param at3Layouts[] = {
	{0x00, 0x0200, 2, 0, 0x100}, {0x03, 0x01A8, 2, 0, 0xD4}, {0x04, 0x0180, 2, 0, 0xC0}, {0x06, 0x0130, 2, 0, 0x98},
	{0x09, 0x0110, 2, 0, 0x88}, {0x0B, 0x00C0, 2, 1, 0x60}, {0x0C, 0x0088, 2, 0, 0x44}, {0x0D, 0x0060, 2, 0, 0x30},
	{0x0E, 0x00C0, 1, 0, 0xC0}, {0x0F, 0x0098, 1, 0, 0x98},
};

// libatrac3plus.prx's own table, keyed by frame size and joint stereo (sceAtrac uses it).
const At3Param libatracParams[] = {
	{0x04, 0x0180, 2, 0, 0xC0}, {0x06, 0x0130, 2, 0, 0x98}, {0x0B, 0x00C0, 2, 1, 0x60}, {0x0E, 0x00C0, 1, 0, 0xC0}, {0x0F, 0x0098, 1, 0, 0x98},
};

// Returns false for a parameter the hardware rejects (above 0x0F).
bool Atrac3LayoutFromContext(const SceAudiocodecCodec *ctx, At3Param *layout) {
	u32 param;
	memcpy(&param, ctx->fmt.raw, 4);
	for (const At3Param &p : at3Layouts) {
		if (p.param == param) {
			*layout = p;
			return true;
		}
	}
	// Unknown: fall back to 132kbps stereo, the most common.
	*layout = at3Layouts[2];
	return param <= 0x0F;
}

// A mono layout is decoded to stereo only when the word at 0x34 is 2, whichever init set it.
int At3OutputChannels(SceAudiocodecCodec *ctx, const At3Param &layout) {
	return layout.channels == 1 && Word(ctx, 0x34) != 2 ? 1 : 2;
}

// ---- MP3 / AAC ----

int AacSampleRateFromContext(const SceAudiocodecCodec *ctx) {
	// 12000 is refused on hardware even though it's a standard AAC rate.
	static const int aacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 11025, 8000, 7350};
	for (int rate : aacRates) {
		if ((int)ctx->fmt.aac.sampleRate == rate) return rate;
	}
	return 0;
}

bool RemoveDecoder(u32 ctxPtr) { return g_decoders.erase(ctxPtr) != 0; }

// Measured at 222MHz (pspautotests audio/audiocodec/timing).
int MECall(u32 result, int us) { return (int)DelayResult(result, MEScheduleJob(us)); }

int InitUs(int codec, const SceAudiocodecCodec *ctx) {
	return AudioCodecInitUs(codec, codec == PSP_CODEC_AT3PLUS && ((ctx->fmt.at3.formatByte1 >> 2) & 7) == 1);
}

AudioDecoder *CreateDecoderForContext(u32 ctxPtr, CodecType type) {
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	int bytesPerFrame = 0;
	int channels = 2;
	int sampleRate = 44100;
	u8 extraData[14]{};
	switch (type) {
	case PSP_CODEC_AT3PLUS: CalculateInputBytesAndChannelsAt3Plus(ctx, &bytesPerFrame, &channels); break;
	case PSP_CODEC_AT3: {
		At3Param l;
		Atrac3LayoutFromContext(ctx, &l);
		bytesPerFrame = l.bytes;
		channels = l.channels;
		extraData[0] = 1;
		extraData[3] = (u8)(channels << 3);
		extraData[6] = l.jointStereo;
		extraData[8] = l.jointStereo;
		extraData[10] = 1;
		break;
	}
	case PSP_CODEC_AAC:
		if (AacSampleRateFromContext(ctx)) sampleRate = AacSampleRateFromContext(ctx);
		break;
	default: break;
	}
	DecoderEntry &e = g_decoders[ctxPtr];
	e.decoder.reset(CreateAudioDecoder(type, sampleRate, channels, bytesPerFrame, extraData, sizeof(extraData)));
	e.decoder->SetCtxPtr(ctxPtr);
	e.at3PlusFrameBytes = bytesPerFrame;
	return e.decoder.get();
}

int AudioCodecInitCommon(u32 ctxPtr, int codec, bool mono) {
	EnsureInit();
	if (!IsValidCodec(codec)) return (int)SCE_KERNEL_ERROR_OUT_OF_RANGE;
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	if (!ctx) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	// Re-initialising a context that still has a decoder is normal (mpeg.prx does it).
	RemoveDecoder(ctxPtr);
	int primingFrames = 0;
	switch (codec) {
	case PSP_CODEC_MP3:
		ctx->fmt.mp3.version = 9999;
		if (mono) return (int)SCE_AVCODEC_ERROR_UNSUPPORTED;
		break;
	case PSP_CODEC_AAC:
		// A rate not in the list is refused at once; the flag bytes only by the ME.
		if (mono || !AacSampleRateFromContext(ctx)) return (int)SCE_AVCODEC_ERROR_UNSUPPORTED;
		if (ctx->fmt.aac.unk2c || ctx->fmt.aac.unk2d) return MECall(SCE_AVCODEC_ERROR_UNSUPPORTED, InitUs(codec, ctx));
		primingFrames = 2;
		break;
	case PSP_CODEC_AT3PLUS: {
		const At3PlusLayout l = At3PlusFormat(ctx->fmt.at3.formatByte1, ctx->fmt.at3.formatByte2);
		if (At3PlusRate(l.rateIndex) == 0) return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		if (l.channels == 0) {
			ctx->err = 0x202;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		}
		// InitMono (libatrac3plus' MOut functions) only takes mono streams.
		if (mono && l.channels != 1) return MECall(SCE_AVCODEC_ERROR_UNSUPPORTED, InitUs(codec, ctx));
		Word(ctx, 0x48) = (u32)(mono ? 1 : At3PlusOutputChannels(l.channels));
		const int rate = At3PlusRate(l.rateIndex);
		if (rate == 32000) {
			ctx->err = 0x201;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		}
		if (l.frameBytes > 0x1000) {
			ctx->err = 0x203;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		}
		// The bitrate in kbps.
		Word(ctx, 0x44) = (u32)((u64)l.frameBytes * 8 * rate / 2048 / 1000);
		// Init wants inited to be exactly 1.
		if (ctx->inited != 1) {
			ctx->err = 0x8001;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		}
		primingFrames = 1;
		break;
	}
	case PSP_CODEC_AT3: {
		At3Param l;
		if (!Atrac3LayoutFromContext(ctx, &l)) {
			ctx->err = 0x186;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		}
		if (mono && l.channels != 1) return (int)SCE_AVCODEC_ERROR_UNSUPPORTED;
		Word(ctx, 0x2c) = 44100;
		Word(ctx, 0x30) = l.perChannel;
		if (l.channels == 1) Word(ctx, 0x34) = mono ? 1 : 2;
		break;
	}
	default: break;
	}
	ctx->magic = 0x5100601;
	ctx->err = 0;
	CreateDecoderForContext(ctxPtr, (CodecType)codec);
	g_decoders[ctxPtr].primingFrames = primingFrames;
	return MECall(0, InitUs(codec, ctx));
}

// ME time for one frame at 222MHz, fitted to sceAudiocodecDecode timings on a PSP.
int EstimateDecodeUs(int codec, int channels, int frameBytes, bool mpeg1) {
	switch (codec) {
	case PSP_CODEC_AT3PLUS: return channels == 1 ? 1500 + frameBytes * 84 / 100 : 1815 + frameBytes * 158 / 100;
	case PSP_CODEC_AT3: return channels == 1 ? 685 : 970 + frameBytes * 44 / 100;
	case PSP_CODEC_MP3: return mpeg1 ? 2492 + frameBytes / 5 : 1358 + frameBytes / 2;
	case PSP_CODEC_AAC: return 1400 + std::min(frameBytes, 400) * 16 / 10;
	default: return 0;
	}
}

int sceAudiocodecInit(u32 ctxPtr, int codec) { return AudioCodecInitCommon(ctxPtr, codec, false); }
int sceAudiocodecInitMono(u32 ctxPtr, int codec) { return AudioCodecInitCommon(ctxPtr, codec, true); }

int sceAudiocodecDecode(u32 ctxPtr, int codec) {
	if (!ctxPtr) return -1;
	if (!IsValidCodec(codec)) return 0;
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	if (!ctx) return -1;

	int bytesPerFrame = 0;
	int channels = 2;
	int outChannels = 2;
	int headerBytes = 0;
	At3Param at3{};
	switch (codec) {
	case PSP_CODEC_AT3PLUS: {
		const int frameError = CalculateInputBytesAndChannelsAt3Plus(ctx, &bytesPerFrame, &channels, &headerBytes);
		if (frameError) {
			ctx->err = frameError;
			ctx->srcBytesRead = 0;
			ctx->dstBytesWritten = 0;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 142);
		}
		// The output channel count is read from the context at every decode.
		outChannels = Word(ctx, 0x48) == 1 ? 1 : 2;
		break;
	}
	case PSP_CODEC_MP3:
		// The bound at 0x28 (0x5A1), which the caller guarantees is readable at inBuf.
		bytesPerFrame = (int)ctx->fmt.mp3.maxFrameBytes;
		if (bytesPerFrame <= 0) bytesPerFrame = 0x5A1;
		break;
	case PSP_CODEC_AAC: {
		bytesPerFrame = ctx->fmt.aac.unk2c ? 0x609 : 0x600;
		// The ME's decoder delimits the raw access unit itself; ours needs its exact length.
		// When the AUs sit in an ADTS stream (the header just before inBuf) take it from there.
		if (const u8 *h = mem::GetPointer(ctx->inBuf - 7, 7)) {
			const int len = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
			if (h[0] == 0xFF && (h[1] & 0xF6) == 0xF0 && len > 7 && len - 7 <= bytesPerFrame) bytesPerFrame = len - 7;
		}
		break;
	}
	case PSP_CODEC_AT3:
		// Decode goes by the parameter, not by what Init wrote back.
		Atrac3LayoutFromContext(ctx, &at3);
		bytesPerFrame = at3.bytes;
		channels = at3.channels;
		outChannels = At3OutputChannels(ctx, at3);
		break;
	}

	auto it = g_decoders.find(ctxPtr);
	AudioDecoder *decoder = it != g_decoders.end() ? it->second.decoder.get() : nullptr;

	if (decoder && codec == PSP_CODEC_AT3PLUS && bytesPerFrame > 0 && it->second.at3PlusFrameBytes != bytesPerFrame) {
		// Only when the context had no frame size at init (mpeg.prx). One decoder throughout on
		// hardware, so the priming carries over.
		it->second.decoder.reset(CreateAudioDecoder(PSP_CODEC_AT3PLUS, 44100, channels, bytesPerFrame));
		it->second.decoder->SetCtxPtr(ctxPtr);
		it->second.at3PlusFrameBytes = bytesPerFrame;
		decoder = it->second.decoder.get();
	}
	if (!decoder) return 0;

	const u32 inAddr = ctx->inBuf + (u32)headerBytes;
	u32 avail = bytesPerFrame > 0 ? ClampValidSize(inAddr, (u32)bytesPerFrame) : 0;
	// MP3 frames are measured by the decoder itself, so a short readable span is fine there.
	if (codec != PSP_CODEC_MP3 && avail != (u32)bytesPerFrame) avail = 0;
	const u8 *inBuf = avail ? mem::GetPointer(inAddr, avail) : nullptr;
	if (!inBuf) {
		ctx->err = 0x20b;
		return 0;
	}

	if (codec == PSP_CODEC_AT3 && at3.jointStereo && (inBuf[0] >> 2) == 0x28 && inBuf[bytesPerFrame - 1] != 0xF8) {
		// A valid first unit, but the second (stored reversed from the end) lacks its 0xF8 marker.
		ctx->err = 0x183;
		ctx->srcBytesRead = 0;
		ctx->dstBytesWritten = 0;
		return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 169);
	}

	int inDataConsumed = 0;
	int outSamples = 0;
	std::vector<s16> out(8192 * 2);
	bool result = decoder->Decode(inBuf, (int)avail, &inDataConsumed, 2, out.data(), &outSamples);
	if (!result && (codec == PSP_CODEC_AT3PLUS || codec == PSP_CODEC_AT3)) {
		// What the hardware reports for a bad frame (0x208 is also possible for ATRAC3+).
		ctx->err = codec == PSP_CODEC_AT3PLUS ? 0x20a : 0x182;
		ctx->srcBytesRead = 0;
		ctx->dstBytesWritten = 0;
		return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, codec == PSP_CODEC_AT3PLUS ? 214 : 169);
	}
	if (!result) {
		ctx->err = 0x20b;
	} else {
		ctx->err = 0;
		if (it->second.primingFrames > 0) {
			it->second.primingFrames--;
			outSamples = 0;
		}
	}
	outSamples = std::min(outSamples, 8192);
	if (outChannels == 1) {
		for (int i = 0; i < outSamples; i++) out[i] = (s16)(((int)out[i * 2] + out[i * 2 + 1]) >> 1);
	}
	const u32 outBytes = (u32)outSamples * outChannels * sizeof(s16);
	if (outBytes) {
		if (u8 *o = mem::GetPointer(ctx->outBuf, outBytes)) memcpy(o, out.data(), outBytes);
	}
	ctx->srcBytesRead = inDataConsumed + headerBytes;
	// In bytes, like sceAudiocodecGetOutputBytes.
	ctx->dstBytesWritten = (s32)outBytes;

	bool mpeg1 = true;
	if (codec == PSP_CODEC_MP3 && result) {
		// Each decode reads the frame header into the context.
		Mp3FrameInfo info;
		bool found = false;
		for (int i = 0; !found && i + 4 <= (int)avail; i++) found = ParseMp3Header(inBuf + i, &info);
		if (found) {
			mpeg1 = info.versionBits == 3;
			Word(ctx, 0x30) = (u32)info.channels;
			Word(ctx, 0x34) = mpeg1 ? 2 : 1;  // granules per frame
			Word(ctx, 0x4c) = (u32)info.padding;
		}
	}
	const int decodeUs = EstimateDecodeUs(codec, channels, inDataConsumed, mpeg1);
	if (decodeUs > 0) return (int)DelayResult(0, MEScheduleJob(decodeUs));
	return 0;
}

int sceAudiocodecGetInfo(u32 ctxPtr, int codec) {
	if (codec < 0x1000 || codec >= 0x1006) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	if (ctx && codec == PSP_CODEC_MP3) {
		// Fill in what libmp3 reads back, from the frame header like the hardware does. Apart from
		// the version index (0 = MPEG2, 1 = MPEG1, 2 = MPEG2.5) these are the raw header fields.
		static const int versionFromHeader[4] = {2, -1, 0, 1};
		const u8 *header = mem::GetPointer(ctx->inBuf, 4);
		const bool haveFrame = ctx->inBuf && header && header[0] == 0xFF && (header[1] & 0xE0) == 0xE0 &&
		                       versionFromHeader[(header[1] >> 3) & 3] >= 0;
		if (!haveFrame) {
			// Nothing to read: everything is marked unknown.
			ctx->err = -2;
			Word(ctx, 0x2c) = 0xFFFFFFFF;
			for (int off = 0x38; off <= 0x64; off += 4) Word(ctx, off) = 0xFFFFFFFF;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 82);
		}
		ctx->fmt.mp3.version = versionFromHeader[(header[1] >> 3) & 3];
		ctx->fmt.mp3.unk3c = 3;
		ctx->fmt.mp3.bitrateIndex = (header[2] >> 4) & 0x0F;
		ctx->fmt.mp3.sampleRateIndex = (header[2] >> 2) & 0x03;
		ctx->fmt.mp3.channelConfig = (header[3] >> 6) & 0x03;
		ctx->fmt.mp3.unk58 = (header[3] >> 4) & 0x03;  // mode extension
		ctx->fmt.mp3.unk60 = 1;
	}
	if (codec == PSP_CODEC_MP3 || codec == PSP_CODEC_AT3PLUS) return MECall(0, 82);
	return 0;
}

int sceAudiocodecCheckNeedMem(u32 ctxPtr, int codec) {
	if (codec < 0x1000 || codec >= 0x1006) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	if (!ctx) return 0;
	switch (codec) {
	case 0x1000: {
		ctx->magic = 0x5100601;
		const At3PlusLayout l = At3PlusFormat(ctx->fmt.at3.formatByte1, ctx->fmt.at3.formatByte2);
		Word(ctx, 0x3c) = (u32)l.channels;
		const int rate = At3PlusRate(l.rateIndex);
		if (rate == 0) return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 147);
		Word(ctx, 0x34) = (u32)rate;
		Word(ctx, 0x40) = (u32)l.frameBytes;
		if (l.channels == 0) {
			ctx->err = 0x20f;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 147);
		}
		static const s32 needMem[8] = {0, 0x4340, 0x7bc0, 0xbe40, 0x100c0, 0x17bc0, 0x1be40, 0x1f6c0};
		ctx->neededMem = needMem[l.channels];
		break;
	}
	case 0x1001: ctx->neededMem = 0x3de0; break;
	case 0x1002: ctx->neededMem = 0x3b68; break;
	case 0x1003: ctx->neededMem = 0x658c; break;
	case 0x1004: return (int)SCE_AVCODEC_ERROR_UNSUPPORTED;
	case 0x1005:
		// What the hardware does without asking the ME.
		ctx->neededMem = (s32)0x80000003;
		ctx->err = 0;
		return 0;
	}
	ctx->err = 0;
	ctx->magic = 0x5100601;
	static const int needMemUs[4] = {147, 80, 79, 57};
	return MECall(0, needMemUs[codec - 0x1000]);
}

int sceAudiocodecGetEDRAM(u32 ctxPtr, int codec) {
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	if (!ctx) return 0;
	ctx->allocMem = 0x0018ea90;
	ctx->edramAddr = (ctx->allocMem + 0x3f) & ~0x3fu;
	return MECall(0, 56);
}

// One parameter only (the real module reads just a0).
int sceAudiocodecReleaseEDRAM(u32 ctxPtr) {
	if (SceAudiocodecCodec *ctx = CodecCtx(ctxPtr)) {
		ctx->edramAddr = 0;
		ctx->allocMem = 0;
	}
	RemoveDecoder(ctxPtr);
	return MECall(0, 56);
}

// Also known as sceAudiocodecGetBlockSize.
int sceAudiocodecGetOutputBytes(u32 ctxPtr, int codec, u32 outBytesAddr) {
	if ((outBytesAddr & 3) != 0 || !mem::IsValidRange(outBytesAddr, 4)) return (int)SCE_MP3_ERROR_BAD_ADDR;
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	u32 bytes;
	switch (codec) {
	case PSP_CODEC_AT3PLUS: bytes = ctx ? 0x1000 * std::min<u32>(Word(ctx, 0x48), 8) : 0x2000; break;
	case PSP_CODEC_AT3: bytes = 0x1000; break;
	case PSP_CODEC_MP3:
		if (ctx && ctx->fmt.mp3.version == 9999) {
			// Not known until GetInfo has looked at a frame.
			ctx->err = -21;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 82);
		}
		bytes = ctx && ctx->fmt.mp3.version != 1 ? 0x900 : 0x1200;
		break;
	case PSP_CODEC_AAC: bytes = ctx && ctx->fmt.aac.unk2d ? 0x2000 : 0x1000; break;
	default: return 0;
	}
	GWrite32(outBytesAddr, bytes);
	return 0;
}

}  // namespace

int AudioCodecInitUs(int codec, bool monoAt3Plus) {
	switch (codec) {
	case PSP_CODEC_AT3PLUS: return monoAt3Plus ? 524 : 646;
	case PSP_CODEC_AT3: return 210;
	case PSP_CODEC_MP3: return 517;
	default: return 230;
	}
}

int AudioCodecDecodeUs(int codec, int channels, int frameBytes) { return EstimateDecodeUs(codec, channels, frameBytes, true); }

// libatrac3plus picks the layout by frame size and joint stereo, scanning from the last entry;
// the channel count plays no part (LocoRoco 2's mono 0xC0 track says 2 channels).
bool Atrac3DecoderChannels(int bytesPerFrame, bool jointStereo, int *channels) {
	for (int i = (int)(sizeof(libatracParams) / sizeof(libatracParams[0])) - 1; i >= 0; i--) {
		if (libatracParams[i].bytes == bytesPerFrame && (libatracParams[i].jointStereo != 0) == jointStereo) {
			*channels = libatracParams[i].channels;
			return true;
		}
	}
	return false;
}

bool IsAtrac3StreamJointStereo(int codecType, int bytesPerFrame, int channels) {
	if (codecType != PSP_CODEC_AT3) return false;
	for (const At3Param &p : libatracParams) {
		if (p.bytes == bytesPerFrame && p.channels == channels) return p.jointStereo != 0;
	}
	return false;
}

void Register_sceAudiocodec() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x70A703F8, sceAudiocodecDecode),
		HLEFN(0x5B37EB1D, sceAudiocodecInit),
		HLEFN(0x8ACA11D5, sceAudiocodecGetInfo),
		HLEFN(0x3A20A200, sceAudiocodecGetEDRAM),
		HLEFN(0x29681260, sceAudiocodecReleaseEDRAM),
		HLEFN(0x9D3F790C, sceAudiocodecCheckNeedMem),
		HLEFN(0x59176A0F, sceAudiocodecGetOutputBytes),
		HLEFN(0x3DD7EE1A, sceAudiocodecInitMono),
	};
	hle::RegisterModule("sceAudiocodec", funcs);
	hle::RegisterModule("sceAudiocodec_driver", funcs);
}

}  // namespace audiohle
