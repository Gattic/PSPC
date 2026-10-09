// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceMp3 and sceAac: stream decoding through a game-managed input buffer (the "Au" context).
// Ported from PPSSPP's Core/HLE/sceMp3.cpp, sceAac.cpp and the AuCtx part of
// Core/HW/SimpleAudioDec.cpp (https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later).
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

constexpr int AU_BUF_MIN_SIZE = 8192;
constexpr int PCM_BUF_MIN_SIZE = 9216;
constexpr size_t MP3_MAX_HANDLES = 2;
constexpr int kMp3DecodeDelayUs = 2400;
constexpr u32 ERROR_AAC_INVALID_ADDRESS = 0x80691002;
constexpr u32 ERROR_AAC_INVALID_PARAMETER = 0x80691003;

class AuCtx {
public:
	~AuCtx() { delete decoder; }

	u32 AuDecode(u32 pcmAddr);
	u32 AuNotifyAddStreamData(int size);
	int AuCheckStreamDataNeeded() { return AuStreamBytesNeeded() > 0 ? 1 : 0; }
	int AuStreamBytesNeeded();
	int AuStreamWorkareaSize() const { return decoder && decoder->GetCodecType() == PSP_CODEC_MP3 ? 0x05c0 : 0; }
	int AuStreamHalfSize() const { return ((int)AuBufSize - AuStreamWorkareaSize()) / 2; }
	int AuStreamWriteOffset() const;
	u32 AuResetPlayPosition();
	u32 AuResetPlayPositionByFrame(int frame);
	u32 AuGetInfoToAddStreamData(u32 bufPtr, u32 sizePtr, u32 srcPosPtr);

	void SetReadPos(int pos) { readPos = pos; }
	int ReadPos() const { return readPos; }

	u64 startPos = 0;
	u64 endPos = 0;
	u32 AuBuf = 0;
	u32 AuBufSize = 0;
	u32 PCMBuf = 0;
	u32 PCMBufSize = 0;

	int freq = -1;
	int BitRate = 0;
	int SamplingRate = -1;
	int Channels = 0;
	int Version = -1;

	u32 SumDecodedSamples = 0;
	int LoopNum = -1;
	u32 MaxOutputSample = 0;
	int FrameNum = 0;

	AudioDecoder *decoder = nullptr;

private:
	size_t FindNextMp3Sync() const;

	std::vector<u8> sourcebuff;
	int AuBufAvailable = 0;
	int readPos = 0;
	int nextOutputHalf = 0;
};

size_t AuCtx::FindNextMp3Sync() const {
	if (sourcebuff.size() < 3) return 0;
	for (size_t i = 0; i < sourcebuff.size() - 2; ++i) {
		if (sourcebuff[i] == 0xFF && (sourcebuff[i + 1] & 0xC0) == 0xC0) return i;
	}
	return 0;
}

u32 AuCtx::AuDecode(u32 pcmAddr) {
	const u32 outptr = PCMBuf + nextOutputHalf * PCMBufSize / 2;
	u8 *outbuf = mem::GetPointer(outptr, PCMBufSize / 2);
	int outpcmbufsize = 0;
	if (pcmAddr && mem::IsValidRange(pcmAddr, 4)) GWrite32(pcmAddr, outptr);

	// The stream is over once the decoder has consumed up to endPos.
	bool end = (s64)readPos - AuBufAvailable >= (s64)endPos;
	if (!end && !sourcebuff.empty()) {
		int nextSync = 0;
		if (decoder->GetCodecType() == PSP_CODEC_MP3) nextSync = (int)FindNextMp3Sync();
		int inbytesConsumed = 0;
		int outSamples = 0;
		// Never let the decoder write past this half of the PCM buffer.
		std::vector<s16> tmp(4096 * 2);
		decoder->Decode(&sourcebuff[nextSync], (int)sourcebuff.size() - nextSync, &inbytesConsumed, 2, tmp.data(), &outSamples);
		outpcmbufsize = outSamples * 2 * (int)sizeof(s16);
		if (outbuf) memcpy(outbuf, tmp.data(), std::min<u32>((u32)outpcmbufsize, PCMBufSize / 2));
		if (outpcmbufsize == 0) {
			// Nothing came out, presumably the end of the stream.
			AuBufAvailable = 0;
			sourcebuff.clear();
		} else {
			SumDecodedSamples += (u32)outSamples;
			int srcPos = inbytesConsumed + nextSync;
			if (srcPos > (int)sourcebuff.size()) srcPos = (int)sourcebuff.size();
			if (srcPos > 0) sourcebuff.erase(sourcebuff.begin(), sourcebuff.begin() + srcPos);
			AuBufAvailable -= srcPos;
		}
	}

	// The hardware rewinds in the same call that decodes the last frame.
	end = (s64)readPos - AuBufAvailable >= (s64)endPos;
	if (end && LoopNum != 0) {
		SumDecodedSamples = 0;
		readPos = (int)startPos;
		if (LoopNum > 0) LoopNum--;
	}

	if (outpcmbufsize == 0 && !end) {
		outpcmbufsize = (int)PCMBufSize / 2;
		if (outbuf) memset(outbuf, 0, outpcmbufsize);
	} else if ((u32)outpcmbufsize < PCMBufSize) {
		if (outbuf && (u32)outpcmbufsize < PCMBufSize / 2) memset(outbuf + outpcmbufsize, 0, PCMBufSize / 2 - outpcmbufsize);
	}
	nextOutputHalf ^= 1;
	return (u32)outpcmbufsize;
}

int AuCtx::AuStreamBytesNeeded() {
	if (decoder->GetCodecType() == PSP_CODEC_MP3) {
		if ((s64)readPos >= (s64)endPos) return 0;
		// After the workarea the buffer is double buffered: a half only opens up once the
		// decoder has consumed past its end (audio/mp3/checkneeded).
		const int half = AuStreamHalfSize();
		if (half <= 0) return 0;
		const s64 written = (s64)readPos - (s64)startPos;
		const s64 consumed = written - AuBufAvailable;
		const s64 halvesDone = consumed / half - ((consumed % half < 0) ? 1 : 0);
		return (int)std::max((s64)0, (halvesDone + 2) * half - written);
	}
	return std::min((int)AuBufSize - AuBufAvailable, (int)endPos - readPos);
}

int AuCtx::AuStreamWriteOffset() const {
	const int size = AuStreamHalfSize() * 2;
	if (size <= 0) return 0;
	s64 pos = ((s64)readPos - (s64)startPos) % size;
	if (pos < 0) pos += size;
	return (int)pos;
}

u32 AuCtx::AuNotifyAddStreamData(int size) {
	const int offset = AuStreamWorkareaSize();
	const int writeOffset = AuStreamWriteOffset();
	readPos += size;
	AuBufAvailable += size;
	if (size > 0 && (s64)offset + writeOffset + size <= (s64)AuBufSize) {
		if (const u8 *src = mem::GetPointer(AuBuf + offset + writeOffset, (u32)size)) {
			sourcebuff.insert(sourcebuff.end(), src, src + size);
		}
	}
	return 0;
}

u32 AuCtx::AuGetInfoToAddStreamData(u32 bufPtr, u32 sizePtr, u32 srcPosPtr) {
	const int readsize = AuStreamBytesNeeded();
	const int offset = AuStreamWorkareaSize();
	auto put = [](u32 addr, u32 v) {
		if (mem::IsValidRange(addr, 4)) GWrite32(addr, v);
	};
	if (readsize != 0) {
		put(bufPtr, AuBuf + offset + AuStreamWriteOffset());
		put(sizePtr, (u32)readsize);
		put(srcPosPtr, (u32)readPos);
	} else {
		put(bufPtr, 0);
		put(sizePtr, 0);
		put(srcPosPtr, 0);
	}
	return 0;
}

u32 AuCtx::AuResetPlayPositionByFrame(int frame) {
	const u32 bytesPerSecond = (MaxOutputSample / 8) * BitRate * 1000;
	readPos = (int)(startPos + ((u64)frame * bytesPerSecond) / (SamplingRate > 0 ? SamplingRate : 44100));
	// It seems to consistently seek one byte before.
	if (frame != 0) readPos -= 1;
	SumDecodedSamples = (u32)frame * MaxOutputSample;
	AuBufAvailable = 0;
	sourcebuff.clear();
	return 0;
}

u32 AuCtx::AuResetPlayPosition() {
	readPos = (int)startPos;
	SumDecodedSamples = 0;
	AuBufAvailable = 0;
	sourcebuff.clear();
	return 0;
}

// ---- sceMp3 ----

std::map<u32, std::unique_ptr<AuCtx>> g_mp3Map;
bool g_mp3ResourceInited = false;

AuCtx *GetMp3Ctx(u32 mp3) {
	auto it = g_mp3Map.find(mp3);
	return it == g_mp3Map.end() ? nullptr : it->second.get();
}

// Error for a missing handle, as most of the API reports it.
u32 MissingHandle(u32 mp3, u32 unreservedError) {
	return mp3 >= MP3_MAX_HANDLES ? SCE_MP3_ERROR_INVALID_HANDLE : unreservedError;
}

int sceMp3Decode(u32 mp3, u32 outPcmPtr) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0 || ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	int pcmBytes = (int)ctx->AuDecode(outPcmPtr);
	if (pcmBytes > 0) return (int)DelayResult((u32)pcmBytes, kMp3DecodeDelayUs);
	return pcmBytes;
}

int sceMp3ResetPlayPosition(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0 || ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	return (int)ctx->AuResetPlayPosition();
}

int sceMp3CheckStreamDataNeeded(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_UNRESERVED_HANDLE;
	return ctx->AuCheckStreamDataNeeded();
}

u32 sceMp3ReserveMp3Handle(u32 mp3Addr) {
	if (!g_mp3ResourceInited) return SCE_MP3_ERROR_NO_RESOURCE_AVAIL;
	if (g_mp3Map.size() >= MP3_MAX_HANDLES) return SCE_MP3_ERROR_NO_RESOURCE_AVAIL;
	if (mp3Addr != 0 && !mem::IsValidRange(mp3Addr, 32)) return SCE_KERNEL_ERROR_INVALID_POINTER;

	auto au = std::make_unique<AuCtx>();
	if (mp3Addr) {
		au->startPos = (u64)GRead32(mp3Addr) | ((u64)GRead32(mp3Addr + 4) << 32);
		au->endPos = (u64)GRead32(mp3Addr + 8) | ((u64)GRead32(mp3Addr + 12) << 32);
		au->AuBuf = GRead32(mp3Addr + 16);
		au->AuBufSize = GRead32(mp3Addr + 20);
		au->PCMBuf = GRead32(mp3Addr + 24);
		au->PCMBufSize = GRead32(mp3Addr + 28);
		if (au->startPos >= au->endPos) return SCE_MP3_ERROR_BAD_SIZE;
		if (!au->AuBuf || !au->PCMBuf) return SCE_MP3_ERROR_BAD_ADDR;
		if ((int)au->AuBufSize < AU_BUF_MIN_SIZE || (int)au->PCMBufSize < PCM_BUF_MIN_SIZE) return SCE_MP3_ERROR_BAD_SIZE;
	}
	au->SetReadPos((int)au->startPos);
	au->decoder = CreateAudioDecoder(PSP_CODEC_MP3);

	// The lowest free handle.
	u32 handle = 0;
	while (g_mp3Map.count(handle)) handle++;
	g_mp3Map[handle] = std::move(au);
	return handle;
}

int sceMp3InitResource() {
	EnsureInit();
	if (g_mp3ResourceInited) return 0;
	g_mp3ResourceInited = true;
	return (int)DelayResult(0, 200);
}

int sceMp3TermResource() {
	if (!g_mp3ResourceInited) return 0;
	g_mp3Map.clear();
	g_mp3ResourceInited = false;
	return (int)DelayResult(0, 100);
}

int CalcMp3Channels(int bitval) {
	if (bitval == 0 || bitval == 1 || bitval == 2) return 2;
	if (bitval == 3) return 1;
	return -1;
}

int CalcMp3SampleRate(int bitval, int version) {
	static const int v1[] = {44100, 48000, 32000, -1};
	static const int v2[] = {22050, 24000, 16000, -1};
	static const int v25[] = {11025, 12000, 8000, -1};
	if (version == 3) return v1[bitval];
	if (version == 2) return v2[bitval];
	if (version == 0) return v25[bitval];
	return -1;
}

int CalcMp3Bitrate(int bitval, int version, int layer) {
	static const int v1l1[] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, -1};
	static const int v1l2[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, -1};
	static const int v1l3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1};
	static const int v2l1[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, -1};
	static const int v2l23[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1};
	if (version == 3) {
		if (layer == 3) return v1l1[bitval];
		if (layer == 2) return v1l2[bitval];
		if (layer == 1) return v1l3[bitval];
		return -1;
	}
	if (version == 2 || version == 0) {
		if (layer == 3) return v2l1[bitval];
		if (layer == 1 || layer == 2) return v2l23[bitval];
		return -1;
	}
	return -1;
}

int CalcMp3SamplesPerFrame(int versionBits, int layerBits) {
	if (versionBits == 1 || layerBits == 0) return -1;
	if (layerBits == 3) return 384;
	if (layerBits == 2 || versionBits == 3) return 1152;
	return 576;
}

int FindMp3Header(AuCtx *ctx, int &header, int end) {
	const u32 addr = ctx->AuBuf + ctx->AuStreamWorkareaSize();
	if (const u8 *ptr = mem::GetPointer(addr, (u32)end + 4)) {
		for (int offset = 0; offset < end; ++offset) {
			if (ptr[offset] == 0xFF && (ptr[offset + 1] & 0xC0) == 0xC0) {
				header = (ptr[offset] << 24) | (ptr[offset + 1] << 16) | (ptr[offset + 2] << 8) | ptr[offset + 3];
				return offset;
			}
		}
	}
	return -1;
}

int sceMp3Init(u32 mp3) {
	const u32 sdkver = CompiledSdkVersion();
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_UNRESERVED_HANDLE;

	const int kParseDelayUs = 500;
	// The header can be offset by at most 1439 bytes (an ID3 tag is skipped by frame sync).
	int header = 0;
	if (FindMp3Header(ctx, header, 1440) < 0) return (int)DelayResult(SCE_AVCODEC_ERROR_INVALID_DATA, kParseDelayUs);

	const int layerBits = (header >> 17) & 0x3;
	const int versionBits = (header >> 19) & 0x3;
	const int bitrate = CalcMp3Bitrate((header >> 12) & 0xF, versionBits, layerBits);
	const int samplerate = CalcMp3SampleRate((header >> 10) & 0x3, versionBits);
	const int channels = CalcMp3Channels((header >> 6) & 0x3);

	if (bitrate == 0 || bitrate == -1) return (int)DelayResult(SCE_AVCODEC_ERROR_INVALID_DATA, kParseDelayUs);
	if (samplerate == -1) return (int)DelayResult(SCE_AVCODEC_ERROR_INVALID_DATA, kParseDelayUs);
	// Newer SDKs need at least 156 bytes (the size of a VBR first frame header).
	if (sdkver >= 0x06000000 && ctx->ReadPos() < 156) return (int)DelayResult(SCE_KERNEL_ERROR_INVALID_VALUE, kParseDelayUs);

	ctx->SamplingRate = samplerate;
	ctx->Channels = channels;
	ctx->BitRate = bitrate;
	ctx->MaxOutputSample = (u32)CalcMp3SamplesPerFrame(versionBits, layerBits);
	ctx->freq = ctx->SamplingRate;

	// libmp3 only accepts rates other than 44.1kHz from games built with SDK 3.09.05+. Like
	// PPSSPP, we only refuse them for programs that declare no SDK version at all (the tests),
	// since user-supplied 48kHz music refusing to play would look like a bug.
	if (samplerate != 44100 && sdkver == 0) return (int)DelayResult(SCE_MP3_ERROR_BAD_SAMPLE_RATE, kParseDelayUs);

	const u32 bytesPerSecond = (ctx->MaxOutputSample / 8) * (u32)ctx->BitRate * 1000;
	const u64 totalBytes = (ctx->endPos & 0xFFFFFFFF) - (ctx->startPos & 0xFFFFFFFF);
	ctx->FrameNum = (int)((totalBytes * (u64)ctx->SamplingRate) / bytesPerSecond);
	ctx->Version = versionBits;
	return (int)DelayResult(0, kParseDelayUs);
}

int sceMp3GetLoopNum(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_UNRESERVED_HANDLE;
	return ctx->LoopNum;
}

int sceMp3GetMaxOutputSample(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0) return (int)SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	if (ctx->AuBuf == 0) return 0;
	return (int)ctx->MaxOutputSample;
}

int sceMp3GetSumDecodedSample(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_UNRESERVED_HANDLE;
	return (int)ctx->SumDecodedSamples;
}

int sceMp3SetLoopNum(u32 mp3, int loop) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_UNRESERVED_HANDLE;
	if (loop < 0) loop = -1;
	ctx->LoopNum = loop;
	return 0;
}

int sceMp3GetMp3ChannelNum(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0) return (int)SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	if (ctx->AuBuf == 0) return 0;
	return ctx->Channels;
}

int sceMp3GetBitRate(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0) return (int)SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	if (ctx->AuBuf == 0) return 0;
	return ctx->BitRate;
}

int sceMp3GetSamplingRate(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0) return (int)SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	if (ctx->AuBuf == 0) return 0;
	return ctx->SamplingRate;
}

int sceMp3GetInfoToAddStreamData(u32 mp3, u32 dstPtr, u32 towritePtr, u32 srcposPtr) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_UNRESERVED_HANDLE;
	return (int)ctx->AuGetInfoToAddStreamData(dstPtr, towritePtr, srcposPtr);
}

int sceMp3NotifyAddStreamData(u32 mp3, int size) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return (int)MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->AuBuf == 0) return (int)SCE_MP3_ERROR_UNRESERVED_HANDLE;
	return (int)ctx->AuNotifyAddStreamData(size);
}

int sceMp3ReleaseMp3Handle(u32 mp3) {
	auto it = g_mp3Map.find(mp3);
	if (it != g_mp3Map.end()) {
		g_mp3Map.erase(it);
		return 0;
	}
	if (mp3 >= MP3_MAX_HANDLES) return (int)SCE_MP3_ERROR_INVALID_HANDLE;
	return 0;  // a double free is ignored
}

u32 sceMp3EndEntry() { return 0; }
u32 sceMp3StartEntry() { return 0; }

u32 sceMp3GetFrameNum(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0 || ctx->AuBuf == 0) return SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	return (u32)ctx->FrameNum;
}

u32 sceMp3GetMPEGVersion(u32 mp3) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return MissingHandle(mp3, SCE_MP3_ERROR_UNRESERVED_HANDLE);
	if (ctx->Version < 0) return SCE_MP3_ERROR_UNRESERVED_HANDLE;
	if (ctx->AuBuf == 0) return 0;
	return (u32)ctx->Version;
}

u32 sceMp3ResetPlayPositionByFrame(u32 mp3, u32 frame) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return MissingHandle(mp3, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
	if (ctx->Version < 0 || ctx->AuBuf == 0) return SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
	if (frame >= (u32)ctx->FrameNum) return SCE_MP3_ERROR_BAD_RESET_FRAME;
	return ctx->AuResetPlayPositionByFrame((int)frame);
}

u32 sceMp3LowLevelInit(u32 mp3, u32 unk) {
	EnsureInit();
	auto ctx = std::make_unique<AuCtx>();
	ctx->decoder = CreateAudioDecoder(PSP_CODEC_MP3);
	// Version 1 marks a low level context.
	ctx->Version = 1;
	g_mp3Map[mp3] = std::move(ctx);
	return DelayResult(0, 600);
}

u32 sceMp3LowLevelDecode(u32 mp3, u32 sourceAddr, u32 sourceBytesConsumedAddr, u32 samplesAddr, u32 sampleBytesAddr) {
	AuCtx *ctx = GetMp3Ctx(mp3);
	if (!ctx) return SCE_MP3_ERROR_INVALID_HANDLE;
	if (!mem::IsValidAddress(sourceAddr) || !mem::IsValidRange(sourceBytesConsumedAddr, 4) || !mem::IsValidAddress(samplesAddr) ||
	    !mem::IsValidRange(sampleBytesAddr, 4))
		return 0xFFFFFFFF;
	u32 avail = 4096;
	while (avail > 0 && !mem::IsValidRange(sourceAddr, avail)) avail /= 2;
	const u8 *inbuff = mem::GetPointer(sourceAddr, avail);
	std::vector<s16> tmp(4096 * 2);
	int outSamples = 0;
	int inbytesConsumed = 0;
	if (!ctx->decoder->Decode(inbuff, (int)avail, &inbytesConsumed, 2, tmp.data(), &outSamples))
		LOGW("MP3", "sceMp3LowLevelDecode: decode failed");
	const int outBytes = outSamples * 2 * (int)sizeof(s16);
	if (u8 *out = mem::GetPointer(samplesAddr, (u32)outBytes)) memcpy(out, tmp.data(), outBytes);
	GWrite32(sourceBytesConsumedAddr, (u32)inbytesConsumed);
	GWrite32(sampleBytesAddr, (u32)outBytes);
	return 0;
}

// ---- sceAac ----

std::map<u32, std::unique_ptr<AuCtx>> g_aacMap;

AuCtx *GetAacCtx(u32 id) {
	auto it = g_aacMap.find(id);
	return it == g_aacMap.end() ? nullptr : it->second.get();
}

u32 sceAacExit(u32 id) {
	auto it = g_aacMap.find(id);
	if (it == g_aacMap.end()) return 0xFFFFFFFF;
	g_aacMap.erase(it);
	return 0;
}

u32 sceAacInit(u32 id) {
	EnsureInit();
	if (!mem::IsValidRange(id, 36)) return ERROR_AAC_INVALID_ADDRESS;
	auto aac = std::make_unique<AuCtx>();
	aac->startPos = (u64)GRead32(id) | ((u64)GRead32(id + 4) << 32);
	aac->endPos = GRead32(id + 8);
	aac->AuBuf = GRead32(id + 16);
	aac->AuBufSize = GRead32(id + 20);
	aac->PCMBuf = GRead32(id + 24);
	aac->PCMBufSize = GRead32(id + 28);
	aac->freq = (int)GRead32(id + 32);
	if (aac->AuBuf == 0 || aac->PCMBuf == 0) return ERROR_AAC_INVALID_ADDRESS;
	if (aac->startPos > aac->endPos) return ERROR_AAC_INVALID_PARAMETER;
	if (aac->AuBufSize < 8192 || aac->PCMBufSize < 8192) return ERROR_AAC_INVALID_PARAMETER;
	if (aac->freq != 24000 && aac->freq != 32000 && aac->freq != 44100 && aac->freq != 48000) return ERROR_AAC_INVALID_PARAMETER;
	aac->Channels = 2;
	aac->MaxOutputSample = aac->PCMBufSize / 4;
	aac->SetReadPos((int)aac->startPos);
	aac->decoder = CreateAacAudio(aac->freq, 2, true);
	g_aacMap[id] = std::move(aac);
	return id;
}

u32 sceAacInitResource(u32 numberIds) { return 0; }
u32 sceAacTermResource() { return 0; }

u32 sceAacDecode(u32 id, u32 pcmAddr) {
	AuCtx *ctx = GetAacCtx(id);
	if (!ctx) return 0xFFFFFFFF;
	return ctx->AuDecode(pcmAddr);
}

u32 sceAacGetLoopNum(u32 id) {
	AuCtx *ctx = GetAacCtx(id);
	return ctx ? (u32)ctx->LoopNum : 0xFFFFFFFF;
}

u32 sceAacSetLoopNum(u32 id, int loop) {
	AuCtx *ctx = GetAacCtx(id);
	if (!ctx) return 0xFFFFFFFF;
	ctx->LoopNum = loop;
	return 0;
}

int sceAacCheckStreamDataNeeded(u32 id) {
	AuCtx *ctx = GetAacCtx(id);
	return ctx ? ctx->AuCheckStreamDataNeeded() : -1;
}

u32 sceAacNotifyAddStreamData(u32 id, int size) {
	AuCtx *ctx = GetAacCtx(id);
	return ctx ? ctx->AuNotifyAddStreamData(size) : 0xFFFFFFFF;
}

u32 sceAacGetInfoToAddStreamData(u32 id, u32 buff, u32 size, u32 srcPos) {
	AuCtx *ctx = GetAacCtx(id);
	return ctx ? ctx->AuGetInfoToAddStreamData(buff, size, srcPos) : 0xFFFFFFFF;
}

u32 sceAacGetMaxOutputSample(u32 id) {
	AuCtx *ctx = GetAacCtx(id);
	return ctx ? ctx->MaxOutputSample : 0xFFFFFFFF;
}

u32 sceAacGetSumDecodedSample(u32 id) {
	AuCtx *ctx = GetAacCtx(id);
	return ctx ? ctx->SumDecodedSamples : 0xFFFFFFFF;
}

u32 sceAacResetPlayPosition(u32 id) {
	AuCtx *ctx = GetAacCtx(id);
	return ctx ? ctx->AuResetPlayPosition() : 0xFFFFFFFF;
}

}  // namespace

void Register_sceMp3() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x07EC321A, sceMp3ReserveMp3Handle),
		HLEFN(0x0DB149F4, sceMp3NotifyAddStreamData),
		HLEFN(0x2A368661, sceMp3ResetPlayPosition),
		HLEFN(0x354D27EA, sceMp3GetSumDecodedSample),
		HLEFN(0x35750070, sceMp3InitResource),
		HLEFN(0x3C2FA058, sceMp3TermResource),
		HLEFN(0x3CEF484F, sceMp3SetLoopNum),
		HLEFN(0x44E07129, sceMp3Init),
		HLEFN(0x732B042A, sceMp3EndEntry),
		HLEFN(0x7F696782, sceMp3GetMp3ChannelNum),
		HLEFN(0x87677E40, sceMp3GetBitRate),
		HLEFN(0x87C263D1, sceMp3GetMaxOutputSample),
		HLEFN(0x8AB81558, sceMp3StartEntry),
		HLEFN(0x8F450998, sceMp3GetSamplingRate),
		HLEFN(0xA703FE0F, sceMp3GetInfoToAddStreamData),
		HLEFN(0xD021C0FB, sceMp3Decode),
		HLEFN(0xD0A56296, sceMp3CheckStreamDataNeeded),
		HLEFN(0xD8F54A51, sceMp3GetLoopNum),
		HLEFN(0xF5478233, sceMp3ReleaseMp3Handle),
		HLEFN(0xAE6D2027, sceMp3GetMPEGVersion),
		HLEFN(0x3548AEC8, sceMp3GetFrameNum),
		HLEFN(0x0840E808, sceMp3ResetPlayPositionByFrame),
		HLEFN(0x1B839B83, sceMp3LowLevelInit),
		HLEFN(0xE3EE2C81, sceMp3LowLevelDecode),
	};
	hle::RegisterModule("sceMp3", funcs);
}

void Register_sceAac() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xE0C89ACA, sceAacInit),
		HLEFN(0x33B8C009, sceAacExit),
		HLEFN(0x5CFFC57C, sceAacInitResource),
		HLEFN(0x23D35CAE, sceAacTermResource),
		HLEFN(0x7E4CFEE4, sceAacDecode),
		HLEFN(0x523347D9, sceAacGetLoopNum),
		HLEFN(0xBBDD6403, sceAacSetLoopNum),
		HLEFN(0xD7C51541, sceAacCheckStreamDataNeeded),
		HLEFN(0xAC6DCBE3, sceAacNotifyAddStreamData),
		HLEFN(0x02098C69, sceAacGetInfoToAddStreamData),
		HLEFN(0x6DC7758A, sceAacGetMaxOutputSample),
		HLEFN(0x506BF66C, sceAacGetSumDecodedSample),
		HLEFN(0xD2DA2BBA, sceAacResetPlayPosition),
		HLESTUB(0xE955E83A, "sceAacLowLevelInit"),
		HLESTUB(0xFA01FCB6, "sceAacLowLevelDecode"),
	};
	hle::RegisterModule("sceAac", funcs);
}

}  // namespace audiohle
