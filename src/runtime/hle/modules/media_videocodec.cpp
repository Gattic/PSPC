// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceVideocodec: the H.264 decoding interface the Media Engine exposes, used by the firmware's
// mpeg.prx (and a few games directly). Ported from PPSSPP's Core/HLE/sceVideocodec.cpp
// (https://github.com/hrydgard/ppsspp, Copyright (c) 2026- PPSSPP Project, GPL-2.0-or-later),
// whose buffer layout follows JPCSP's analysis of sceMpegBaseYCrCbCopy output on hardware.
//
// Also the shared media plumbing (media_hle.h): the Media Engine's memory and the registration of
// every media library.
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "common/log.h"
#include "media/guest_memory.h"
#include "media/h264_decoder.h"
#include "media/ycbcr.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/hle/modules/media_hle.h"
#include "runtime/kernel/errors.h"



namespace mediahle {

namespace {

u8 *GuestPtrAccessor(u32 addr, u32 size) { return mem::GetPointer(addr, size); }

// The 96-byte context the caller hands over. These are the offsets mpeg.prx reads and writes.
enum {
	CTX_MAGIC = 0,    // 0x05100601, the same marker sceAudiocodec's context carries
	CTX_VERSION = 4,  // GetVersion writes 0x78 here
	CTX_STATUS = 8,
	CTX_MEM = 12,
	CTX_OUT_INFO = 16,  // pointer to the 108-byte result descriptor
	CTX_EDRAM = 20,
	CTX_EDRAM_SIZE = 24,
	CTX_AU_DATA = 36,     // the access unit to decode
	CTX_AU_SIZE = 40,
	CTX_YUV_STRUCT = 44,  // type 0: pointer to the eight output buffers
	CTX_EDRAM_RAW = 92,
};

// Fields of the descriptor at CTX_OUT_INFO that mpeg.prx reads back (type 1).
enum {
	OUT_DATA = 0,
	OUT_SIZE = 4,
	OUT_UNK12 = 12,
	OUT_CONSUMED = 44,
	OUT_WIDTH = 48,
	OUT_HEIGHT = 52,
	OUT_FRAME_READY = 60,  // 2 when a frame came out, 1 when it didn't
	OUT_UNK64 = 64,
	OUT_UNK72 = 72,
	OUT_TIMESTAMP = 76,
	OUT_FPS = 80,
	OUT_WIDTH_Y = 96,
	OUT_WIDTH_CR = 100,
	OUT_WIDTH_CB = 104,
};

// The Media Engine's memory: mpeg.prx bounds-checks against 4MB, but only 2MB are the ME's in
// the default configuration (the rest of the EDRAM belongs to the GE). What we hand out lives in
// the top half, out of the way of the addresses mpeg.prx picks itself.
constexpr u32 ME_MEM_SIZE = 2 * 1024 * 1024;
constexpr u32 ME_ALLOC_BASE = ME_MEM_SIZE / 2;
std::vector<u8> g_meRam;
std::map<u32, u32> g_meBlocks;  // start -> size

void MEEnsureRam() {
	if (g_meRam.size() != ME_MEM_SIZE) g_meRam.assign(ME_MEM_SIZE, 0);
}

u32 MEAlloc(u32 size) {
	MEEnsureRam();
	size = (size + 63) & ~63u;
	u32 at = ME_ALLOC_BASE;
	for (const auto &b : g_meBlocks) {
		if (b.first >= at + size) break;
		at = std::max(at, (b.first + b.second + 63) & ~63u);
	}
	if (at + size > ME_MEM_SIZE) return 0;
	g_meBlocks[at] = size;
	return at;
}

void MEFree(u32 addr) { g_meBlocks.erase(addr); }

struct VideocodecCtx {
	media::H264Decoder *decoder = nullptr;
	media::YuvFrame frame;
	int type = 0;
	int frameCount = 0;
	u32 edram = 0;
	u32 frameBuffers = 0;
	u32 frameBuffersSize = 0;
	int frameBufferWidth = 0;
	int frameBufferHeight = 0;
};

std::map<u32, VideocodecCtx> g_videocodecCtxs;

void FreeContext(VideocodecCtx &ctx) {
	delete ctx.decoder;
	ctx.decoder = nullptr;
	if (ctx.edram) MEFree(ctx.edram);
	ctx.edram = 0;
	if (ctx.frameBuffers) MEFree(ctx.frameBuffers);
	ctx.frameBuffers = 0;
}

// The descriptor mpeg.prx passes is empty: on hardware the ME owns the frame buffers and reports
// where it put them. Allocate them and write the eight buffer addresses.
bool PublishFrameBuffers(VideocodecCtx &vctx, u32 structAddr, int width, int height, u32 buffers[8]) {
	u32 offsets[8];
	const u32 total = media::FrameBufferLayout(width, height, nullptr, offsets);
	if (total == 0) return false;
	if (vctx.frameBuffers && (width != vctx.frameBufferWidth || height != vctx.frameBufferHeight)) {
		MEFree(vctx.frameBuffers);
		vctx.frameBuffers = 0;
	}
	if (!vctx.frameBuffers) {
		vctx.frameBuffers = MEAlloc(total);
		if (!vctx.frameBuffers) {
			LOGE("ME", "sceVideocodec: no room in ME memory for %u bytes of frame buffers", total);
			return false;
		}
		vctx.frameBuffersSize = total;
		vctx.frameBufferWidth = width;
		vctx.frameBufferHeight = height;
	}
	for (int i = 0; i < 8; i++) buffers[i] = vctx.frameBuffers + offsets[i];
	// mpeg.prx reads only the eight addresses off the front of this structure.
	if (!mem::IsValidRange(structAddr, 8 * 4)) return false;
	for (int i = 0; i < 8; i++) GWrite32(structAddr + i * 4, buffers[i]);
	return true;
}

void WriteTiled(const u32 *buffers, const media::YuvFrame &f, int width, int height) {
	int sizes[8];
	media::FrameBufferLayout(width, height, sizes, nullptr);
	u8 *dst[8];
	for (int i = 0; i < 8; i++) dst[i] = sizes[i] > 0 ? MEGetPointerRange(buffers[i], (u32)sizes[i]) : nullptr;
	media::TileYCbCr(dst, sizes, f.y.data(), f.cb.data(), f.cr.data(), f.width, f.width / 2, width, height);
}

// Measured at 222MHz in pspautotests video/mp4/mp4timing (see PPSSPP's sceVideocodec.cpp).
constexpr int openUs = 96;
constexpr int getEdramUs = 146;
constexpr int getVersionUs = 94;
constexpr int releaseEdramUs = 66;
constexpr int initUs = 26600;
constexpr int stopUs = 132;
constexpr int deleteUs = 21000;

int MECall(int result, int us) { return (int)audiohle::DelayResult((u32)result, audiohle::MEScheduleJob(us)); }
// Init and Delete take tens of milliseconds for the caller but don't hold the ME that long
// (SAS keeps mixing through them on hardware, audio/timing/meshare).
int CallerWait(int result, int us) { return (int)audiohle::DelayResult((u32)result, us); }

int sceVideocodecOpen(u32 ctxAddr, int type) {
	if (!mem::IsValidRange(ctxAddr, 96)) return -1;
	GWrite32(ctxAddr + CTX_MAGIC, 0x05100601);
	if (!media::H264Decoder::Available()) return -1;
	g_videocodecCtxs[ctxAddr].type = type;
	// The EDRAM the decoder needs, which mpeg.prx reads back and passes to GetEDRAM.
	if (type == 0) GWrite32(ctxAddr + CTX_EDRAM_SIZE, 0x3c2c);
	return MECall(0, openUs);
}

int sceVideocodecInit(u32 ctxAddr, int type) {
	if (!mem::IsValidRange(ctxAddr, 96)) return -1;
	GWrite32(ctxAddr + CTX_MEM, GRead32(ctxAddr + CTX_EDRAM) + 8);
	VideocodecCtx &vctx = g_videocodecCtxs[ctxAddr];
	delete vctx.decoder;
	vctx.decoder = new media::H264Decoder();
	vctx.decoder->Open();
	vctx.frameCount = 0;
	vctx.type = type;
	return CallerWait(0, initUs);
}

int sceVideocodecGetEDRAM(u32 ctxAddr, int type) {
	(void)type;
	if (!mem::IsValidRange(ctxAddr, 96)) return -1;
	// The firmware refuses rather than replacing one it already handed out.
	if (GRead32(ctxAddr + CTX_EDRAM_RAW) != 0) return (int)SCE_MPEG_ERROR_AVC_INVALID_VALUE;
	// Rounded as the firmware does it: the OR really is an OR.
	const u32 size = (GRead32(ctxAddr + CTX_EDRAM_SIZE) + 63) | 0x3F;
	const u32 addr = MEAlloc(size);
	if (!addr) return (int)SCE_MPEG_ERROR_AVC_INVALID_VALUE;
	g_videocodecCtxs[ctxAddr].edram = addr;
	GWrite32(ctxAddr + CTX_EDRAM, addr);
	GWrite32(ctxAddr + CTX_EDRAM_RAW, addr);
	return MECall(0, getEdramUs);
}

int sceVideocodecReleaseEDRAM(u32 ctxAddr) {
	if (!mem::IsValidRange(ctxAddr, 96)) return -1;
	const u32 token = GRead32(ctxAddr + CTX_EDRAM_RAW);
	if (!token) return (int)SCE_MPEG_ERROR_AVC_INVALID_VALUE;
	auto it = g_videocodecCtxs.find(ctxAddr);
	if (it != g_videocodecCtxs.end() && it->second.edram) {
		MEFree(it->second.edram);
		it->second.edram = 0;
	}
	GWrite32(ctxAddr + CTX_EDRAM, 0);
	GWrite32(ctxAddr + CTX_EDRAM_RAW, 0);
	return MECall(0, releaseEdramUs);
}

int sceVideocodecDecode(u32 ctxAddr, int type) {
	if (!mem::IsValidRange(ctxAddr, 96)) return -1;
	if (type != 0 && type != 1) return -1;
	auto ctxIter = g_videocodecCtxs.find(ctxAddr);
	if (ctxIter == g_videocodecCtxs.end()) return -1;
	VideocodecCtx &vctx = ctxIter->second;
	if (!vctx.decoder) {
		vctx.decoder = new media::H264Decoder();
		vctx.decoder->Open();
	}

	const u32 auAddr = GRead32(ctxAddr + CTX_AU_DATA);
	const int auSize = (int)GRead32(ctxAddr + CTX_AU_SIZE);
	const u32 outAddr = GRead32(ctxAddr + CTX_OUT_INFO);
	GWrite32(ctxAddr + CTX_STATUS, 0);
	if (!mem::IsValidRange(outAddr, 108)) return -1;

	// The AU is normally in ME memory (sceMpegBasePESpacketCopy put it there).
	bool gotFrame = false;
	const u8 *au = auSize > 0 ? MEOrGuestPointer(auAddr, (u32)auSize) : nullptr;
	const int auBytes = au ? auSize : 0;
	if (au && auBytes > 0) {
		vctx.decoder->Decode(au, auBytes, (s64)vctx.frameCount * 3003);
		media::YuvFrame f;
		if (vctx.decoder->PopFrame(&f)) {
			vctx.frame = std::move(f);
			gotFrame = true;
		}
	}
	const int width = gotFrame ? vctx.frame.width : 0;
	const int height = gotFrame ? vctx.frame.height : 0;
	auto out32 = [outAddr](int offset, u32 value) { GWrite32(outAddr + offset, value); };

	// Only type 1 fills the descriptor in: for type 0 the YCbCr descriptor sits 0x40 bytes after
	// this one, and the type 1 fields would overwrite its buffer addresses.
	bool published = false;
	if (type == 0) {
		out32(8, (u32)width);
		out32(12, (u32)height);
		out32(28, 1);
	} else {
		out32(OUT_DATA, auAddr);
		out32(OUT_SIZE, (u32)auSize);
		out32(OUT_UNK12, 0x40);
		out32(OUT_CONSUMED, (u32)auSize);
		out32(OUT_WIDTH, (u32)width);
		out32(OUT_HEIGHT, (u32)height);
		out32(OUT_FRAME_READY, gotFrame ? 2 : 1);
		out32(OUT_UNK64, 1);
		out32(OUT_UNK72, (u32)-1);
		out32(OUT_TIMESTAMP, (u32)(vctx.frameCount * 0x64));
		out32(OUT_FPS, 2997);
	}
	if (gotFrame) {
		vctx.frameCount++;
		if (type == 0) {
			const u32 yuvStructAddr = GRead32(ctxAddr + CTX_YUV_STRUCT);
			if (mem::IsValidRange(yuvStructAddr, 8 * 4)) {
				u32 buffers[8];
				if (PublishFrameBuffers(vctx, yuvStructAddr, width, height, buffers)) {
					WriteTiled(buffers, vctx.frame, width, height);
					published = true;
				}
			}
		} else {
			out32(OUT_WIDTH_Y, (u32)width);
			out32(OUT_WIDTH_CR, (u32)(width / 2));
			out32(OUT_WIDTH_CB, (u32)(width / 2));
		}
	}
	if (type == 0) {
		out32(32, published ? 1 : 0);  // images decoded: mpeg.prx won't convert without this
		out32(36, published ? 0 : 1);
	}

	// About 3.4ms for a 480x272 frame on a PSP (video/mpeg/playertiming), also when the decoder
	// holds the picture back. Until a picture tells the size, assume full screen.
	int delayUs = 0;
	if (auBytes > 0) {
		const int w = vctx.decoder->Width() > 0 ? vctx.decoder->Width() : 480;
		const int h = vctx.decoder->Height() > 0 ? vctx.decoder->Height() : 272;
		delayUs = audiohle::MEScheduleJob((int)(3400LL * w * h / (480 * 272)));
	}
	return delayUs > 0 ? (int)audiohle::DelayResult(0, delayUs) : 0;
}

int sceVideocodecStop(u32 ctxAddr, int type) {
	(void)type;
	auto it = g_videocodecCtxs.find(ctxAddr);
	if (it != g_videocodecCtxs.end() && it->second.decoder) it->second.decoder->Flush();
	return MECall(0, stopUs);
}

int sceVideocodecDelete(u32 ctxAddr, int type) {
	(void)type;
	auto it = g_videocodecCtxs.find(ctxAddr);
	if (it != g_videocodecCtxs.end()) {
		FreeContext(it->second);
		g_videocodecCtxs.erase(it);
	}
	// Not instant: a game can rely on one of its threads running once more meanwhile (Jak and
	// Daxter's video_sound_thread).
	return CallerWait(0, deleteUs);
}

int sceVideocodecGetVersion(u32 ctxAddr, int type) {
	(void)type;
	if (!mem::IsValidRange(ctxAddr, 96)) return -1;
	// What a real PSP returns.
	GWrite32(ctxAddr + CTX_VERSION, 0x78);
	return MECall(0, getVersionUs);
}

int sceVideocodecUnimplemented(u32 ctxAddr, int type) {
	(void)ctxAddr;
	(void)type;
	LOGW("ME", "UNIMPL %s", hle::CurrentFunctionName());
	return 0;
}

int sceVideocodecSetMemory(u32 ctxAddr, int type) {
	(void)ctxAddr;
	(void)type;
	return 0;
}

// Hands a decoded frame back as three planes (sceMpegAvcCopyYCbCr in mpeg.prx). The descriptor:
// 0x00 width, 0x04 height, 0x0c the eight buffers ordered 0,2,4,6,1,3,5,7, 0x2c/0x30/0x34 the
// destination Y, Cb, Cr.
int sceVideocodecCopyYCbCr(u32 ctxAddr, int type) {
	(void)type;
	if (!mem::IsValidRange(ctxAddr, 0x38)) return -1;
	const int width = (int)GRead32(ctxAddr + 0x00);
	const int height = (int)GRead32(ctxAddr + 0x04);
	if (width <= 0 || height <= 0 || width > 1024 || height > 1024) return -1;
	static const int fromDescriptor[8] = {0, 2, 4, 6, 1, 3, 5, 7};
	u32 buffers[8]{};
	for (int i = 0; i < 8; i++) buffers[fromDescriptor[i]] = GRead32(ctxAddr + 0x0c + i * 4);

	int sizes[8];
	media::FrameBufferLayout(width, height, sizes, nullptr);
	const u8 *src[8]{};
	for (int i = 0; i < 8; i++) {
		if (sizes[i] > 0) {
			src[i] = MEOrGuestPointer(buffers[i], (u32)sizes[i]);
			if (!src[i]) return -1;
		}
	}
	const u32 planeSizes[3] = {(u32)(width * height), (u32)((width >> 1) * (height >> 1)),
	                           (u32)((width >> 1) * (height >> 1))};
	std::vector<u8> planes(planeSizes[0] + planeSizes[1] * 2);
	media::UntileYCbCr(planes.data(), planes.data() + planeSizes[0], planes.data() + planeSizes[0] + planeSizes[1], src,
	                   sizes, width, height);
	u32 off = 0;
	for (int i = 0; i < 3; i++) {
		const u32 dst = GRead32(ctxAddr + 0x2c + i * 4);
		u8 *d = mem::GetPointer(dst, planeSizes[i]);
		if (!d) return -1;
		memcpy(d, planes.data() + off, planeSizes[i]);
		off += planeSizes[i];
	}
	return 0;
}

}  // namespace

void EnsureInit() {
	static bool inited = false;
	if (inited) return;
	inited = true;
	media::SetGuestMemory(&GuestPtrAccessor);
}

u8 *MEGetPointerRange(u32 addr, u32 size) {
	if (g_meRam.size() != ME_MEM_SIZE || addr >= ME_MEM_SIZE || size > ME_MEM_SIZE - addr) return nullptr;
	return g_meRam.data() + addr;
}

const u8 *MEOrGuestPointer(u32 addr, u32 size) {
	if (const u8 *me = MEGetPointerRange(addr, size)) return me;
	return mem::GetPointer(addr, size);
}

u32 MEAllocate(u32 size) { return MEAlloc(size); }
void MERelease(u32 addr) { MEFree(addr); }

void Register_sceVideocodec() {
	static const hle::HleFunction funcs[] = {
	    HLEFN(0xC01EC829, sceVideocodecOpen),
	    HLEFN(0x2D31F5B1, sceVideocodecGetEDRAM),
	    HLEFN(0x17099F0A, sceVideocodecInit),
	    HLEFN(0xDBA273FA, sceVideocodecDecode),
	    HLEFN(0x4F160BF4, sceVideocodecReleaseEDRAM),
	    HLEFN(0x745A7B7A, sceVideocodecSetMemory),
	    hle::HleFunction{0x2F385E7F, &hle::Wrap<&sceVideocodecUnimplemented>, "sceVideocodecScanHeader", 0},
	    HLEFN(0x307E6E1C, sceVideocodecDelete),
	    HLEFN(0xA2F0564E, sceVideocodecStop),
	    hle::HleFunction{0x17CF7D2C, &hle::Wrap<&sceVideocodecUnimplemented>, "sceVideocodecGetFrameCrop", 0},
	    HLEFN(0x26927D19, sceVideocodecGetVersion),
	    hle::HleFunction{0x627B7D42, &hle::Wrap<&sceVideocodecUnimplemented>, "sceVideocodecGetSEI", 0},
	    hle::HleFunction{0x893B32B1, &hle::Wrap<&sceVideocodecUnimplemented>, "sceVideocodecSetMode", 0},
	    HLEFN(0xD95C24D5, sceVideocodecCopyYCbCr),
	};
	hle::RegisterModule("sceVideocodec", funcs);
}

}  // namespace mediahle

void Register_MediaLibraries() {
	using namespace mediahle;
	EnsureInit();
	Register_sceMpeg();
	Register_sceMpegbase();
	Register_scePsmf();
	Register_scePsmfPlayer();
	Register_sceVideocodec();
	Register_sceJpeg();
	Register_sceMp4();
}
