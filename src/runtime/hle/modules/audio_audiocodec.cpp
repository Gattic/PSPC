// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceAudiocodec: frame-by-frame ATRAC3/ATRAC3+/MP3/AAC decoding on a game-owned codec context
// (what libatrac3plus, libmp3, mpeg.prx and libmp4 sit on).
// Ported from PPSSPP's Core/HLE/sceAudiocodec.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later), which documents the context fields.
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

// ATRAC3+ frame size and channels from the two format bytes.
void At3PlusFormat(u8 formatByte1, u8 formatByte2, int *inputBytes, int *channels) {
	*channels = (formatByte1 >> 2) & 7;
	*inputBytes = (((formatByte1 & 3) << 8) | formatByte2) * 8 + 8;
}

// For a decode (headerBytes non-null) also handles the 0x0FD0 frame header and returns the ME
// error for a frame that doesn't match the context, or 0.
int CalculateInputBytesAndChannelsAt3Plus(const SceAudiocodecCodec *ctx, int *inputBytes, int *channels, int *headerBytes = nullptr) {
	At3PlusFormat(ctx->fmt.at3.formatByte1, ctx->fmt.at3.formatByte2, inputBytes, channels);
	if (!headerBytes) return 0;
	*headerBytes = 0;
	if (ctx->fmt.at3.at3Related == 0) return 0;
	const u8 *frame = mem::GetPointer(ctx->inBuf, 8);
	if (!frame || frame[0] != 0x0F || frame[1] != 0xD0) return 0x211;
	int frameChannels;
	At3PlusFormat(frame[2], frame[3], inputBytes, &frameChannels);
	if (frameChannels != *channels) return 0x213;
	*headerBytes = 8;
	return 0;
}

struct At3Param {
	u8 param;
	u16 bytes;
	u8 channels;
	u8 jointStereo;
};

// The ATRAC3 parameter at 0x28, as libatrac3plus's table maps it from frame size and joint stereo.
const At3Param at3Params[] = {
	{0x04, 0x0180, 2, 0}, {0x06, 0x0130, 2, 0}, {0x0B, 0x00C0, 2, 1}, {0x0E, 0x00C0, 1, 0}, {0x0F, 0x0098, 1, 0},
};

bool Atrac3LayoutFromContext(const SceAudiocodecCodec *ctx, int *bytesPerFrame, int *channels, bool *jointStereo) {
	u32 param;
	memcpy(&param, ctx->fmt.raw, 4);
	for (const At3Param &p : at3Params) {
		if (p.param == param) {
			*bytesPerFrame = p.bytes;
			*channels = p.channels;
			*jointStereo = p.jointStereo != 0;
			return true;
		}
	}
	// Not in the table: fall back to 132kbps stereo, the most common.
	*bytesPerFrame = 0x180;
	*channels = 2;
	*jointStereo = false;
	return param <= 0x0F;
}

const int g_mpegSampleRates[3][4] = {
	{22050, 24000, 16000, 0},
	{44100, 48000, 32000, 0},
	{11025, 12000, 8000, 0},
};

int Mp3SampleRateFromContext(const SceAudiocodecCodec *ctx) {
	const int version = ctx->fmt.mp3.version;
	const int index = ctx->fmt.mp3.sampleRateIndex;
	if (version < 0 || version >= 3 || index < 0 || index >= 4) return 0;
	return g_mpegSampleRates[version][index];
}

int Mp3ChannelsFromContext(const SceAudiocodecCodec *ctx) { return ctx->fmt.mp3.channelConfig == 3 ? 1 : 2; }

int AacSampleRateFromContext(const SceAudiocodecCodec *ctx) {
	static const int aacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
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
		bool jointStereo;
		Atrac3LayoutFromContext(ctx, &bytesPerFrame, &channels, &jointStereo);
		extraData[0] = 1;
		extraData[3] = (u8)(channels << 3);
		extraData[6] = jointStereo;
		extraData[8] = jointStereo;
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
	ctx->magic = 0x5100601;
	ctx->err = 0;
	int primingFrames = 0;
	switch (codec) {
	case PSP_CODEC_MP3: ctx->fmt.mp3.version = 9999; break;
	case PSP_CODEC_AAC:
		if (!AacSampleRateFromContext(ctx)) return MECall(SCE_AVCODEC_ERROR_UNSUPPORTED, InitUs(codec, ctx));
		primingFrames = 2;
		break;
	case PSP_CODEC_AT3PLUS: {
		int bytesPerFrame, channels;
		CalculateInputBytesAndChannelsAt3Plus(ctx, &bytesPerFrame, &channels);
		if (channels == 0) {
			ctx->err = 0x202;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		}
		primingFrames = 1;
		break;
	}
	case PSP_CODEC_AT3: {
		int bytesPerFrame, channels;
		bool jointStereo;
		if (!Atrac3LayoutFromContext(ctx, &bytesPerFrame, &channels, &jointStereo)) {
			ctx->err = 0x186;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, InitUs(codec, ctx));
		}
		break;
	}
	default: break;
	}
	CreateDecoderForContext(ctxPtr, (CodecType)codec);
	g_decoders[ctxPtr].primingFrames = primingFrames;
	return MECall(0, InitUs(codec, ctx));
}

// ME time for one frame at 222MHz, fitted to sceAudiocodecDecode timings on a PSP.
int EstimateDecodeUs(int codec, int channels, int frameBytes, const SceAudiocodecCodec *ctx) {
	switch (codec) {
	case PSP_CODEC_AT3PLUS: return channels == 1 ? 1500 + frameBytes * 84 / 100 : 1815 + frameBytes * 158 / 100;
	case PSP_CODEC_AT3: return channels == 1 ? 685 : 970 + frameBytes * 44 / 100;
	case PSP_CODEC_MP3: return (ctx && ctx->fmt.mp3.version == 1) ? 2492 + frameBytes / 5 : 1358 + frameBytes / 2;
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
	int headerBytes = 0;
	switch (codec) {
	case PSP_CODEC_AT3PLUS: {
		const int frameError = CalculateInputBytesAndChannelsAt3Plus(ctx, &bytesPerFrame, &channels, &headerBytes);
		if (frameError) {
			ctx->err = frameError;
			ctx->srcBytesRead = 0;
			ctx->dstBytesWritten = 0;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 142);
		}
		break;
	}
	case PSP_CODEC_MP3:
		// The bound at 0x28 (0x5A1), which the caller guarantees is readable at inBuf.
		bytesPerFrame = (int)ctx->fmt.mp3.maxFrameBytes;
		if (bytesPerFrame <= 0) bytesPerFrame = ctx->srcBytesRead;
		channels = Mp3ChannelsFromContext(ctx);
		break;
	case PSP_CODEC_AAC: bytesPerFrame = ctx->fmt.aac.unk2c ? 0x609 : 0x600; break;
	case PSP_CODEC_AT3: {
		bool jointStereo;
		Atrac3LayoutFromContext(ctx, &bytesPerFrame, &channels, &jointStereo);
		break;
	}
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

	int decodeUs = 0;
	if (decoder) {
		int inDataConsumed = 0;
		int outSamples = 0;
		const u32 inAddr = ctx->inBuf + (u32)headerBytes;
		u32 avail = bytesPerFrame > 0 ? ClampValidSize(inAddr, (u32)bytesPerFrame) : 0;
		// MP3 frames are measured by the decoder itself, so a short readable span is fine there.
		if (codec != PSP_CODEC_MP3 && avail != (u32)bytesPerFrame) avail = 0;
		const u8 *inBuf = avail ? mem::GetPointer(inAddr, avail) : nullptr;
		if (!inBuf) {
			ctx->err = 0x20b;
			return 0;
		}
		std::vector<s16> out(4096 * 2);
		bool result = decoder->Decode(inBuf, (int)avail, &inDataConsumed, 2, out.data(), &outSamples);
		if (!result && (codec == PSP_CODEC_AT3PLUS || codec == PSP_CODEC_AT3)) {
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
		const u32 outBytes = (u32)outSamples * 2 * sizeof(s16);
		if (outBytes) {
			if (u8 *o = mem::GetPointer(ctx->outBuf, outBytes)) memcpy(o, out.data(), outBytes);
		}
		ctx->srcBytesRead = inDataConsumed + headerBytes;
		// In bytes; the decoder always writes stereo 16-bit.
		ctx->dstBytesWritten = (s32)outBytes;
		decodeUs = EstimateDecodeUs(codec, channels, inDataConsumed, ctx);
	}
	if (decodeUs > 0) return (int)DelayResult(0, MEScheduleJob(decodeUs));
	return 0;
}

int sceAudiocodecGetInfo(u32 ctxPtr, int codec) {
	if (codec < 0x1000 || codec >= 0x1006) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	if (ctx && codec == PSP_CODEC_MP3) {
		// Fill in what libmp3 reads back, from the frame header like the hardware does.
		ctx->fmt.mp3.unk3c = 3;
		ctx->fmt.mp3.unk60 = 1;
		static const int versionFromHeader[4] = {2, -1, 0, 1};
		const u8 *header = mem::GetPointer(ctx->inBuf, 4);
		const bool haveFrame = header && header[0] == 0xFF && (header[1] & 0xE0) == 0xE0 && versionFromHeader[(header[1] >> 3) & 3] >= 0;
		if (haveFrame) {
			ctx->fmt.mp3.version = versionFromHeader[(header[1] >> 3) & 3];
			ctx->fmt.mp3.bitrateIndex = (header[2] >> 4) & 0x0F;
			ctx->fmt.mp3.sampleRateIndex = (header[2] >> 2) & 0x03;
			ctx->fmt.mp3.channelConfig = (header[3] >> 6) & 0x03;
		} else {
			LOGW("AUDIOCODEC", "sceAudiocodecGetInfo: no MP3 frame at %08x, guessing 128kbps 44.1kHz stereo", ctx->inBuf);
			ctx->fmt.mp3.version = 1;
			ctx->fmt.mp3.bitrateIndex = 9;
			ctx->fmt.mp3.sampleRateIndex = 0;
			ctx->fmt.mp3.channelConfig = 1;
		}
	}
	if (codec == PSP_CODEC_MP3 || codec == PSP_CODEC_AT3PLUS) return MECall(0, 82);
	return 0;
}

int sceAudiocodecCheckNeedMem(u32 ctxPtr, int codec) {
	if (codec < 0x1000 || codec >= 0x1006) return (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
	SceAudiocodecCodec *ctx = CodecCtx(ctxPtr);
	if (!ctx) return 0;
	switch (codec) {
	case 0x1000:
		if (ctx->fmt.at3.formatByte1 == 0 && ctx->fmt.at3.formatByte2 == 0) {
			ctx->err = 0x20f;
			return MECall(SCE_AVCODEC_ERROR_INVALID_DATA, 147);
		}
		ctx->neededMem = 0x7bc0;
		break;
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
	ctx->allocMem = codec == PSP_CODEC_MP3 ? 0x001B3124 : 0x0018ea90;
	ctx->edramAddr = (ctx->allocMem + 0x3f) & ~0x3fu;
	return MECall(0, 56);
}

// One parameter only (the real module reads just a0).
int sceAudiocodecReleaseEDRAM(u32 ctxPtr) {
	if (SceAudiocodecCodec *ctx = CodecCtx(ctxPtr)) ctx->edramAddr = 0;
	RemoveDecoder(ctxPtr);
	return MECall(0, 56);
}

int sceAudiocodecGetOutputBytes(u32 ctxPtr, int codec, u32 outBytesAddr) {
	if ((outBytesAddr & 3) != 0 || !mem::IsValidRange(outBytesAddr, 4)) return (int)SCE_MP3_ERROR_BAD_ADDR;
	u32 bytes;
	switch (codec) {
	case PSP_CODEC_AT3PLUS: bytes = 0x2000; break;
	case PSP_CODEC_AT3: bytes = 0x1000; break;
	case PSP_CODEC_MP3: bytes = 0x1200; break;
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

int AudioCodecDecodeUs(int codec, int channels, int frameBytes) { return EstimateDecodeUs(codec, channels, frameBytes, nullptr); }

// libatrac3plus picks the layout by frame size and joint stereo, scanning from the last entry;
// the channel count plays no part (LocoRoco 2's mono 0xC0 track says 2 channels).
bool Atrac3DecoderChannels(int bytesPerFrame, bool jointStereo, int *channels) {
	for (int i = (int)(sizeof(at3Params) / sizeof(at3Params[0])) - 1; i >= 0; i--) {
		if (at3Params[i].bytes == bytesPerFrame && (at3Params[i].jointStereo != 0) == jointStereo) {
			*channels = at3Params[i].channels;
			return true;
		}
	}
	return false;
}

bool IsAtrac3StreamJointStereo(int codecType, int bytesPerFrame, int channels) {
	if (codecType != PSP_CODEC_AT3) return false;
	for (const At3Param &p : at3Params) {
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
