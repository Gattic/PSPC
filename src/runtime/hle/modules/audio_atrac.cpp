// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceAtrac3plus (libatrac3plus): the stateful ATRAC3/ATRAC3+ player API - full, half-way and
// streamed buffers, loops, the second buffer, the low-level decoder and the sceSas binding.
//
// Ported from PPSSPP's Core/HLE/sceAtrac.cpp and Core/HLE/AtracCtx2.cpp, the "Atrac2"
// implementation that keeps all state in the PSP-visible context exactly as the firmware does
// (https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
//
// Buffer states: NO_DATA (1), ALL_DATA_LOADED (2), HALFWAY_BUFFER (3), STREAMED_WITHOUT_LOOP (4),
// STREAMED_LOOP_FROM_END (5), STREAMED_LOOP_WITH_TRAILER (6, needs a second buffer), LOW_LEVEL
// (8) and FOR_SCESAS (16). See the overview comment in PPSSPP's sceAtrac.cpp.
#include <algorithm>
#include <string>
#include <vector>

#include "audio/atrac_track.h"
#include "audio/decoder.h"
#include "audio/sas.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

using namespace audio;

namespace audiohle {

namespace {

constexpr int PSP_MAX_ATRAC_IDS = 6;

enum AtracStatus : u8 {
	ATRAC_STATUS_UNINITIALIZED = 0,
	ATRAC_STATUS_NO_DATA = 1,
	ATRAC_STATUS_ALL_DATA_LOADED = 2,
	ATRAC_STATUS_HALFWAY_BUFFER = 3,
	ATRAC_STATUS_STREAMED_WITHOUT_LOOP = 4,
	ATRAC_STATUS_STREAMED_LOOP_FROM_END = 5,
	ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER = 6,
	ATRAC_STATUS_LOW_LEVEL = 8,
	ATRAC_STATUS_FOR_SCESAS = 16,
	ATRAC_STATUS_STREAMED_MASK = 4,
};

inline bool AtracStatusIsStreaming(u8 status) { return (status & ATRAC_STATUS_STREAMED_MASK) != 0; }

struct SceAtracIdInfo {
	s32 decodePos;         // next sample position to decode
	s32 endSample;         // last sample index
	s32 loopStart;
	s32 loopEnd;
	s32 firstValidSample;  // skipped samples at the start
	u8 numSkipFrames;
	u8 state;              // AtracStatus
	u8 curBuffer;          // 1 = second buffer, 2 = done with it
	u8 numChan;
	u16 sampleSize;        // bytes per encoded frame
	u16 codec;             // 0x1000 ATRAC3+, 0x1001 ATRAC3
	s32 dataOff;
	s32 curFileOff;
	s32 fileDataEnd;
	s32 loopNum;
	s32 streamDataByte;
	s32 streamOff;
	s32 secondStreamOff;
	u32 buffer;
	u32 secondBuffer;
	u32 bufferByte;
	u32 secondBufferByte;
	u32 unk[14];

	int SamplesPerFrame() const { return codec == 0x1000 ? (int)ATRAC3PLUS_MAX_SAMPLES : (int)ATRAC3_MAX_SAMPLES; }
	int SamplesFrameMask() const { return SamplesPerFrame() - 1; }
	int SkipSamples() const { return codec == 0x1000 ? 0x170 : 0x45; }
	int BitRate() const {
		int bitrate = (sampleSize * 352800) / 1000;
		if (codec == PSP_CODEC_AT3PLUS)
			bitrate = ((bitrate >> 11) + 8) & 0xFFFFFFF0;
		else
			bitrate = (bitrate + 511) >> 10;
		return bitrate;
	}
};

struct SceAtracContext {
	SceAudiocodecCodec codec;
	SceAtracIdInfo info;
};
static_assert(sizeof(SceAtracIdInfo) == 128, "SceAtracIdInfo layout");
static_assert(sizeof(SceAtracContext) == 256, "SceAtracContext layout");

struct Atrac3LowLevelParams {
	int encodedChannels;
	int outputChannels;
	int bytesPerFrame;
};

struct AtracSingleResetBufferInfo {
	u32 writePosPtr;
	u32 writableBytes;
	u32 minWriteBytes;
	u32 filePos;
};

struct AtracResetBufferInfo {
	AtracSingleResetBufferInfo first;
	AtracSingleResetBufferInfo second;
};

struct AtracSasStreamState {
	u32 bufPtr[2]{};
	u32 bufSize[2]{};
	int streamOffset = 0;
	int fileOffset = 0;
	int curBuffer = 0;
	bool isStreaming = false;
};

constexpr int PSP_ATRAC_ALLDATA_IS_ON_MEMORY = -1;
constexpr int PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY = -2;
constexpr int PSP_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY = -3;

struct AT3BitrateMeta {
	u16 sampleSize;
	u8 dataByte;
	u8 jointStereo;
};

const AT3BitrateMeta g_at3BitrateMeta[5] = {
	{0x180, 0x04, 0}, {0x130, 0x06, 0}, {0x0C0, 0x0B, 1}, {0x0C0, 0x0E, 0}, {0x098, 0x0F, 0},
};

int RoundDownToMultiple(int size, int grain) { return size - (size % grain); }

int RoundDownToMultipleWithOffset(int offset, int size, int grain) {
	if (size > offset) return ((size - offset) / grain) * grain + offset;
	return size;
}

int ComputeSkipFrames(const SceAtracIdInfo &info, int seekPos) {
	return (seekPos & info.SamplesFrameMask()) < info.SkipSamples() ? 2 : 1;
}

int ComputeFileOffset(const SceAtracIdInfo &info, int seekPos) {
	int frameOffset = ((seekPos / info.SamplesPerFrame()) - 1) * info.sampleSize;
	if ((seekPos & info.SamplesFrameMask()) < info.SkipSamples() && (frameOffset != 0)) frameOffset -= info.sampleSize;
	return frameOffset + info.dataOff;
}

// Inclusive, unlike ComputeFileOffset.
int ComputeLoopEndFileOffset(const SceAtracIdInfo &info, int seekPos) {
	return (seekPos / info.SamplesPerFrame() + 1) * info.sampleSize + info.dataOff;
}

int ComputeSpaceUsed(const SceAtracIdInfo &info) {
	// Streaming from the second buffer and past the loop end (in the tail).
	if (info.decodePos > info.loopEnd && info.curBuffer == 1) {
		int space = (int)info.secondBufferByte;
		if (info.secondStreamOff < space) space = RoundDownToMultipleWithOffset(info.secondStreamOff, (int)info.secondBufferByte, info.sampleSize);
		if ((info.secondStreamOff <= space) && (space - info.secondStreamOff < info.streamDataByte))
			return info.streamDataByte - (space - info.secondStreamOff);
		return 0;
	}
	return info.streamDataByte;
}

int ComputeRemainFrameStream(const SceAtracIdInfo &info) {
	if (info.streamDataByte >= info.fileDataEnd - info.curFileOff) return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
	return std::max(0, info.streamDataByte / info.sampleSize - (int)info.numSkipFrames);
}

int ComputeRemainFrameLooped(const SceAtracIdInfo &info) {
	const int loopStartFileOffset = ComputeFileOffset(info, info.loopStart);
	const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
	const int writeFileOff = info.curFileOff + info.streamDataByte;
	const int leftToRead = writeFileOff - loopEndFileOffset;

	int remainFrames;
	if (writeFileOff <= loopEndFileOffset) {
		remainFrames = info.streamDataByte / info.sampleSize;
	} else {
		const int skipFramesAtLoopStart = ComputeSkipFrames(info, info.loopStart);
		const int firstPartLength = loopEndFileOffset - loopStartFileOffset;
		const int secondPartLength = leftToRead % firstPartLength;
		remainFrames = (loopEndFileOffset - info.curFileOff) / info.sampleSize +
		               (leftToRead / firstPartLength) * (firstPartLength / info.sampleSize - skipFramesAtLoopStart);
		if (secondPartLength > skipFramesAtLoopStart * info.sampleSize)
			remainFrames += secondPartLength / info.sampleSize - skipFramesAtLoopStart;
	}
	remainFrames = std::max(0, remainFrames - (int)info.numSkipFrames);
	if (info.loopNum < 0) return remainFrames;

	const int streamBufferEndFileOffset = info.curFileOff + info.streamDataByte;
	if (streamBufferEndFileOffset >= loopEndFileOffset) {
		const int numBufferedLoops = (streamBufferEndFileOffset - loopEndFileOffset) / (loopEndFileOffset - loopStartFileOffset);
		if (info.loopNum <= numBufferedLoops) return PSP_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY;
	}
	return remainFrames;
}

void InitLengthAndLoop(SceAtracIdInfo *ctx, int endSample, int waveDataSize, int firstSampleOffset, int loopBegin, int loopEnd) {
	const int off = ctx->codec == 0x1001 ? 0x45 : 0x170;
	const int blockShift = 0x100b - ctx->codec;
	const int firstValidSample = firstSampleOffset + off;
	int numSamplesInFile;
	if (endSample == 0) {
		numSamplesInFile = waveDataSize / ctx->sampleSize << (blockShift & 0x1f);
	} else {
		numSamplesInFile = endSample + firstValidSample;
	}
	ctx->decodePos = firstValidSample;
	ctx->loopNum = 0;
	ctx->endSample = numSamplesInFile - 1;
	ctx->numSkipFrames = (u8)(firstValidSample >> (blockShift & 0x1f));
	if (-1 < loopBegin) {
		ctx->loopEnd = loopEnd + off;
		ctx->loopStart = loopBegin + off;
		return;
	}
	ctx->loopEnd = 0;
	ctx->loopStart = 0;
}

int ComputeAtracStateAndInitSecondBuffer(SceAtracIdInfo *info, u32 readSize, u32 bufferSize) {
	AtracStatus state;
	if (bufferSize < (u32)info->fileDataEnd) {
		// sampleSize * 3 would be more accurate; 2 helps GTA LCS custom music.
		if (info->streamDataByte < (s32)info->sampleSize * 2) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
		int loopEnd = info->loopEnd;
		state = ATRAC_STATUS_STREAMED_WITHOUT_LOOP;
		if (loopEnd != 0) {
			state = ATRAC_STATUS_STREAMED_LOOP_FROM_END;
			if (loopEnd != info->endSample) {
				const int loopEndFileOffset = ComputeLoopEndFileOffset(*info, loopEnd);
				loopEnd = (loopEndFileOffset - info->dataOff) + 1;
				info->state = ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER;
				if (loopEnd < info->streamDataByte) info->streamDataByte = loopEnd;
				info->secondStreamOff = 0;
				info->secondBuffer = 0;
				info->secondBufferByte = 0;
				return 0;
			}
		}
	} else {
		state = ATRAC_STATUS_HALFWAY_BUFFER;
		if (readSize >= (u32)info->fileDataEnd) state = ATRAC_STATUS_ALL_DATA_LOADED;
	}
	info->state = state;
	return 0;
}

int InitContextFromTrackInfo(SceAtracContext *ctx, const TrackInfo *wave, u32 bufferAddr, int readSize, int bufferSize) {
	// A zero frame size (possible from an AA3 header) would divide by zero below.
	if (wave->blockAlign == 0) return (int)SCE_ERROR_ATRAC_BAD_CODEC_PARAMS;
	ctx->info.numChan = (u8)wave->numChans;
	const int extraSamples = ctx->info.codec == 0x1001 ? 0x45 : 0x170;
	ctx->info.firstValidSample = extraSamples + (int)wave->firstSampleOffset;
	ctx->info.sampleSize = wave->blockAlign;
	InitLengthAndLoop(&ctx->info, (int)wave->endSample, (int)wave->waveDataSize, (int)wave->firstSampleOffset, (int)wave->loopStart,
	                  (int)wave->loopEnd);
	const int dataOff = (int)wave->dataOff;
	const int endSample = ctx->info.endSample;
	ctx->info.streamDataByte = readSize - dataOff;
	ctx->info.buffer = bufferAddr;
	ctx->info.curFileOff = dataOff;
	ctx->info.dataOff = dataOff;
	ctx->info.fileDataEnd = (int)wave->waveDataSize + dataOff;
	ctx->info.curBuffer = 0;
	ctx->info.bufferByte = (u32)bufferSize;
	ctx->info.streamOff = dataOff;
	if (ctx->info.sampleSize > (u32)bufferSize) return (int)SCE_ERROR_ATRAC_BAD_CODEC_PARAMS;
	if (ctx->info.loopEnd > endSample) return (int)SCE_ERROR_ATRAC_BAD_CODEC_PARAMS;

	const int numChunks = (endSample >> ((0x100b - ctx->info.codec) & 0x1f));
	if ((u32)numChunks * (u32)ctx->info.sampleSize < (u32)(((int)wave->waveDataSize + dataOff) - dataOff)) {
		int retval = ComputeAtracStateAndInitSecondBuffer(&ctx->info, (u32)readSize, (u32)bufferSize);
		if (retval < 0) return retval;
		if (ctx->info.codec != PSP_CODEC_AT3) {
			ctx->codec.fmt.at3.formatByte1 = wave->sampleSizeMaybe;
			ctx->codec.fmt.at3.at3Related = 0;
			ctx->codec.fmt.at3.formatByte2 = wave->tailFlag;
			return 0;
		}
		// The hardware codec parameter for ATRAC3, as libatrac3plus's SetData picks it.
		for (int counter = 4; counter >= 0; counter--) {
			if ((g_at3BitrateMeta[counter].sampleSize == ctx->info.sampleSize) && ((int)g_at3BitrateMeta[counter].jointStereo == wave->sampleSizeMaybe)) {
				ctx->codec.fmt.at3.formatByte1 = g_at3BitrateMeta[counter].dataByte;
				ctx->codec.fmt.at3.formatByte2 = 0;
				ctx->codec.fmt.at3.unk2a = 0;
				ctx->codec.fmt.at3.unk2b = 0;
				return 0;
			}
		}
		return 0;
	}
	return (int)SCE_ERROR_ATRAC_BAD_CODEC_PARAMS;
}

class Atrac2 {
public:
	Atrac2(u32 contextAddr, int codecType) : ctxAddr_(contextAddr) {
		SceAtracIdInfo &info = Info();
		info.codec = (u16)codecType;
		info.state = ATRAC_STATUS_NO_DATA;
		info.curBuffer = 0;
	}
	~Atrac2() {
		delete decoder_;
		delete[] decodeTemp_;
	}

	SceAtracContext *Ctx() const { return (SceAtracContext *)GPtr(ctxAddr_); }
	SceAtracIdInfo &Info() const { return Ctx()->info; }
	u32 ContextAddr() const { return ctxAddr_; }

	u8 BufferState() const { return Info().state; }
	int Channels() const { return Info().numChan; }
	int BytesPerFrame() const { return Info().sampleSize; }
	int SamplesPerFrame() const { return Info().SamplesPerFrame(); }
	int CodecType() const { return Info().codec; }
	int GetOutputChannels() const { return outputChannels_; }
	int SkippedFramesOnSetData() const { return setDataSkippedFrames_; }
	u32 GetInternalCodecError() const { return (u32)Ctx()->codec.err; }
	int Bitrate() const { return Info().BitRate(); }
	int LoopNum() const { return Info().loopNum; }
	bool HasSecondBuffer() const { return Info().secondBufferByte != 0; }

	int RemainingFrames() const;
	int LoopStatus() const;
	int SetLoopNum(int loopNum);
	u32 GetNextSamples() const;
	int GetNextDecodePosition(int *pos) const;
	int AddStreamData(u32 bytesToAdd);
	void GetStreamDataInfo(u32 *writePtr, u32 *bytesToWrite, u32 *readFileOffset) const;
	int GetSoundSample(int *outEndSample, int *outLoopStartSample, int *outLoopEndSample) const;
	int ResetPlayPosition(int seekPos, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf, bool *delay);
	int GetBufferInfoForResetting(AtracResetBufferInfo *bufferInfo, int seekPos, bool *delay);
	int SetData(const Track &track, u32 bufferAddr, u32 readSize, u32 bufferSize, u32 fileSize, int outputChannels, bool isAA3);
	int GetSecondBufferInfo(u32 *fileOffset, u32 *readSize) const;
	int SetSecondBuffer(u32 secondBuffer, u32 secondBufferSize);
	u32 DecodeData(u32 outbufAddr, int *SamplesNum, int *finish, int *remains);
	void InitLowLevel(const Atrac3LowLevelParams &params, int codecType);
	int DecodeLowLevel(const u8 *srcData, int *bytesConsumed, s16 *dstData, int *bytesWritten);
	void CheckForSas();
	int EnqueueForSas(u32 address, u32 ptr);
	void DecodeForSas(s16 *dstData, int *bytesWritten, int *finish);

private:
	void CreateDecoder(int codecType, int bytesPerFrame, int channels, bool jointStereo);
	u32 DecodeInternal(u32 outbufAddr, int *SamplesNum, int *finish);
	void GetResetBufferInfoInternal(AtracResetBufferInfo *bufferInfo, int sample) const;
	u32 ResetPlayPositionInternal(int seekPos, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf);
	u32 SkipFrames(int *skippedCount);
	void WrapLastPacket();

	u32 ctxAddr_;
	u16 outputChannels_ = 2;
	int setDataSkippedFrames_ = 0;
	AudioDecoder *decoder_ = nullptr;
	s16 *decodeTemp_ = nullptr;
	AtracSasStreamState sas_;
	bool jointStereo_ = false;
};

void Atrac2::CreateDecoder(int codecType, int bytesPerFrame, int channels, bool jointStereo) {
	delete decoder_;
	if (codecType == PSP_CODEC_AT3) {
		// The layout comes from the frame size and the joint stereo flag, as on hardware.
		int decoderChannels = channels;
		Atrac3DecoderChannels(bytesPerFrame, jointStereo, &decoderChannels);
		u8 extraData[14]{};
		extraData[0] = 1;
		extraData[3] = (u8)(decoderChannels << 3);
		extraData[6] = jointStereo;
		extraData[8] = jointStereo;
		extraData[10] = 1;
		decoder_ = CreateAtrac3Audio(decoderChannels, bytesPerFrame, extraData, sizeof(extraData));
	} else {
		decoder_ = CreateAtrac3PlusAudio(channels, bytesPerFrame);
	}
}

int Atrac2::RemainingFrames() const {
	const SceAtracIdInfo &info = Info();
	switch (info.state) {
	case ATRAC_STATUS_UNINITIALIZED:
	case ATRAC_STATUS_NO_DATA: return 0;
	case ATRAC_STATUS_ALL_DATA_LOADED: return PSP_ATRAC_ALLDATA_IS_ON_MEMORY;
	case ATRAC_STATUS_HALFWAY_BUFFER: {
		const int writeFileOff = info.dataOff + info.streamDataByte;
		if (info.curFileOff < writeFileOff) return std::max(0, (writeFileOff - info.curFileOff) / info.sampleSize - (int)info.numSkipFrames);
		return 0;
	}
	case ATRAC_STATUS_STREAMED_WITHOUT_LOOP: return ComputeRemainFrameStream(info);
	case ATRAC_STATUS_STREAMED_LOOP_FROM_END: return ComputeRemainFrameLooped(info);
	case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER:
		if (info.decodePos <= info.loopEnd) return ComputeRemainFrameLooped(info);
		return ComputeRemainFrameStream(info);
	default: return (int)SCE_ERROR_ATRAC_BAD_ATRACID;
	}
}

int Atrac2::GetSoundSample(int *outEndSample, int *outLoopStartSample, int *outLoopEndSample) const {
	const SceAtracIdInfo &info = Info();
	*outEndSample = info.endSample - info.firstValidSample;
	if (info.loopEnd == 0) {
		*outLoopStartSample = -1;
		*outLoopEndSample = -1;
	} else {
		*outLoopStartSample = info.loopStart - info.firstValidSample;
		*outLoopEndSample = info.loopEnd - info.firstValidSample;
	}
	return 0;
}

int Atrac2::ResetPlayPosition(int seekPos, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf, bool *delay) {
	*delay = false;
	SceAtracIdInfo &info = Info();
	if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.secondBufferByte == 0) return (int)SCE_ERROR_ATRAC_SECOND_BUFFER_NEEDED;
	seekPos += info.firstValidSample;
	if ((u32)seekPos > (u32)info.endSample) return (int)SCE_ERROR_ATRAC_BAD_SAMPLE;
	int result = (int)ResetPlayPositionInternal(seekPos, bytesWrittenFirstBuf, bytesWrittenSecondBuf);
	if (result >= 0) {
		int skipCount = 0;
		result = (int)SkipFrames(&skipCount);
		if (skipCount) *delay = true;
	}
	return result;
}

u32 Atrac2::ResetPlayPositionInternal(int seekPos, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf) {
	AtracResetBufferInfo bufferInfo;
	GetResetBufferInfoInternal(&bufferInfo, seekPos);
	if ((u32)bytesWrittenFirstBuf < bufferInfo.first.minWriteBytes || (u32)bytesWrittenFirstBuf > bufferInfo.first.writableBytes)
		return SCE_ERROR_ATRAC_BAD_FIRST_RESET_SIZE;
	if ((u32)bytesWrittenSecondBuf < bufferInfo.second.minWriteBytes || (u32)bytesWrittenSecondBuf > bufferInfo.second.writableBytes)
		return SCE_ERROR_ATRAC_BAD_SECOND_RESET_SIZE;

	SceAtracIdInfo &info = Info();
	info.decodePos = seekPos;
	info.numSkipFrames = (u8)ComputeSkipFrames(info, seekPos);
	info.loopNum = 0;
	info.curFileOff = ComputeFileOffset(info, seekPos);
	Ctx()->codec.err = 0x20b;  // testing shows it

	switch (info.state) {
	case ATRAC_STATUS_ALL_DATA_LOADED: return 0;
	case ATRAC_STATUS_HALFWAY_BUFFER:
		info.streamDataByte += bytesWrittenFirstBuf;
		if (info.dataOff + info.streamDataByte >= info.fileDataEnd) info.state = ATRAC_STATUS_ALL_DATA_LOADED;
		return 0;
	case ATRAC_STATUS_STREAMED_WITHOUT_LOOP:
	case ATRAC_STATUS_STREAMED_LOOP_FROM_END:
		info.streamDataByte = bytesWrittenFirstBuf;
		info.curBuffer = 0;
		info.streamOff = 0;
		return 0;
	case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER: {
		const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
		if (info.curFileOff >= loopEndFileOffset) {
			const int secondBufferSizeRounded = RoundDownToMultiple((int)info.secondBufferByte, info.sampleSize);
			if (info.curFileOff < loopEndFileOffset + secondBufferSizeRounded) {
				info.streamDataByte = ((loopEndFileOffset + secondBufferSizeRounded) - info.curFileOff) + bytesWrittenFirstBuf;
				info.curBuffer = 1;
				info.secondStreamOff = info.curFileOff - loopEndFileOffset;
			} else {
				info.streamDataByte = bytesWrittenFirstBuf;
				info.curBuffer = 2;  // temporary, switches back to 0 at once
				info.streamOff = 0;
			}
		} else {
			info.streamDataByte = bytesWrittenFirstBuf;
			info.curBuffer = 0;
			info.streamOff = 0;
		}
		return 0;
	}
	default: return 0;
	}
}

// Not const: frames to skip get skipped here.
int Atrac2::GetBufferInfoForResetting(AtracResetBufferInfo *bufferInfo, int seekPos, bool *delay) {
	const SceAtracIdInfo &info = Info();
	if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.secondBufferByte == 0) return (int)SCE_ERROR_ATRAC_SECOND_BUFFER_NEEDED;
	seekPos += info.firstValidSample;
	if ((u32)seekPos > (u32)info.endSample) return (int)SCE_ERROR_ATRAC_BAD_SAMPLE;
	GetResetBufferInfoInternal(bufferInfo, seekPos);
	int skipCount = 0;
	int retval = (int)SkipFrames(&skipCount);
	if (skipCount > 0) *delay = true;
	return retval;
}

void Atrac2::GetResetBufferInfoInternal(AtracResetBufferInfo *bufferInfo, int seekPos) const {
	const SceAtracIdInfo &info = Info();
	switch (info.state) {
	case ATRAC_STATUS_NO_DATA:
	case ATRAC_STATUS_ALL_DATA_LOADED:
		bufferInfo->first.writePosPtr = info.buffer;
		bufferInfo->first.writableBytes = 0;
		bufferInfo->first.minWriteBytes = 0;
		bufferInfo->first.filePos = 0;
		break;
	case ATRAC_STATUS_HALFWAY_BUFFER: {
		const int streamPos = info.dataOff + info.streamDataByte;
		const int fileOff = info.dataOff + (seekPos / info.SamplesPerFrame() + 1) * info.sampleSize;
		bufferInfo->first.writePosPtr = info.buffer + streamPos;
		bufferInfo->first.writableBytes = (u32)(info.fileDataEnd - streamPos);
		bufferInfo->first.filePos = (u32)streamPos;
		bufferInfo->first.minWriteBytes = (u32)std::max(0, fileOff - streamPos);
		break;
	}
	case ATRAC_STATUS_STREAMED_WITHOUT_LOOP:
	case ATRAC_STATUS_STREAMED_LOOP_FROM_END: {
		const int curFileOffset = ComputeFileOffset(info, seekPos);
		const int bufferEnd = RoundDownToMultiple((int)info.bufferByte, info.sampleSize);
		bufferInfo->first.writePosPtr = info.buffer;
		bufferInfo->first.writableBytes = (u32)std::min(info.fileDataEnd - curFileOffset, bufferEnd);
		bufferInfo->first.minWriteBytes = (u32)((ComputeSkipFrames(info, seekPos) + 1) * info.sampleSize);
		bufferInfo->first.filePos = (u32)curFileOffset;
		break;
	}
	case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER: {
		const int seekFileOffset = ComputeFileOffset(info, seekPos);
		const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd) - 1;
		const int bufferEnd = RoundDownToMultiple((int)info.bufferByte, info.sampleSize);
		const int skipBytes = (ComputeSkipFrames(info, seekPos) + 1) * info.sampleSize;
		const int secondBufferEnd = RoundDownToMultiple((int)info.secondBufferByte, info.sampleSize);
		if (seekFileOffset < loopEndFileOffset) {
			const int remainingBeforeLoop = (loopEndFileOffset - seekFileOffset) + 1;
			bufferInfo->first.writePosPtr = info.buffer;
			bufferInfo->first.writableBytes = (u32)std::min(bufferEnd, remainingBeforeLoop);
			bufferInfo->first.minWriteBytes = (u32)std::min(skipBytes, remainingBeforeLoop);
			bufferInfo->first.filePos = (u32)seekFileOffset;
		} else if (loopEndFileOffset + secondBufferEnd <= seekFileOffset) {
			bufferInfo->first.writePosPtr = info.buffer;
			bufferInfo->first.writableBytes = (u32)std::min(info.fileDataEnd - seekFileOffset, bufferEnd);
			bufferInfo->first.minWriteBytes = (u32)skipBytes;
			bufferInfo->first.filePos = (u32)seekFileOffset;
		} else if (loopEndFileOffset + (int)info.secondBufferByte + 1 < info.fileDataEnd) {
			const int endOffset = loopEndFileOffset + secondBufferEnd + 1;
			bufferInfo->first.writePosPtr = info.buffer;
			bufferInfo->first.writableBytes = (u32)std::min(info.fileDataEnd - endOffset, bufferEnd);
			bufferInfo->first.minWriteBytes = (u32)std::max(0, seekFileOffset + skipBytes - endOffset);
			bufferInfo->first.filePos = (u32)endOffset;
		} else {
			bufferInfo->first.writePosPtr = info.buffer;
			bufferInfo->first.writableBytes = 0;
			bufferInfo->first.minWriteBytes = 0;
			bufferInfo->first.filePos = 0;
		}
		break;
	}
	default: break;
	}
	// Reset never needs a second buffer write; its pointer mirrors the first buffer's.
	bufferInfo->second.writePosPtr = info.buffer;
	bufferInfo->second.writableBytes = 0;
	bufferInfo->second.minWriteBytes = 0;
	bufferInfo->second.filePos = 0;
}

int Atrac2::SetLoopNum(int loopNum) {
	SceAtracIdInfo &info = Info();
	if (info.loopEnd <= 0) return (int)SCE_ERROR_ATRAC_NO_LOOP_INFORMATION;
	info.loopNum = loopNum;
	return 0;
}

int Atrac2::LoopStatus() const {
	const SceAtracIdInfo &info = Info();
	if (info.loopEnd == 0) return 0;
	if (info.loopNum != 0) return 1;
	return info.decodePos <= info.loopEnd ? 1 : 0;
}

u32 Atrac2::GetNextSamples() const {
	const SceAtracIdInfo &info = Info();
	const int endOfCurrentFrame = info.decodePos | info.SamplesFrameMask();
	const int remainder = std::max(0, endOfCurrentFrame - info.endSample);
	const int adjusted = (info.decodePos & info.SamplesFrameMask()) + remainder;
	return (u32)std::max(0, info.SamplesPerFrame() - adjusted);
}

int Atrac2::GetNextDecodePosition(int *pos) const {
	const SceAtracIdInfo &info = Info();
	if (info.decodePos > info.endSample) return (int)SCE_ERROR_ATRAC_ALL_DATA_DECODED;
	if (info.fileDataEnd - info.curFileOff < info.sampleSize) return (int)SCE_ERROR_ATRAC_ALL_DATA_DECODED;
	*pos = info.decodePos - info.firstValidSample;
	return 0;
}

int Atrac2::AddStreamData(u32 bytesToAdd) {
	SceAtracIdInfo &info = Info();
	if (info.state == ATRAC_STATUS_HALFWAY_BUFFER) {
		const int newFileOffset = info.streamDataByte + info.dataOff + (int)bytesToAdd;
		if (newFileOffset == info.fileDataEnd) {
			info.state = ATRAC_STATUS_ALL_DATA_LOADED;
		} else if (newFileOffset > info.fileDataEnd) {
			return (int)SCE_ERROR_ATRAC_ADD_DATA_IS_TOO_BIG;
		}
		info.streamDataByte += (int)bytesToAdd;
	} else {
		info.streamDataByte += (int)bytesToAdd;
	}
	return 0;
}

int ComputeLoopedStreamWritableBytes(const SceAtracIdInfo &info, const int loopStartFileOffset, const u32 loopEndFileOffset) {
	const u32 writeOffset = (u32)(info.curFileOff + info.streamDataByte);
	if (writeOffset >= loopEndFileOffset) {
		const int loopLength = (int)loopEndFileOffset - loopStartFileOffset;
		return loopLength - (int)((info.curFileOff + info.streamDataByte) - loopEndFileOffset) % loopLength;
	}
	return (int)(loopEndFileOffset - writeOffset);
}

int IncrementAndLoop(int curOffset, int increment, int loopStart, int loopEnd) {
	const int sum = curOffset + increment;
	if (sum >= loopEnd) return loopStart + (sum - loopEnd) % (loopEnd - loopStart);
	return sum;
}

int WrapAroundRoundedBufferSize(int offset, int bufferSize, int addend, int grainSize) {
	bufferSize = RoundDownToMultipleWithOffset(offset, bufferSize, grainSize);
	const int sum = offset + addend;
	if (bufferSize <= sum) return sum - bufferSize;
	return sum;
}

void ComputeStreamBufferDataInfo(const SceAtracIdInfo &info, u32 *writePtr, u32 *bytesToWrite, u32 *readFileOffset) {
	const u32 streamOff = info.curBuffer != 1 ? (u32)info.streamOff : 0;
	const int spaceUsed = ComputeSpaceUsed(info);
	const int spaceLeftAfterStreamOff = RoundDownToMultipleWithOffset((int)streamOff, (int)info.bufferByte, info.sampleSize);
	const int streamPos = (int)streamOff + spaceUsed;
	int spaceLeftInBuffer;
	if (streamPos >= spaceLeftAfterStreamOff) {
		spaceLeftInBuffer = spaceLeftAfterStreamOff - spaceUsed;
	} else {
		spaceLeftInBuffer = spaceLeftAfterStreamOff - streamPos;
	}
	const int loopStartFileOffset = ComputeFileOffset(info, info.loopStart);
	const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
	if (spaceLeftInBuffer < 0) spaceLeftInBuffer = 0;  // most likely a truncated file

	switch (info.state) {
	case ATRAC_STATUS_STREAMED_WITHOUT_LOOP: {
		*bytesToWrite = (u32)std::clamp(info.fileDataEnd - (info.curFileOff + info.streamDataByte), 0, spaceLeftInBuffer);
		const int streamFileOff = info.curFileOff + info.streamDataByte;
		if (streamFileOff < info.fileDataEnd) {
			*readFileOffset = (u32)streamFileOff;
			*writePtr = info.buffer + (u32)WrapAroundRoundedBufferSize(info.streamOff, (int)info.bufferByte, info.streamDataByte, info.sampleSize);
		} else {
			*readFileOffset = 0;
			*writePtr = info.buffer;
		}
		break;
	}
	case ATRAC_STATUS_STREAMED_LOOP_FROM_END:
		*bytesToWrite = (u32)std::min(ComputeLoopedStreamWritableBytes(info, loopStartFileOffset, (u32)loopEndFileOffset), spaceLeftInBuffer);
		*readFileOffset = (u32)IncrementAndLoop(info.curFileOff, info.streamDataByte, loopStartFileOffset, loopEndFileOffset);
		*writePtr = info.buffer + (u32)WrapAroundRoundedBufferSize(info.streamOff, (int)info.bufferByte, info.streamDataByte, info.sampleSize);
		break;
	case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER: {
		if (info.decodePos <= info.loopEnd) {
			*bytesToWrite = (u32)std::min(ComputeLoopedStreamWritableBytes(info, loopStartFileOffset, (u32)loopEndFileOffset), spaceLeftInBuffer);
			*readFileOffset = (u32)IncrementAndLoop(info.curFileOff, info.streamDataByte, loopStartFileOffset, loopEndFileOffset);
		} else {
			const int streamFileOff = info.curFileOff + info.streamDataByte;
			*bytesToWrite = (u32)std::clamp(info.fileDataEnd - streamFileOff, 0, spaceLeftInBuffer);
			*readFileOffset = streamFileOff < info.fileDataEnd ? (u32)streamFileOff : 0;
		}
		if (info.decodePos <= info.loopEnd || info.curBuffer != 1) {
			*writePtr = info.buffer + (u32)WrapAroundRoundedBufferSize(info.streamOff, (int)info.bufferByte, info.streamDataByte, info.sampleSize);
		} else {
			*writePtr = info.buffer + (u32)WrapAroundRoundedBufferSize(0, (int)info.bufferByte, spaceUsed, info.sampleSize);
		}
		break;
	}
	default: break;
	}
}

void Atrac2::GetStreamDataInfo(u32 *writePtr, u32 *bytesToWrite, u32 *readFileOffset) const {
	const SceAtracIdInfo &info = Info();
	switch (info.state) {
	case ATRAC_STATUS_ALL_DATA_LOADED:
		*writePtr = info.buffer;
		*bytesToWrite = 0;
		*readFileOffset = 0;
		break;
	case ATRAC_STATUS_HALFWAY_BUFFER: {
		// Direct mapped: the file offset is also the buffer offset.
		const int fileOffset = info.dataOff + info.streamDataByte;
		const int bytesLeftInFile = info.fileDataEnd - fileOffset;
		*writePtr = info.buffer + (u32)fileOffset;
		*bytesToWrite = (u32)bytesLeftInFile;
		*readFileOffset = (u32)fileOffset;
		break;
	}
	default: ComputeStreamBufferDataInfo(info, writePtr, bytesToWrite, readFileOffset); break;
	}
}

u32 Atrac2::DecodeData(u32 outbufAddr, int *SamplesNum, int *finish, int *remains) {
	SceAtracIdInfo &info = Info();
	const int tries = info.numSkipFrames + 1;
	for (int i = 0; i < tries; i++) {
		u32 result = DecodeInternal(outbufAddr, SamplesNum, finish);
		if (result != 0) {
			*SamplesNum = 0;
			return result;
		}
	}
	*remains = RemainingFrames();
	return 0;
}

u32 Atrac2::DecodeInternal(u32 outbufAddr, int *SamplesNum, int *finish) {
	SceAtracIdInfo &info = Info();
	const int samplesToDecode = (int)GetNextSamples();
	const int nextFileOff = info.curFileOff + info.sampleSize;
	if (nextFileOff > info.fileDataEnd || info.decodePos > info.endSample) {
		*finish = 1;
		return SCE_ERROR_ATRAC_ALL_DATA_DECODED;
	}
	if (AtracStatusIsStreaming(info.state) && info.streamDataByte < info.sampleSize) {
		*finish = 0;
		return SCE_ERROR_ATRAC_BUFFER_IS_EMPTY;
	}
	if (info.state == ATRAC_STATUS_HALFWAY_BUFFER && info.dataOff + info.streamDataByte < nextFileOff) {
		*finish = 0;
		return SCE_ERROR_ATRAC_BUFFER_IS_EMPTY;
	}

	u32 streamOff;
	u32 bufferPtr;
	if (!AtracStatusIsStreaming(info.state)) {
		bufferPtr = info.buffer;
		streamOff = (u32)info.curFileOff;
	} else {
		const int bufferIndex = info.curBuffer & 1;
		bufferPtr = bufferIndex == 0 ? info.buffer : info.secondBuffer;
		streamOff = (u32)(bufferIndex == 0 ? info.streamOff : info.secondStreamOff);
	}
	const u32 inAddr = bufferPtr + streamOff;

	s16 *outPtr = nullptr;
	if (samplesToDecode != info.SamplesPerFrame()) {
		if (!decodeTemp_) decodeTemp_ = new s16[ATRAC3PLUS_MAX_SAMPLES * 2];
		outPtr = decodeTemp_;
	} else if (outbufAddr) {
		outPtr = (s16 *)mem::GetPointer(outbufAddr, (u32)(info.SamplesPerFrame() * outputChannels_ * 2));
	}

	Ctx()->codec.inBuf = inAddr;
	Ctx()->codec.outBuf = outbufAddr;

	const u8 *in = mem::GetPointer(inAddr, info.sampleSize);
	if (!in) {
		LOGE("ATRAC", "DecodeInternal: bad inAddr %08x", inAddr);
		return SCE_ERROR_ATRAC_API_FAIL;
	}

	int bytesConsumed = 0;
	int outSamples = 0;
	if (!decoder_ || !decoder_->Decode(in, info.sampleSize, &bytesConsumed, outputChannels_, outPtr, &outSamples)) {
		*finish = 0;
		// 0x20b and 0x20c have been seen for garbage data.
		Ctx()->codec.err = 0x20b;
		return SCE_ERROR_ATRAC_API_FAIL;
	}
	Ctx()->codec.err = 0;

	info.curFileOff += info.sampleSize;

	if (info.numSkipFrames == 0) {
		*SamplesNum = samplesToDecode;
		if (info.endSample < info.decodePos + samplesToDecode) {
			*finish = info.loopNum == 0;
		} else {
			*finish = 0;
		}
		if (samplesToDecode != info.SamplesPerFrame() && samplesToDecode != 0 && outbufAddr) {
			if (u8 *outBuf = mem::GetPointer(outbufAddr, (u32)(samplesToDecode * outputChannels_ * 2)))
				memcpy(outBuf, decodeTemp_, samplesToDecode * outputChannels_ * sizeof(s16));
		}
		info.decodePos += samplesToDecode;
		if (info.loopEnd != 0 && info.loopNum != 0 && info.decodePos > info.loopEnd) {
			info.curFileOff = ComputeFileOffset(info, info.loopStart);
			info.numSkipFrames = (u8)ComputeSkipFrames(info, info.loopStart);
			info.decodePos = info.loopStart;
			if (info.loopNum > 0) info.loopNum--;
		}
	} else {
		info.numSkipFrames--;
	}

	if (AtracStatusIsStreaming(info.state)) {
		info.streamDataByte -= info.sampleSize;
		if (info.curBuffer == 1) {
			const int nextStreamOff = info.secondStreamOff + info.sampleSize;
			if ((int)info.secondBufferByte < nextStreamOff + info.sampleSize) {
				info.streamOff = 0;
				info.secondStreamOff = 0;
				info.curBuffer = 2;
			} else {
				info.secondStreamOff = nextStreamOff;
			}
		} else {
			const int nextStreamOff = info.streamOff + info.sampleSize;
			if (nextStreamOff + info.sampleSize > (int)info.bufferByte) {
				info.streamOff = 0;
			} else {
				info.streamOff = nextStreamOff;
			}
			// In LOOP_WITH_TRAILER, streaming from the main buffer, with no loop or the final
			// loop done and past the loop point...
			if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.curBuffer == 0 &&
			    (info.loopEnd == 0 || (info.loopNum == 0 && info.loopEnd < info.decodePos))) {
				if (info.curFileOff >= ComputeLoopEndFileOffset(info, info.loopEnd)) {
					// ...switch to the second buffer, copying its trailing partial packet to
					// the start of the main buffer.
					info.curBuffer = 1;
					info.streamDataByte = (s32)info.secondBufferByte;
					info.secondStreamOff = 0;
					u32 copyLen = info.secondBufferByte % info.sampleSize;
					if (copyLen > info.bufferByte) copyLen = info.bufferByte;
					mem::Memcpy(info.buffer, info.secondBuffer + (info.secondBufferByte - info.secondBufferByte % info.sampleSize), copyLen);
				}
			}
		}
	}
	return 0;
}

int Atrac2::SetData(const Track &track, u32 bufferAddr, u32 readSize, u32 bufferSize, u32 fileSize, int outputChannels, bool isAA3) {
	TrackInfo trackInfo{};
	if (mem::IsValidAddress(bufferAddr)) {
		// Games abuse bufferSize, so only readSize must be mapped; clamp it if not.
		readSize = ClampValidSize(bufferAddr, readSize);
		const u8 *bufferPtr = GPtr(bufferAddr);
		int retval = isAA3 ? ParseAA3(bufferPtr, readSize, fileSize, &trackInfo) : ParseWaveAT3(bufferPtr, readSize, &trackInfo);
		if (retval < 0) return retval;
	}
	int retval = InitContextFromTrackInfo(Ctx(), &trackInfo, bufferAddr, (int)readSize, (int)bufferSize);
	if (retval < 0) return retval;

	SceAtracIdInfo &info = Info();
	jointStereo_ = track.jointStereo != 0;
	CreateDecoder(info.codec, info.sampleSize, info.numChan, jointStereo_);
	outputChannels_ = (u16)outputChannels;

	int skipCount = 0;
	retval = (int)SkipFrames(&skipCount);
	setDataSkippedFrames_ = skipCount;
	if (retval == (int)SCE_ERROR_ATRAC_API_FAIL) LOGE("ATRAC", "Bad frame during initial skip");
	WrapLastPacket();
	return retval;
}

void Atrac2::WrapLastPacket() {
	SceAtracIdInfo &info = Info();
	if (!AtracStatusIsStreaming(info.state)) return;
	const int distanceToEnd = RoundDownToMultiple((int)info.bufferByte - info.streamOff, info.sampleSize);
	if (info.streamDataByte < distanceToEnd) {
		// Hardware zeroes some bytes here (336 in one test); 128 keeps the hexdumps happy.
		mem::Memset(info.buffer, 0, 128);
	} else {
		// The last packet wraps around: copy its overshoot to the start of the buffer.
		const int copyStart = info.streamOff + distanceToEnd;
		const int copyLen = (int)info.bufferByte - copyStart;
		if (copyLen > 0) mem::Memcpy(info.buffer, info.buffer + (u32)copyStart, (u32)copyLen);
	}
}

u32 Atrac2::SkipFrames(int *skipCount) {
	SceAtracIdInfo &info = Info();
	*skipCount = 0;
	int finishIgnored;
	while (true) {
		if (info.numSkipFrames == 0) return 0;
		int samples = 0;
		u32 retval = DecodeInternal(0, &samples, &finishIgnored);
		if (retval != 0) {
			if (retval == SCE_ERROR_ATRAC_API_FAIL) (*skipCount)++;
			return retval;
		}
		(*skipCount)++;
	}
}

int Atrac2::GetSecondBufferInfo(u32 *fileOffset, u32 *readSize) const {
	const SceAtracIdInfo &info = Info();
	if (info.state != ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER) {
		*fileOffset = 0;
		*readSize = 0;
		return (int)SCE_ERROR_ATRAC_SECOND_BUFFER_NOT_NEEDED;
	}
	const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
	*fileOffset = (u32)loopEndFileOffset;
	*readSize = (u32)(info.fileDataEnd - loopEndFileOffset);
	return 0;
}

int Atrac2::SetSecondBuffer(u32 secondBuffer, u32 secondBufferSize) {
	SceAtracIdInfo &info = Info();
	const u32 loopEndFileOffset = (u32)ComputeLoopEndFileOffset(info, info.loopEnd);
	if ((info.sampleSize * 3 <= (int)secondBufferSize || (info.fileDataEnd - (int)loopEndFileOffset) <= (int)secondBufferSize)) {
		if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER) {
			info.secondBuffer = secondBuffer;
			info.secondBufferByte = secondBufferSize;
			info.secondStreamOff = 0;
			return 0;
		}
		return (int)SCE_ERROR_ATRAC_SECOND_BUFFER_NOT_NEEDED;
	}
	return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
}

void Atrac2::InitLowLevel(const Atrac3LowLevelParams &params, int codecType) {
	SceAtracIdInfo &info = Info();
	info.codec = (u16)codecType;
	info.numChan = (u8)params.encodedChannels;
	outputChannels_ = (u16)params.outputChannels;
	info.sampleSize = (u16)params.bytesPerFrame;
	info.dataOff = 0;
	info.decodePos = 0;
	info.state = ATRAC_STATUS_LOW_LEVEL;
	// No track header here, so go by the bitrate.
	jointStereo_ = IsAtrac3StreamJointStereo(codecType, info.sampleSize, info.numChan);
	CreateDecoder(codecType, info.sampleSize, info.numChan, jointStereo_);
}

int Atrac2::DecodeLowLevel(const u8 *srcData, int *bytesConsumed, s16 *dstData, int *bytesWritten) {
	SceAtracIdInfo &info = Info();
	const int channels = outputChannels_;
	int outSamples = 0;
	if (!decoder_ || !decoder_->Decode(srcData, info.sampleSize, bytesConsumed, channels, dstData, &outSamples)) {
		// Proceed anyway (PPSSPP issue #20452).
		LOGW("ATRAC", "Low level decoding failed: sampleSize %d", info.sampleSize);
	}
	*bytesWritten = outSamples * channels * (int)sizeof(s16);
	return 0;
}

void Atrac2::CheckForSas() {
	SceAtracIdInfo &info = Info();
	if (info.numChan != 1) LOGW("ATRAC", "sceSas atrac: caller forgot to set channels to 1");
	if (info.state != ATRAC_STATUS_FOR_SCESAS) LOGW("ATRAC", "sceSas atrac: caller forgot to set state to 0x10");
	sas_.isStreaming = info.fileDataEnd > (s32)info.bufferByte;
}

int Atrac2::EnqueueForSas(u32 address, u32 ptr) {
	SceAtracIdInfo &info = Info();
	// Only allowed while the decoder asks for another buffer.
	if (info.secondBuffer != 0xFFFFFFFF) return (int)SCE_SAS_ERROR_ATRAC3_ALREADY_QUEUED;
	info.secondBuffer = address;
	info.secondBufferByte = ptr;
	return 0;
}

void Atrac2::DecodeForSas(s16 *dstData, int *bytesWritten, int *finish) {
	SceAtracIdInfo &info = Info();
	*bytesWritten = 0;
	// First frame: adopt the initial buffer as the current stream buffer.
	if (info.buffer) {
		sas_.curBuffer = 0;
		sas_.bufPtr[0] = info.buffer;
		sas_.bufSize[0] = info.bufferByte - (u32)info.streamOff;
		sas_.streamOffset = 0;
		sas_.fileOffset = (int)info.bufferByte;
		info.buffer = 0;  // yes, this happens
	}
	if (!decoder_) {
		*finish = 1;
		return;
	}
	u8 assembly[1000];
	if (sas_.streamOffset + (int)info.sampleSize <= (int)sas_.bufSize[sas_.curBuffer]) {
		const u8 *srcData = mem::GetPointer(sas_.bufPtr[sas_.curBuffer] + (u32)sas_.streamOffset, info.sampleSize);
		if (!srcData) {
			*finish = 1;
			return;
		}
		int bytesConsumed = 0;
		if (!decoder_->Decode(srcData, info.sampleSize, &bytesConsumed, 1, dstData, bytesWritten)) LOGE("ATRAC", "SAS failed to decode regular packet");
		sas_.streamOffset += bytesConsumed;
	} else if (sas_.isStreaming) {
		if ((u32)info.sampleSize > sizeof(assembly)) {
			*finish = 1;
			return;
		}
		// The packet spans two buffers.
		const int part1Size = (int)sas_.bufSize[sas_.curBuffer] - sas_.streamOffset;
		const int part2Size = info.sampleSize - part1Size;
		if (part1Size > 0) mem::ReadBytes(assembly, sas_.bufPtr[sas_.curBuffer] + (u32)sas_.streamOffset, (u32)part1Size);
		if (sas_.fileOffset >= info.fileDataEnd) {
			*finish = 1;
			return;
		}
		if (info.secondBuffer == sas_.bufPtr[sas_.curBuffer]) {
			LOGE("ATRAC", "Can't enqueue the same buffer twice in a row");
			*finish = 1;
			return;
		}
		if ((int)info.secondBuffer < 0) {
			LOGE("ATRAC", "AtracSas streaming ran out of data, no second buffer pending");
			*finish = 1;
			return;
		}
		sas_.curBuffer ^= 1;
		sas_.bufPtr[sas_.curBuffer] = info.secondBuffer;
		sas_.bufSize[sas_.curBuffer] = info.secondBufferByte;
		sas_.fileOffset += (int)info.secondBufferByte;
		sas_.streamOffset = part2Size;
		// At the end, 0 means "no more data needed"; otherwise 0xFFFFFFFF asks for the next.
		info.secondBuffer = sas_.fileOffset >= info.fileDataEnd ? 0 : 0xFFFFFFFF;
		if (part2Size > 0) mem::ReadBytes(assembly + std::max(part1Size, 0), sas_.bufPtr[sas_.curBuffer], (u32)part2Size);
		int bytesConsumed = 0;
		if (!decoder_->Decode(assembly, info.sampleSize, &bytesConsumed, 1, dstData, bytesWritten)) LOGE("ATRAC", "SAS failed to decode assembled packet");
	}
}

// ---- Registry of contexts ----

bool g_atracInited = true;
Atrac2 *g_atracContexts[PSP_MAX_ATRAC_IDS];
u32 g_atracContextTypes[PSP_MAX_ATRAC_IDS] = {PSP_CODEC_AT3PLUS, PSP_CODEC_AT3PLUS, PSP_CODEC_AT3, PSP_CODEC_AT3, 0, 0};
int g_atracMaxContexts = 6;
u32 g_atracBSS = 0;

struct AtracPendingOutput {
	u32 id;
	u32 addr;
	std::vector<u8> data;
};
std::vector<AtracPendingOutput> g_pendingOutput;
u32 g_pendingOutputId = 0;

void WritePendingOutput(u32 id) {
	for (size_t i = 0; i < g_pendingOutput.size(); ++i) {
		if (g_pendingOutput[i].id == id) {
			const AtracPendingOutput &p = g_pendingOutput[i];
			if (mem::IsValidRange(p.addr, (u32)p.data.size())) mem::WriteBytes(p.addr, p.data.data(), (u32)p.data.size());
			g_pendingOutput.erase(g_pendingOutput.begin() + i);
			return;
		}
	}
}

u32 ContextAddress(int atracID) {
	if (!g_atracBSS) {
		// Without a loaded libatrac3plus to hand us its bss, keep the contexts in kernel memory.
		g_atracBSS = kernel::AllocKernelMemory(PSP_MAX_ATRAC_IDS * sizeof(SceAtracContext), "atrac contexts");
		mem::Memset(g_atracBSS, 0, PSP_MAX_ATRAC_IDS * sizeof(SceAtracContext));
	}
	return g_atracBSS + (u32)atracID * sizeof(SceAtracContext);
}

Atrac2 *GetAtrac(int atracID) {
	if (atracID < 0 || atracID >= PSP_MAX_ATRAC_IDS) return nullptr;
	return g_atracContexts[atracID];
}

int AllocAndRegisterAtrac(int codecType) {
	for (int i = 0; i < g_atracMaxContexts; ++i) {
		if (g_atracContextTypes[i] == (u32)codecType && g_atracContexts[i] == nullptr) {
			// The context memory is not cleared: fields like secondBufferByte survive from the
			// previous user of the slot (audio/atrac/stream).
			g_atracContexts[i] = new Atrac2(ContextAddress(i), codecType);
			return i;
		}
	}
	return (int)SCE_ERROR_ATRAC_NO_ATRACID;
}

int UnregisterAndDeleteAtrac(int atracID) {
	if (atracID >= 0 && atracID < PSP_MAX_ATRAC_IDS && g_atracContexts[atracID]) {
		delete g_atracContexts[atracID];
		g_atracContexts[atracID] = nullptr;
		return 0;
	}
	return (int)SCE_ERROR_ATRAC_BAD_ATRACID;
}

// Costs from pspautotests threads/scheduling/callcosts, where a worse thread's share of the CPU
// tells working from waiting. Setting data parses the file and sets up the codec on the CPU (ATRAC3
// ~200 us, ATRAC3+ ~900 us), then sets up the ME (~50 us, ~200 us) and decodes the frames before
// the first sample there: ~900 us and ~3.5 ms, worse threads getting 50-90%. Even with no frame to
// skip the caller waits (audio/atrac/headerfields).
int AtracFrameUs(const Atrac2 *atrac) { return AudioCodecDecodeUs(atrac->CodecType(), atrac->Channels(), atrac->BytesPerFrame()); }

int AtracSetDataDelay(const Atrac2 *atrac) {
	const bool plus = atrac->CodecType() == PSP_CODEC_AT3PLUS;
	sched::SpendUs(plus ? 900 : 200, 0);
	return MEScheduleJob((plus ? 200 : 50) + atrac->SkippedFramesOnSetData() * AtracFrameUs(atrac));
}

// The first decode after setting data also sets up the output on the CPU, ~200 us while the ME
// already decodes, and ATRAC3+'s first frame takes ~500 us longer there (callcosts: ~700 us with
// 50-90% for worse threads, ~3.0 ms with over 90%).
bool g_atracFirstDecode[PSP_MAX_ATRAC_IDS] = {};
constexpr u64 kFirstDecodeBusyUs = 200;
constexpr int kFirstDecodePlusUs = 500;

u32 AtracSetDataError(int ret, const Track &track) {
	if (ret != (int)SCE_ERROR_ATRAC_API_FAIL) return (u32)ret;
	// API_FAIL comes after setting up the codec and trying the frame (audio/atrac/c0mono).
	const int us = AudioCodecInitUs((int)track.codecType, track.channels == 1) + (track.codecType == PSP_CODEC_AT3PLUS ? 214 : 169);
	return DelayResult((u32)ret, MEScheduleJob(us));
}

int AtracDecodeDelay(const Atrac2 *atrac) { return MEScheduleJob(AtracFrameUs(atrac)); }

int AtracValidateData(const Atrac2 *atrac) {
	if (!atrac) return (int)SCE_ERROR_ATRAC_BAD_ATRACID;
	if (atrac->BufferState() == ATRAC_STATUS_NO_DATA) return (int)SCE_ERROR_ATRAC_NO_DATA;
	return 0;
}

int AtracValidateManaged(const Atrac2 *atrac) {
	if (!atrac) return (int)SCE_ERROR_ATRAC_BAD_ATRACID;
	if (atrac->BufferState() == ATRAC_STATUS_NO_DATA) return (int)SCE_ERROR_ATRAC_NO_DATA;
	if (atrac->BufferState() == ATRAC_STATUS_LOW_LEVEL) return (int)SCE_ERROR_ATRAC_IS_LOW_LEVEL;
	if (atrac->BufferState() == ATRAC_STATUS_FOR_SCESAS) return (int)SCE_ERROR_ATRAC_IS_FOR_SCESAS;
	return 0;
}

const u8 *TrackPtr(u32 buffer, u32 size, u32 *maxReadable) {
	*maxReadable = 0;
	if (!buffer || !mem::IsValidAddress(buffer)) return nullptr;
	*maxReadable = ClampValidSize(buffer, 0x10000000);
	return GPtr(buffer);
}

int AnalyzeTrack(u32 buffer, u32 size, Track *track) {
	u32 maxReadable;
	const u8 *p = TrackPtr(buffer, size, &maxReadable);
	std::string error;
	// The small-size check comes before any access, so a null pointer gets that error first.
	if (size < 72) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
	if (p && size > maxReadable && maxReadable) size = maxReadable;
	int ret = AnalyzeAtracTrack(p, size, maxReadable, track, &error);
	if (ret < 0) LOGD("ATRAC", "AnalyzeAtracTrack: %08x %s", ret, error.c_str());
	return ret;
}

int AnalyzeAA3(u32 buffer, u32 size, u32 fileSize, Track *track) {
	u32 maxReadable;
	const u8 *p = TrackPtr(buffer, size, &maxReadable);
	std::string error;
	return AnalyzeAA3Track(p, size, fileSize, track, &error);
}

// ---- sceAtrac3plus ----

u32 sceAtracGetAtracID(int codecType) {
	EnsureInit();
	if (codecType != PSP_CODEC_AT3 && codecType != PSP_CODEC_AT3PLUS) return SCE_ERROR_ATRAC_INVALID_CODECTYPE;
	const int id = AllocAndRegisterAtrac(codecType);
	// 10-30 us of work (callcosts).
	if (id >= 0) sched::SpendUs(20, 0);
	return (u32)id;
}

u32 sceAtracAddStreamData(int atracID, u32 bytesToAdd) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	if (atrac->BufferState() == ATRAC_STATUS_ALL_DATA_LOADED) return SCE_ERROR_ATRAC_ALL_DATA_LOADED;
	return (u32)atrac->AddStreamData(bytesToAdd);
}

// outAddr 0 is valid and skips data.
u32 sceAtracDecodeData(int atracID, u32 outAddr, u32 numSamplesAddr, u32 finishFlagAddr, u32 remainAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	if (outAddr & 1) return SCE_ERROR_ATRAC_BAD_ALIGNMENT;
	if (outAddr != 0 && !mem::IsValidAddress(outAddr)) return SCE_ERROR_ATRAC_SIZE_TOO_SMALL;

	// The ME writes the samples when the call returns, a frame decode later. Games can still be
	// playing out of that memory meanwhile, so keep what was there until then.
	std::vector<u8> previous;
	u32 maxSize = 0;
	if (outAddr) {
		maxSize = ClampValidSize(outAddr, (u32)(atrac->GetOutputChannels() * 2 * atrac->SamplesPerFrame()));
		if (maxSize) previous.assign(GPtr(outAddr), GPtr(outAddr) + maxSize);
	}

	int numSamplesWritten = 0;
	int finish = 0;
	int remains = 0;
	const int ret = (int)atrac->DecodeData(outAddr, &numSamplesWritten, &finish, &remains);
	if (ret != (int)SCE_ERROR_ATRAC_BAD_ATRACID && ret != (int)SCE_ERROR_ATRAC_NO_DATA) {
		if (mem::IsValidRange(numSamplesAddr, 4)) GWrite32(numSamplesAddr, (u32)numSamplesWritten);
		if (mem::IsValidRange(finishFlagAddr, 4)) GWrite32(finishFlagAddr, (u32)finish);
		// On error, no remaining frame value is written.
		if (ret == 0 && mem::IsValidRange(remainAddr, 4)) GWrite32(remainAddr, (u32)remains);
	}

	if (ret == 0 || ret == (int)SCE_ERROR_ATRAC_API_FAIL) {
		const bool first = g_atracFirstDecode[atracID];
		g_atracFirstDecode[atracID] = false;
		int delay = first && atrac->CodecType() == PSP_CODEC_AT3PLUS ? MEScheduleJob(AtracFrameUs(atrac) + kFirstDecodePlusUs)
		                                                             : AtracDecodeDelay(atrac);
		if (first) {
			const u64 doneUs = sched::NowUs() + (u64)delay;
			sched::SpendUs(std::min<u64>(kFirstDecodeBusyUs, (u64)delay), 0);
			delay = (int)(doneUs > sched::NowUs() ? doneUs - sched::NowUs() : 0);
		}
		const u32 written = std::min((u32)(atrac->GetOutputChannels() * 2 * numSamplesWritten), (u32)previous.size());
		if (outAddr && written != 0) {
			AtracPendingOutput pending{++g_pendingOutputId, outAddr, {}};
			pending.data.assign(GPtr(outAddr), GPtr(outAddr) + written);
			memcpy(GPtr(outAddr), previous.data(), previous.size());
			const u32 id = pending.id;
			g_pendingOutput.push_back(std::move(pending));
			// Just ahead of the thread waking up (same deadline, earlier event).
			sched::ScheduleEvent(sched::NowUs() + (u64)delay, [id]() { WritePendingOutput(id); });
			if (sched::InInterrupt() || !sched::DispatchEnabled()) WritePendingOutput(id);
		}
		return DelayResult((u32)ret, delay);
	}
	return (u32)ret;
}

u32 sceAtracReleaseResources() { return 0; }

u32 sceAtracGetBufferInfoForResetting(int atracID, int sample, u32 bufferInfoAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	if (!mem::IsValidRange(bufferInfoAddr, sizeof(AtracResetBufferInfo))) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	// Early errors leave the guest struct untouched, so read it first and always write it back.
	AtracResetBufferInfo info;
	mem::ReadBytes(&info, bufferInfoAddr, sizeof(info));
	bool delay = false;
	int ret = atrac->GetBufferInfoForResetting(&info, sample, &delay);
	mem::WriteBytes(bufferInfoAddr, &info, sizeof(info));
	if (delay) return DelayResult((u32)ret, 300);
	return (u32)ret;
}

u32 WriteOut(u32 addr, u32 value) {
	if (mem::IsValidRange(addr, 4)) {
		GWrite32(addr, value);
		return 0;
	}
	return 0;
}

u32 sceAtracGetBitrate(int atracID, u32 outBitrateAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	return WriteOut(outBitrateAddr, (u32)atrac->Bitrate());
}

u32 sceAtracGetChannel(int atracID, u32 channelAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	return WriteOut(channelAddr, (u32)atrac->Channels());
}

u32 sceAtracGetLoopStatus(int atracID, u32 loopNumAddr, u32 statusAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	if (mem::IsValidRange(loopNumAddr, 4)) GWrite32(loopNumAddr, (u32)atrac->LoopNum());
	return WriteOut(statusAddr, (u32)atrac->LoopStatus());
}

u32 sceAtracGetInternalErrorInfo(int atracID, u32 errorAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	return WriteOut(errorAddr, atrac->GetInternalCodecError());
}

u32 sceAtracGetMaxSample(int atracID, u32 maxSamplesAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	return WriteOut(maxSamplesAddr, (u32)atrac->SamplesPerFrame());
}

u32 sceAtracGetNextDecodePosition(int atracID, u32 outposAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	if (!mem::IsValidRange(outposAddr, 4)) return 0;
	int pos = 0;
	int ret = atrac->GetNextDecodePosition(&pos);
	if (ret < 0) return (u32)ret;
	GWrite32(outposAddr, (u32)pos);
	return 0;
}

u32 sceAtracGetNextSample(int atracID, u32 outNAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	return WriteOut(outNAddr, atrac->GetNextSamples());
}

u32 sceAtracGetRemainFrame(int atracID, u32 remainAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	if (!mem::IsValidRange(remainAddr, 4)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	GWrite32(remainAddr, (u32)atrac->RemainingFrames());
	return 0;
}

u32 sceAtracGetSecondBufferInfo(int atracID, u32 fileOffsetAddr, u32 desiredSizeAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	if (!mem::IsValidRange(fileOffsetAddr, 4) || !mem::IsValidRange(desiredSizeAddr, 4)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	u32 fileOffset = 0, desiredSize = 0;
	int result = atrac->GetSecondBufferInfo(&fileOffset, &desiredSize);
	GWrite32(fileOffsetAddr, fileOffset);
	GWrite32(desiredSizeAddr, desiredSize);
	return (u32)result;
}

u32 sceAtracGetSoundSample(int atracID, u32 outEndSampleAddr, u32 outLoopStartSampleAddr, u32 outLoopEndSampleAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	int endSample = -1, loopStart = -1, loopEnd = -1;
	int retval = atrac->GetSoundSample(&endSample, &loopStart, &loopEnd);
	if (retval < 0) return (u32)retval;
	if (mem::IsValidRange(outEndSampleAddr, 4)) GWrite32(outEndSampleAddr, (u32)endSample);
	if (mem::IsValidRange(outLoopStartSampleAddr, 4)) GWrite32(outLoopStartSampleAddr, (u32)loopStart);
	if (mem::IsValidRange(outLoopEndSampleAddr, 4)) GWrite32(outLoopEndSampleAddr, (u32)loopEnd);
	return (u32)retval;
}

u32 sceAtracGetStreamDataInfo(int atracID, u32 writePtrAddr, u32 writableBytesAddr, u32 readOffsetAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	u32 writePtr = 0, writableBytes = 0, readOffset = 0;
	atrac->GetStreamDataInfo(&writePtr, &writableBytes, &readOffset);
	if (mem::IsValidRange(writePtrAddr, 4)) GWrite32(writePtrAddr, writePtr);
	if (mem::IsValidRange(writableBytesAddr, 4)) GWrite32(writableBytesAddr, writableBytes);
	if (mem::IsValidRange(readOffsetAddr, 4)) GWrite32(readOffsetAddr, readOffset);
	return 0;
}

u32 sceAtracReleaseAtracID(int atracID) { return (u32)UnregisterAndDeleteAtrac(atracID); }

u32 sceAtracResetPlayPosition(int atracID, int sample, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	bool delay = false;
	int res = atrac->ResetPlayPosition(sample, bytesWrittenFirstBuf, bytesWrittenSecondBuf, &delay);
	if (res < 0) return delay ? DelayResult((u32)res, 200) : (u32)res;
	return DelayResult((u32)res, 3000);
}

u32 SetDataCommon(int atracID, u32 buffer, u32 readSize, u32 bufferSize, int outputChannels, bool checkCodec, bool allowNotMono) {
	Atrac2 *atrac = GetAtrac(atracID);
	if (!atrac) return SCE_ERROR_ATRAC_BAD_ATRACID;
	if (readSize > bufferSize) return SCE_ERROR_ATRAC_INCORRECT_READ_SIZE;
	Track track;
	int ret = AnalyzeTrack(buffer, readSize, &track);
	if (ret < 0) return (u32)ret;
	if (checkCodec && track.codecType != g_atracContextTypes[atracID]) return SCE_ERROR_ATRAC_WRONG_CODECTYPE;
	ret = atrac->SetData(track, buffer, readSize, bufferSize, 0, outputChannels, false);
	if (ret < 0 && !(allowNotMono && ret == (int)SCE_ERROR_ATRAC_NOT_MONO)) return AtracSetDataError(ret, track);
	g_atracFirstDecode[atracID] = true;
	return DelayResult((u32)ret, AtracSetDataDelay(atrac));
}

u32 sceAtracSetHalfwayBuffer(int atracID, u32 buffer, u32 readSize, u32 bufferSize) {
	return SetDataCommon(atracID, buffer, readSize, bufferSize, 2, true, false);
}

u32 sceAtracSetSecondBuffer(int atracID, u32 secondBuffer, u32 secondBufferSize) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return (u32)err;
	return (u32)atrac->SetSecondBuffer(secondBuffer, secondBufferSize);
}

u32 sceAtracSetData(int atracID, u32 buffer, u32 bufferSize) {
	return SetDataCommon(atracID, buffer, bufferSize, bufferSize, 2, true, false);
}

int SetDataAndGetIDCommon(u32 buffer, u32 readSize, u32 bufferSize, int outputChannels, bool requireMono, bool isAA3, u32 fileSize) {
	EnsureInit();
	Track track;
	int ret = isAA3 ? AnalyzeAA3(buffer, readSize, fileSize, &track) : AnalyzeTrack(buffer, readSize, &track);
	if (ret < 0) return ret;
	if (requireMono && track.channels != 1) return (int)SCE_ERROR_ATRAC_NOT_MONO;
	int atracID = AllocAndRegisterAtrac((int)track.codecType);
	if (atracID < 0) return atracID;
	ret = g_atracContexts[atracID]->SetData(track, buffer, readSize, bufferSize, fileSize, outputChannels, isAA3);
	if (ret < 0 && !(outputChannels == 1 && ret == (int)SCE_ERROR_ATRAC_NOT_MONO)) {
		UnregisterAndDeleteAtrac(atracID);
		return (int)AtracSetDataError(ret, track);
	}
	g_atracFirstDecode[atracID] = true;
	return (int)DelayResult((u32)atracID, AtracSetDataDelay(g_atracContexts[atracID]));
}

int sceAtracSetDataAndGetID(u32 buffer, int bufferSize) {
	// A negative size happens in Tales of VS; it can't really be that big.
	if (bufferSize < 0) bufferSize = 0x10000000;
	return SetDataAndGetIDCommon(buffer, (u32)bufferSize, (u32)bufferSize, 2, false, false, 0);
}

int sceAtracSetHalfwayBufferAndGetID(u32 buffer, u32 readSize, u32 bufferSize) {
	if (readSize > bufferSize) return (int)SCE_ERROR_ATRAC_INCORRECT_READ_SIZE;
	return SetDataAndGetIDCommon(buffer, readSize, bufferSize, 2, false, false, 0);
}

u32 sceAtracStartEntry() { return 0; }

u32 sceAtracSetLoopNum(int atracID, int loopNum) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return (u32)err;
	return (u32)atrac->SetLoopNum(loopNum);
}

int sceAtracReinit(int at3Count, int at3plusCount) {
	for (int i = 0; i < PSP_MAX_ATRAC_IDS; ++i) {
		if (g_atracContexts[i] != nullptr) return (int)SCE_KERNEL_ERROR_BUSY;
	}
	// Setting up the codec fails in an interrupt handler, while deinit still works.
	if (!g_atracInited && (at3Count > 0 || at3plusCount > 0) && sched::InInterrupt()) return (int)SCE_AVCODEC_ERROR_INVALID_DATA;

	memset(g_atracContextTypes, 0, sizeof(g_atracContextTypes));
	int next = 0;
	int space = g_atracMaxContexts;

	if (at3Count == 0 && at3plusCount == 0) {
		g_atracInited = false;
		return (int)DelayResult(0, 200);
	}
	// ATRAC3+ first; these cost double. Signed: 9999 tries to allocate, -1 does not.
	for (int i = 0; i < at3plusCount; ++i) {
		space -= 2;
		if (space >= 0) g_atracContextTypes[next++] = PSP_CODEC_AT3PLUS;
	}
	for (int i = 0; i < at3Count; ++i) {
		space -= 1;
		if (space >= 0) g_atracContextTypes[next++] = PSP_CODEC_AT3;
	}
	const int result = space >= 0 ? 0 : (int)SCE_KERNEL_ERROR_OUT_OF_MEMORY;
	if (g_atracInited || next == 0) {
		g_atracInited = true;
		return result;
	}
	g_atracInited = true;
	return (int)DelayResult((u32)result, 400);
}

int sceAtracGetOutputChannel(int atracID, u32 outputChanPtr) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateData(atrac);
	if (err != 0) return err;
	return (int)WriteOut(outputChanPtr, (u32)atrac->GetOutputChannels());
}

int sceAtracIsSecondBufferNeeded(int atracID) {
	Atrac2 *atrac = GetAtrac(atracID);
	int err = AtracValidateManaged(atrac);
	if (err != 0) return err;
	return atrac->BufferState() == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER ? 1 : 0;
}

int sceAtracSetMOutHalfwayBuffer(int atracID, u32 buffer, u32 readSize, u32 bufferSize) {
	return (int)SetDataCommon(atracID, buffer, readSize, bufferSize, 1, false, true);
}

u32 sceAtracSetMOutData(int atracID, u32 buffer, u32 bufferSize) {
	return SetDataCommon(atracID, buffer, bufferSize, bufferSize, 1, false, true);
}

int sceAtracSetMOutDataAndGetID(u32 buffer, u32 bufferSize) {
	return SetDataAndGetIDCommon(buffer, bufferSize, bufferSize, 1, true, false, 0);
}

int sceAtracSetMOutHalfwayBufferAndGetID(u32 buffer, u32 readSize, u32 bufferSize) {
	if (readSize > bufferSize) return (int)SCE_ERROR_ATRAC_INCORRECT_READ_SIZE;
	return SetDataAndGetIDCommon(buffer, readSize, bufferSize, 1, true, false, 0);
}

int sceAtracSetAA3DataAndGetID(u32 buffer, u32 bufferSize, u32 fileSize, u32 metadataSizeAddr) {
	return SetDataAndGetIDCommon(buffer, bufferSize, bufferSize, 2, false, true, fileSize);
}

int sceAtracSetAA3HalfwayBufferAndGetID(u32 buffer, u32 readSize, u32 bufferSize, u32 fileSize) {
	if (readSize > bufferSize) return (int)SCE_ERROR_ATRAC_INCORRECT_READ_SIZE;
	return SetDataAndGetIDCommon(buffer, readSize, bufferSize, 2, false, true, fileSize);
}

u32 _sceAtracGetContextAddress(int atracID) {
	Atrac2 *atrac = GetAtrac(atracID);
	if (!atrac) return 0;
	return atrac->ContextAddr();
}

int sceAtracLowLevelInitDecoder(int atracID, u32 paramsAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	if (!atrac) return (int)SCE_ERROR_ATRAC_BAD_ATRACID;
	if (!mem::IsValidRange(paramsAddr, 12)) return 0;
	Atrac3LowLevelParams params;
	mem::ReadBytes(&params, paramsAddr, sizeof(params));
	atrac->InitLowLevel(params, (int)g_atracContextTypes[atracID]);
	return 0;
}

int sceAtracLowLevelDecode(int atracID, u32 sourceAddr, u32 sourceBytesConsumedAddr, u32 samplesAddr, u32 sampleBytesAddr) {
	Atrac2 *atrac = GetAtrac(atracID);
	if (!atrac) return (int)SCE_ERROR_ATRAC_BAD_ATRACID;
	const u32 frameBytes = (u32)std::max(atrac->BytesPerFrame(), 1);
	const u32 outBytes = (u32)(ATRAC3PLUS_MAX_SAMPLES * 2 * 2);
	const u8 *src = mem::GetPointer(sourceAddr, frameBytes);
	s16 *out = (s16 *)mem::GetPointer(samplesAddr, 2);
	if (!src || !mem::IsValidRange(sourceBytesConsumedAddr, 4) || !out || !mem::IsValidRange(sampleBytesAddr, 4)) return 0;
	std::vector<s16> tmp(outBytes / 2);
	int bytesConsumed = 0;
	int bytesWritten = 0;
	int retval = atrac->DecodeLowLevel(src, &bytesConsumed, tmp.data(), &bytesWritten);
	if (u8 *o = mem::GetPointer(samplesAddr, (u32)bytesWritten)) memcpy(o, tmp.data(), bytesWritten);
	GWrite32(sourceBytesConsumedAddr, (u32)bytesConsumed);
	GWrite32(sampleBytesAddr, (u32)bytesWritten);
	return (int)DelayResult((u32)retval, AtracDecodeDelay(atrac));
}

// ---- The sceSas binding ----

int SasBindContext(u32 contextAddr) {
	int atracID = -1;
	for (int i = 0; i < PSP_MAX_ATRAC_IDS; i++) {
		if (g_atracContexts[i] && g_atracContexts[i]->ContextAddr() == contextAddr) {
			atracID = i;
			break;
		}
	}
	if (atracID < 0) return -1;
	g_atracContexts[atracID]->CheckForSas();
	return atracID;
}

void SasDecode(int atracID, s16 *out, int *samples, int *finish) {
	Atrac2 *atrac = GetAtrac(atracID);
	if (!atrac) {
		*samples = 0;
		*finish = 1;
		return;
	}
	atrac->DecodeForSas(out, samples, finish);
}

int SasAddStreamData(int atracID, u32 bufPtr, u32 bytes) {
	Atrac2 *atrac = GetAtrac(atracID);
	return atrac ? atrac->EnqueueForSas(bufPtr, bytes) : 0;
}

}  // namespace

void AtracNotifyLoadModule(int version, u32 crc, u32 bssAddr, int bssSize) {
	if (bssAddr && bssSize >= (int)(PSP_MAX_ATRAC_IDS * sizeof(SceAtracContext))) {
		g_atracBSS = bssAddr;
		mem::Memset(g_atracBSS, 0, PSP_MAX_ATRAC_IDS * sizeof(SceAtracContext));
	}
	g_atracMaxContexts = version <= 0x101 ? 4 : 6;
}

void AtracNotifyUnloadModule() { g_atracMaxContexts = 6; }

void Register_sceAtrac3plus() {
	SasAtracHooks hooks;
	hooks.bindContext = &SasBindContext;
	hooks.decode = &SasDecode;
	hooks.addStreamData = &SasAddStreamData;
	SetSasAtracHooks(hooks);

	static const hle::HleFunction funcs[] = {
		HLEFN(0x7DB31251, sceAtracAddStreamData),
		HLEFN(0x6A8C3CD5, sceAtracDecodeData),
		HLEFN(0xD5C28CC0, sceAtracReleaseResources),
		HLEFN(0x780F88D1, sceAtracGetAtracID),
		HLEFN(0xCA3CA3D2, sceAtracGetBufferInfoForResetting),
		HLEFN(0xA554A158, sceAtracGetBitrate),
		HLEFN(0x31668BAA, sceAtracGetChannel),
		HLEFN(0xFAA4F89B, sceAtracGetLoopStatus),
		HLEFN(0xE88F759B, sceAtracGetInternalErrorInfo),
		HLEFN(0xD6A5F2F7, sceAtracGetMaxSample),
		HLEFN(0xE23E3A35, sceAtracGetNextDecodePosition),
		HLEFN(0x36FAABFB, sceAtracGetNextSample),
		HLEFN(0x9AE849A7, sceAtracGetRemainFrame),
		HLEFN(0x83E85EA0, sceAtracGetSecondBufferInfo),
		HLEFN(0xA2BBA8BE, sceAtracGetSoundSample),
		HLEFN(0x5D268707, sceAtracGetStreamDataInfo),
		HLEFN(0x61EB33F5, sceAtracReleaseAtracID),
		HLEFN(0x644E5607, sceAtracResetPlayPosition),
		HLEFN(0x3F6E26B5, sceAtracSetHalfwayBuffer),
		HLEFN(0x83BF7AFD, sceAtracSetSecondBuffer),
		HLEFN(0x0E2A73AB, sceAtracSetData),
		HLEFN(0x7A20E7AF, sceAtracSetDataAndGetID),
		HLEFN(0xD1F59FDB, sceAtracStartEntry),
		HLEFN(0x868120B5, sceAtracSetLoopNum),
		HLEFN(0x132F1ECA, sceAtracReinit),
		HLEFN(0xECA32A99, sceAtracIsSecondBufferNeeded),
		HLEFN(0x0FAE370E, sceAtracSetHalfwayBufferAndGetID),
		HLEFN(0x2DD3E298, sceAtracGetBufferInfoForResetting),
		HLEFN(0x5CF9D852, sceAtracSetMOutHalfwayBuffer),
		HLEFN(0xF6837A1A, sceAtracSetMOutData),
		HLEFN(0x472E3825, sceAtracSetMOutDataAndGetID),
		HLEFN(0x9CD7DE03, sceAtracSetMOutHalfwayBufferAndGetID),
		HLEFN(0xB3B5D042, sceAtracGetOutputChannel),
		HLEFN(0x5622B7C1, sceAtracSetAA3DataAndGetID),
		HLEFN(0x5DD66588, sceAtracSetAA3HalfwayBufferAndGetID),
		HLEFN(0x231FC6B7, _sceAtracGetContextAddress),
		HLEFN(0x1575D64B, sceAtracLowLevelInitDecoder),
		HLEFN(0x0C116E1B, sceAtracLowLevelDecode),
	};
	hle::RegisterModule("sceATRAC3plus_Library", funcs);
	hle::RegisterModule("sceAtrac3plus", funcs);
}

}  // namespace audiohle
