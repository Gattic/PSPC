// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceMpeg: MPEG (PSMF) stream playback through a game-managed ringbuffer.
// Ported from PPSSPP's Core/HLE/sceMpeg.cpp (https://github.com/hrydgard/ppsspp, Copyright (c)
// 2012- PPSSPP Project, GPL-2.0-or-later), itself partly based on JPCSP. The decoding is in
// pspc_media (media/media_engine.h).
//
// Differences from PPSSPP: the ringbuffer callback is called synchronously (cpu::CallGuest)
// rather than through a queued MIPS call, and, as on hardware (video/mpeg/basic), it is called
// again for the rest whenever it delivers fewer packets than asked for.
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

#include "common/log.h"
#include "media/media_engine.h"
#include "media/mpeg_demux.h"
#include "media/ycbcr.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/hle/modules/media_hle.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

using namespace kernel;
using media::MediaEngine;

namespace mediahle {

namespace {

// MPEG AVC elementary stream.
constexpr int MPEG_AVC_ES_SIZE = 2048;
// MPEG ATRAC elementary stream.
constexpr int MPEG_ATRAC_ES_SIZE = 2112;
constexpr int MPEG_ATRAC_ES_OUTPUT_SIZE = 8192;
// MPEG PCM elementary stream.
constexpr int MPEG_PCM_ES_SIZE = 320;
constexpr int MPEG_PCM_ES_OUTPUT_SIZE = 320;
// MPEG userdata elementary stream.
constexpr int MPEG_DATA_ES_SIZE = 0xA0000;
constexpr int MPEG_DATA_ES_OUTPUT_SIZE = 0xA0000;
// MPEG AVC resource management.
constexpr int MPEG_AVC_RESOURCE_FLAG = 0x4;

// MPEG analysis results.
constexpr int MPEG_VERSION_0012 = 0;
constexpr int MPEG_VERSION_0013 = 1;
constexpr int MPEG_VERSION_0014 = 2;
constexpr int MPEG_VERSION_0015 = 3;
// PSMF analysis results.
constexpr u32 PSMF_VERSION_0012 = 0x32313030;
constexpr u32 PSMF_VERSION_0013 = 0x33313030;
constexpr u32 PSMF_VERSION_0014 = 0x34313030;
constexpr u32 PSMF_VERSION_0015 = 0x35313030;

// MPEG streams.
constexpr int MPEG_AVC_STREAM = 0;
constexpr int MPEG_ATRAC_STREAM = 1;
constexpr int MPEG_PCM_STREAM = 2;
constexpr int MPEG_DATA_STREAM = 3;  // arbitrary user defined type
constexpr int MPEG_AUDIO_STREAM = 15;
constexpr int MPEG_AU_MODE_DECODE = 0;
constexpr int MPEG_AU_MODE_SKIP = 1;
constexpr u32 MPEG_MEMSIZE_0104 = 0x0b3DB;
constexpr u32 MPEG_MEMSIZE_0105 = 0x10000;  // 64k
constexpr int MPEG_AVC_DECODE_SUCCESS = 1;  // internal value

// Delays, in microseconds.
constexpr int atracDecodeDelayUs = 3000;
constexpr int avcFirstDelayUs = 3600;
constexpr int avcCscDelayUs = 4000;
constexpr int avcDecodeDelayUs = 5400;  // varies between 4700 and 6000
constexpr int avcEmptyDelayUs = 320;
constexpr int mpegDecodeErrorDelayUs = 100;
constexpr int videoTimestampStep = 3003;  // 90000 / 29.97
constexpr s64 UNKNOWN_TIMESTAMP = -1;

constexpr int GE_CMODE_16BIT_BGR5650 = 0;
constexpr int GE_CMODE_32BIT_ABGR8888 = 3;

constexpr int MPEG_DATA_ES_BUFFERS = 2;

struct SceMpegAu {
	s64 pts;  // presentation time stamp
	s64 dts;  // decode time stamp
	u32 esBuffer;  // we abuse this to keep track of the stream number
	u32 esSize;

	void read(u32 addr) {
		memset(this, 0, sizeof(*this));
		if (!mem::IsValidRange(addr, sizeof(*this))) return;
		memcpy(this, GPtr(addr), sizeof(*this));
		pts = (s64)(((u64)pts & 0xFFFFFFFFULL) << 32 | ((u64)pts >> 32));
		dts = (s64)(((u64)dts & 0xFFFFFFFFULL) << 32 | ((u64)dts >> 32));
	}
	void write(u32 addr) {
		SceMpegAu out = *this;
		out.pts = (s64)(((u64)pts & 0xFFFFFFFFULL) << 32 | ((u64)pts >> 32));
		out.dts = (s64)(((u64)dts & 0xFFFFFFFFULL) << 32 | ((u64)dts >> 32));
		if (mem::IsValidRange(addr, sizeof(out))) memcpy(GPtr(addr), &out, sizeof(out));
	}
};
static_assert(sizeof(SceMpegAu) == 24, "SceMpegAu layout");

// As in PSP RAM.
struct SceMpegRingBuffer {
	s32 packets;
	// Misused: this is used as total read, but should be the read offset (within the ring).
	s32 packetsRead;
	s32 packetsWritePos;
	s32 packetsAvail;  // pspsdk: unk2, noxa: iUnk0
	s32 packetSize;    // 2048
	u32 data;          // address of the ring
	u32 callback_addr;  // see sceMpegRingbufferPut
	s32 callback_args;
	s32 dataUpperBound;
	s32 semaID;
	u32 mpeg;  // pointer to the mpeg struct, fixed up in sceMpegCreate
	// Not in all versions.
	u32 gp;
};
static_assert(sizeof(SceMpegRingBuffer) == 48, "SceMpegRingBuffer layout");

struct AvcContext {
	int avcDetailFrameWidth = 0;
	int avcDetailFrameHeight = 0;
	int avcDecodeResult = 0;
	int avcFrameStatus = 0;
};

struct StreamInfo {
	int type = 0;
	int num = 0;
	int sid = 0;
	bool needsReset = false;
};

const u8 defaultMpegheader[2048] = {
    0x50, 0x53, 0x4d, 0x46, 0x30, 0x30, 0x31, 0x35, 0x00, 0x00, 0x08, 0x00, 0x00, 0x10, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x4e, 0x00, 0x00, 0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x0d, 0xbe, 0xca, 0x00, 0x00, 0x61, 0xa8, 0x00, 0x01, 0x5f,
    0x90, 0x02, 0x01, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x0d, 0xbe, 0xca, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x22, 0x00, 0x02, 0xe0, 0x00, 0x20, 0xfb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
    0x11, 0x00, 0x00, 0xbd, 0x00, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02};

// Memory Stick Video (sceMpegCreate modes 4 to 6, see sceMpegGetAvcNalAu below): the access unit
// sceMpegGetAvcNalAu built and the decoder it goes to. The pictures are left in Media Engine
// memory, a few frame buffer sets in turn, as the ME's decoded picture buffer would.
constexpr int kNalFrameSets = 4;
struct NalState {
	std::vector<u8> au;  // Annex B
	media::H264Decoder decoder;
	bool decoderFailed = false;
	u32 frames[kNalFrameSets] = {};
	int framesW = 0, framesH = 0;
	int nextSet = 0;
	int decoded = 0;

	NalState() = default;
	NalState(const NalState &) = delete;
	NalState &operator=(const NalState &) = delete;
	~NalState() {
		for (u32 f : frames)
			if (f) MERelease(f);
	}
};

struct MpegContext {
	MpegContext() { memcpy(mpegheader, defaultMpegheader, 2048); }

	u32 workArea = 0;    // the memory given to sceMpegCreate
	int createMode = 0;  // its mode argument
	std::unique_ptr<NalState> nal;

	u8 mpegheader[2048];
	u32 defaultFrameWidth = 0;
	int videoFrameCount = 0;
	int audioFrameCount = 0;
	bool endOfAudioReached = false;
	bool endOfVideoReached = false;
	int videoPixelMode = GE_CMODE_32BIT_ABGR8888;
	u32 mpegMagic = 0;
	int mpegVersion = -1;
	u32 mpegRawVersion = 0;
	u32 mpegOffset = 0;
	u32 mpegStreamSize = 0;
	s64 mpegFirstTimestamp = 0;
	s64 mpegLastTimestamp = 0;
	u32 mpegFirstDate = 0;
	u32 mpegLastDate = 0;
	u32 mpegRingbufferAddr = 0;
	int mpegwarmUp = 0;
	bool esBuffers[MPEG_DATA_ES_BUFFERS] = {};
	AvcContext avc;

	bool avcRegistered = false;
	bool atracRegistered = false;
	bool pcmRegistered = false;
	bool dataRegistered = false;

	bool ignoreAtrac = false;
	bool ignorePcm = false;
	bool ignoreAvc = false;

	bool isAnalyzed = false;

	std::map<u32, StreamInfo> streamMap;
	std::map<u32, bool> streamIgnoreMap;
	std::unique_ptr<MediaEngine> mediaengine;
	u32 handle = 0;
	int registeredStreams = 0;
};

bool g_isMpegInit = false;
int g_mpegLibVersion = 0x010A;
u32 g_streamIdGen = 1;
std::map<u32, MpegContext *> g_mpegCtxs;

// MPEG AVC resource management.
u32 g_avcResourceAddr = 0;
u32 g_avcResourceDataAddr = 0;
int g_avcResourceFlags = 0;

// For PMP media (not supported: it needs a decoder fed from sceMpegBasePESpacketCopy's lists).
u32 g_pmpVideoSource = 0;
int g_pmpNBlocks = 0;

u32 R32(u32 addr) { return mem::IsValidRange(addr, 4) ? GRead32(addr) : 0; }
void W32(u32 addr, u32 v) {
	if (mem::IsValidRange(addr, 4)) GWrite32(addr, v);
}
bool IsValid4Aligned(u32 addr) { return (addr & 3) == 0 && mem::IsValidRange(addr, 4); }

SceMpegRingBuffer *RingPtr(u32 addr) {
	return mem::IsValidRange(addr, sizeof(SceMpegRingBuffer)) ? (SceMpegRingBuffer *)GPtr(addr) : nullptr;
}

u32 Delay(u32 result, int us) { return audiohle::DelayResult(result, us); }
// Work the Media Engine does: it queues behind audio decoding (audio_hle.h).
u32 MEDelay(u32 result, int us) { return audiohle::DelayResult(result, audiohle::MEScheduleJob(us)); }

MpegContext *getMpegCtx(u32 mpegAddr) {
	if (!mem::IsValidRange(mpegAddr, 4)) return nullptr;
	const u32 mpeg = GRead32(mpegAddr);
	auto found = g_mpegCtxs.find(mpeg);
	if (found == g_mpegCtxs.end()) return nullptr;
	return found->second;
}

u32 convertTimestampToDate(u32 ts) { return ts; }

int getMpegVersion(u32 mpegRawVersion) {
	switch (mpegRawVersion) {
	case PSMF_VERSION_0012: return MPEG_VERSION_0012;
	case PSMF_VERSION_0013: return MPEG_VERSION_0013;
	case PSMF_VERSION_0014: return MPEG_VERSION_0014;
	case PSMF_VERSION_0015: return MPEG_VERSION_0015;
	default: return -1;
	}
}

u32 LE32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

void AnalyzeMpeg(const u8 *buffer, u32 validSize, MpegContext *ctx) {
	if (validSize < 0x90) {
		ctx->mpegMagic = validSize >= 4 ? LE32(buffer) : 0;
		return;
	}
	ctx->mpegMagic = LE32(buffer);
	ctx->mpegRawVersion = LE32(buffer + media::PSMF_STREAM_VERSION_OFFSET);
	ctx->mpegVersion = getMpegVersion(ctx->mpegRawVersion);
	ctx->mpegOffset = media::ReadBE32(buffer + media::PSMF_STREAM_OFFSET_OFFSET);
	ctx->mpegStreamSize = media::ReadBE32(buffer + media::PSMF_STREAM_SIZE_OFFSET);
	ctx->mpegFirstTimestamp = media::GetMpegTimeStamp(buffer + media::PSMF_FIRST_TIMESTAMP_OFFSET);
	ctx->mpegLastTimestamp = media::GetMpegTimeStamp(buffer + media::PSMF_LAST_TIMESTAMP_OFFSET);
	ctx->mpegFirstDate = convertTimestampToDate((u32)ctx->mpegFirstTimestamp);
	ctx->mpegLastDate = convertTimestampToDate((u32)ctx->mpegLastTimestamp);
	ctx->mpegwarmUp = 0;
	ctx->avc.avcDetailFrameWidth = buffer[142] * 0x10;
	ctx->avc.avcDetailFrameHeight = buffer[143] * 0x10;
	ctx->avc.avcDecodeResult = MPEG_AVC_DECODE_SUCCESS;
	ctx->avc.avcFrameStatus = 0;
	ctx->videoFrameCount = 0;
	ctx->audioFrameCount = 0;
	ctx->endOfAudioReached = false;
	ctx->endOfVideoReached = false;

	if (ctx->mpegMagic != media::PSMF_MAGIC || ctx->mpegVersion < 0 || (ctx->mpegOffset & 2047) != 0 ||
	    ctx->mpegOffset == 0) {
		// The header is invalid.
		return;
	}

	if (!ctx->isAnalyzed && ctx->mediaengine && ctx->mpegStreamSize > 0 && validSize >= ctx->mpegOffset) {
		SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
		if (ring) {
			ctx->mediaengine->loadStream(buffer, (int)ctx->mpegOffset, ring->packets * ring->packetSize);
		} else {
			ctx->mediaengine->loadStream(buffer, (int)ctx->mpegOffset, 0);
		}
		// With scePsmf, some games use sceMpegQueryStreamOffset and sceMpegQueryStreamSize, which
		// would overwrite packets. Mark it analyzed to filter this; restored in sceMpegFinish.
		ctx->isAnalyzed = true;
	}

	// Copy the header struct to the mpeg header.
	memcpy(ctx->mpegheader, buffer, validSize >= 2048 ? 2048 : validSize);
	// Stored as big endian 0x800: the header copy is exactly 2048 bytes.
	ctx->mpegheader[8] = 0x00;
	ctx->mpegheader[9] = 0x00;
	ctx->mpegheader[10] = 0x08;
	ctx->mpegheader[11] = 0x00;
}

u32 sceMpegInit() {
	audiohle::EnsureInit();
	EnsureInit();
	if (g_isMpegInit) LOGD("MPEG", "sceMpegInit(): already initialized");
	g_isMpegInit = true;
	return Delay(0, 750);
}

u32 MpegRingbufferQueryMemSize(int packets) { return (u32)packets * (104 + 2048); }

u32 sceMpegRingbufferQueryMemSize(int packets) { return MpegRingbufferQueryMemSize(packets); }

u32 sceMpegRingbufferConstruct(u32 ringbufferAddr, u32 numPackets, u32 data, u32 size, u32 callbackAddr,
                               u32 callbackArg) {
	SceMpegRingBuffer *ring = RingPtr(ringbufferAddr);
	if (!ring) return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	if ((int)size < 0) return SCE_MPEG_ERROR_NO_MEMORY;
	if (MpegRingbufferQueryMemSize((int)numPackets) > size) {
		// The firmware allows some cases here, due to a bug in its validation.
		if (numPackets < 0x00100000) return SCE_MPEG_ERROR_NO_MEMORY;
	}
	ring->packets = (s32)numPackets;
	ring->packetsRead = 0;
	ring->packetsWritePos = 0;
	ring->packetsAvail = 0;
	ring->packetSize = 2048;
	ring->data = data;
	ring->callback_addr = callbackAddr;
	ring->callback_args = (s32)callbackArg;
	ring->dataUpperBound = (s32)(data + numPackets * 2048);
	ring->mpeg = 0;
	// This isn't in ver 0104, but it is in 0105.
	if (g_mpegLibVersion >= 0x0105 && sched::Current()) ring->gp = sched::Current()->gpReg;
	return 0;
}

u32 MpegRequiredMem() { return g_mpegLibVersion < 0x0105 ? MPEG_MEMSIZE_0104 : MPEG_MEMSIZE_0105; }

u32 sceMpegCreate(u32 mpegAddr, u32 dataPtr, u32 size, u32 ringbufferAddr, u32 frameWidth, u32 mode, u32 ddrTop) {
	(void)ddrTop;
	EnsureInit();
	// pspautotests intr/delays.
	if (sched::InInterrupt()) return SCE_MPEG_ERROR_IN_INTERRUPT;
	if (!mem::IsValidAddress(mpegAddr)) return (u32)-1;
	if (size < MpegRequiredMem()) return SCE_MPEG_ERROR_NO_MEMORY;

	SceMpegRingBuffer *ring = RingPtr(ringbufferAddr);
	if (ring) {
		if (ring->packetSize == 0) {
			ring->packetsAvail = 0;
		} else {
			ring->packetsAvail = ring->packets - (ring->dataUpperBound - (s32)ring->data) / ring->packetSize;
		}
		ring->mpeg = mpegAddr;
	}

	// Generate, and write the mpeg handle into the mpeg data, for some reason.
	const u32 mpegHandle = dataPtr + 0x30;
	W32(mpegAddr, mpegHandle);

	// Initialize the fake mpeg struct.
	if (mem::IsValidRange(mpegHandle, 24)) {
		memcpy(GPtr(mpegHandle), "LIBMPEG\0", 8);
		memcpy(GPtr(mpegHandle + 8), "001\0", 4);
		GWrite32(mpegHandle + 12, (u32)-1);
		if (ring) {
			GWrite32(mpegHandle + 16, ringbufferAddr);
			GWrite32(mpegHandle + 20, (u32)ring->dataUpperBound);
		}
	}
	auto old = g_mpegCtxs.find(mpegHandle);
	if (old != g_mpegCtxs.end()) {
		LOGW("MPEG", "Replacing existing mpeg context at %08x", mpegAddr);
		delete old->second;
		g_mpegCtxs.erase(old);
	}
	MpegContext *ctx = new MpegContext();
	g_mpegCtxs[mpegHandle] = ctx;

	ctx->mpegRingbufferAddr = ringbufferAddr;
	ctx->handle = mpegHandle;
	ctx->workArea = dataPtr;
	ctx->createMode = (int)mode;
	ctx->videoPixelMode = GE_CMODE_32BIT_ABGR8888;
	ctx->defaultFrameWidth = frameWidth;
	// Detailed "analysis" is done later in Query* for some reason.
	ctx->isAnalyzed = false;
	ctx->mediaengine = std::make_unique<MediaEngine>();

	return Delay(0, 29000);
}

int sceMpegDelete(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	g_mpegCtxs.erase(GRead32(mpeg));
	delete ctx;
	return (int)Delay(0, 40000);
}

int sceMpegAvcDecodeMode(u32 mpeg, u32 modeAddr) {
	if (!mem::IsValidRange(modeAddr, 8)) return -1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	const int pixelMode = (int)GRead32(modeAddr + 4);
	if (pixelMode >= GE_CMODE_16BIT_BGR5650 && pixelMode <= GE_CMODE_32BIT_ABGR8888) {
		ctx->videoPixelMode = pixelMode;
	} else {
		LOGW("MPEG", "sceMpegAvcDecodeMode(%i): unknown pixelMode", pixelMode);
	}
	return 0;
}

int sceMpegQueryStreamOffset(u32 mpeg, u32 bufferAddr, u32 offsetAddr) {
	if (!mem::IsValidAddress(bufferAddr) || !mem::IsValidAddress(offsetAddr)) return -1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;

	// Kinda destructive: this loads the stream, sceMpegQueryStreamSize doesn't.
	AnalyzeMpeg(GPtr(bufferAddr), audiohle::ClampValidSize(bufferAddr, 32768), ctx);

	if (ctx->mpegMagic != media::PSMF_MAGIC) {
		W32(offsetAddr, 0);
		return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	} else if (ctx->mpegVersion < 0) {
		W32(offsetAddr, 0);
		return (int)SCE_MPEG_ERROR_BAD_VERSION;
	} else if ((ctx->mpegOffset & 2047) != 0 || ctx->mpegOffset == 0) {
		W32(offsetAddr, 0);
		return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	}
	W32(offsetAddr, ctx->mpegOffset);
	return 0;
}

u32 sceMpegQueryStreamSize(u32 bufferAddr, u32 sizeAddr) {
	if (!mem::IsValidAddress(bufferAddr) || !mem::IsValidAddress(sizeAddr)) return (u32)-1;
	MpegContext ctx;
	ctx.isAnalyzed = false;  // no media engine: the stream isn't loaded
	AnalyzeMpeg(GPtr(bufferAddr), audiohle::ClampValidSize(bufferAddr, 32768), &ctx);
	if (ctx.mpegMagic != media::PSMF_MAGIC) {
		W32(sizeAddr, 0);
		return SCE_MPEG_ERROR_INVALID_VALUE;
	} else if ((ctx.mpegOffset & 2047) != 0) {
		W32(sizeAddr, 0);
		return SCE_MPEG_ERROR_INVALID_VALUE;
	}
	W32(sizeAddr, ctx.mpegStreamSize);
	return 0;
}

int sceMpegRegistStream(u32 mpeg, u32 streamType, u32 streamNum) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	switch (streamType) {
	case MPEG_AVC_STREAM:
		ctx->avcRegistered = true;
		ctx->mediaengine->addVideoStream((int)streamNum);
		ctx->mediaengine->setVideoStream((int)streamNum);
		break;
	case MPEG_AUDIO_STREAM:
	case MPEG_ATRAC_STREAM:
		ctx->atracRegistered = true;
		ctx->mediaengine->setAudioStream((int)streamNum);
		break;
	case MPEG_PCM_STREAM: ctx->pcmRegistered = true; break;
	case MPEG_DATA_STREAM: ctx->dataRegistered = true; break;
	default: LOGD("MPEG", "sceMpegRegistStream(%i): unknown stream type", streamType); break;
	}
	// On hardware the id is the address of the stream's slot in the mpeg data, 0x440 bytes each
	// from 0x12C0 past the handle (video/mpeg/basic).
	u32 sid = ctx->handle + 0x12C0 + 0x440 * (u32)ctx->registeredStreams++;
	if (!ctx->handle || ctx->streamMap.count(sid)) sid = g_streamIdGen++;
	StreamInfo info;
	info.type = (int)streamType;
	info.num = (int)streamNum;
	info.sid = (int)sid;
	info.needsReset = true;
	ctx->streamMap[sid] = info;
	ctx->streamIgnoreMap[sid] = false;
	return (int)sid;
}

int sceMpegMallocAvcEsBuf(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	// Doesn't actually malloc, just keeps track of a couple of flags.
	for (int i = 0; i < MPEG_DATA_ES_BUFFERS; i++) {
		if (!ctx->esBuffers[i]) {
			ctx->esBuffers[i] = true;
			return i + 1;
		}
	}
	return 0;
}

int sceMpegFreeAvcEsBuf(u32 mpeg, int esBuf) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	if (esBuf == 0) return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	if (esBuf >= 1 && esBuf <= MPEG_DATA_ES_BUFFERS) ctx->esBuffers[esBuf - 1] = false;
	return 0;
}

void UpdateAvail(MpegContext *ctx, SceMpegRingBuffer *ring, s32 beforeAvail) {
	const s32 afterAvail = ring->packets - ctx->mediaengine->getRemainSize() / 2048;
	// Don't actually reset avail, only change it by what was decoded. Garbage frames can make
	// this wrong, but some games expect it.
	if (g_mpegLibVersion <= 0x0103) {
		ring->packetsAvail += afterAvail - beforeAvail;
	} else {
		ring->packetsAvail = afterAvail;
	}
}

// ---- Memory Stick Video: AVC out of MP4 files ----
//
// msvideo_main_plugin demuxes MP4 files itself (sceMpegCreate mode 4 or 5) and hands the decoder
// one access unit at a time as length-prefixed NAL units, with the SPS and PPS from the avcC box
// (sceMpegGetAvcNalAu). The picture comes back through the structure sceMpegAvcDecodeDetail2
// points at: msvideo copies it out of the ME's frame buffers with sceMpegBaseYCrCbCopy and
// converts it with sceMpegBaseCscAvc. Layouts as mpeg_vsh.prx 4.01 builds them and
// msvideo_main_plugin reads them.

bool IsNalMode(const MpegContext *ctx) { return ctx->createMode >= 4 && ctx->createMode <= 6; }

NalState &Nal(MpegContext *ctx) {
	if (!ctx->nal) ctx->nal = std::make_unique<NalState>();
	return *ctx->nal;
}

// What sceMpegGetAvcNalAu reads.
struct SceMpegAvcNal {
	u32 sps, spsSize;
	u32 pps, ppsSize;
	u32 prefixSize;  // 1, 2 or 4: the big-endian length in front of every NAL unit
	u32 nal, nalSize;
	u32 mode;        // bit 0: put the SPS in front, bit 1: the PPS
};
static_assert(sizeof(SceMpegAvcNal) == 32, "SceMpegAvcNal layout");

// The structure sceMpegAvcDecodeDetail2 hands out, kept in the work area given to sceMpegCreate.
constexpr u32 kNalDetailOffset = 0x1000;
enum : u32 {
	NAL_DETAIL = 0x00,   // +0x10 picture info, +0x2c frame buffers, +0x30 VUI (pointers to the below)
	NAL_INFO = 0x40,     // as sceMpegAvcDecodeDetail: result, count, width, height (pixels), crop
	                     // left/right/top/bottom, pictures this decode
	NAL_BUFFERS = 0x80,  // the eight frame buffers (as SceMp4AvcCscStruct's), pts, two SEI pointers
	NAL_VUI = 0xC0,      // [0] aspect_ratio_info_present, [1] aspect_ratio_idc, +4/+8 SAR, ...
	NAL_AREA_SIZE = 0x124,
};

u32 NalDetailAddr(const MpegContext *ctx) { return ctx->workArea + kNalDetailOffset; }

void NalWriteDetail(MpegContext *ctx, const u32 *buffers, int width, int height, int cropRight, int cropBottom,
                    int pictures, s64 pts) {
	const u32 base = NalDetailAddr(ctx);
	u8 *p = mem::GetPointer(base, NAL_AREA_SIZE);
	if (!p) return;
	memset(p + NAL_DETAIL, 0, NAL_INFO - NAL_DETAIL);
	GWrite32(base + NAL_DETAIL + 0x10, base + NAL_INFO);
	GWrite32(base + NAL_DETAIL + 0x2c, base + NAL_BUFFERS);
	GWrite32(base + NAL_DETAIL + 0x30, base + NAL_VUI);
	GWrite32(base + NAL_INFO + 0x00, MPEG_AVC_DECODE_SUCCESS);
	GWrite32(base + NAL_INFO + 0x04, (u32)ctx->videoFrameCount);
	GWrite32(base + NAL_INFO + 0x20, (u32)pictures);
	GWrite32(base + NAL_INFO + 0x24, 0);
	if (!buffers) return;  // no new picture: the last one stays described
	GWrite32(base + NAL_INFO + 0x08, (u32)width);
	GWrite32(base + NAL_INFO + 0x0c, (u32)height);
	GWrite32(base + NAL_INFO + 0x10, 0);
	GWrite32(base + NAL_INFO + 0x14, (u32)cropRight);
	GWrite32(base + NAL_INFO + 0x18, 0);
	GWrite32(base + NAL_INFO + 0x1c, (u32)cropBottom);
	for (int i = 0; i < 8; i++) GWrite32(base + NAL_BUFFERS + i * 4, buffers[i]);
	GWrite32(base + NAL_BUFFERS + 0x20, (u32)pts);
	GWrite32(base + NAL_BUFFERS + 0x24, 0);
	GWrite32(base + NAL_BUFFERS + 0x28, 0);
	memset(p + NAL_VUI, 0, NAL_AREA_SIZE - NAL_VUI);  // no aspect ratio information: square pixels
}

// Puts a decoded picture into the next frame buffer set and describes it. False if there is no
// room in ME memory.
bool NalPublish(MpegContext *ctx, const media::YuvFrame &f) {
	NalState &st = Nal(ctx);
	const int w = (f.width + 15) & ~15, h = (f.height + 15) & ~15;
	u32 offsets[8];
	int sizes[8];
	const u32 total = media::FrameBufferLayout(w, h, sizes, offsets);
	if (total == 0) return false;
	if (w != st.framesW || h != st.framesH) {
		for (u32 &set : st.frames) {
			if (set) MERelease(set);
			set = 0;
		}
		st.framesW = w;
		st.framesH = h;
	}
	u32 &set = st.frames[st.nextSet];
	st.nextSet = (st.nextSet + 1) % kNalFrameSets;
	if (!set) set = MEAllocate(total);
	if (!set) {
		LOGE("MPEG", "No room in ME memory for a %dx%d picture", w, h);
		return false;
	}
	u32 buffers[8];
	u8 *dst[8];
	for (int i = 0; i < 8; i++) {
		buffers[i] = set + offsets[i];
		dst[i] = sizes[i] > 0 ? MEGetPointerRange(buffers[i], (u32)sizes[i]) : nullptr;
	}
	if (w == f.width && h == f.height) {
		media::TileYCbCr(dst, sizes, f.y.data(), f.cb.data(), f.cr.data(), f.width, f.width / 2, w, h);
	} else {
		// Pad to whole macroblocks by repeating the last row and column, as the cropped-off part.
		std::vector<u8> y((size_t)w * h), cb((size_t)(w / 2) * (h / 2)), cr(cb.size());
		for (int row = 0; row < h; row++) {
			const u8 *src = f.y.data() + (size_t)std::min(row, f.height - 1) * f.width;
			u8 *out = y.data() + (size_t)row * w;
			memcpy(out, src, f.width);
			memset(out + f.width, src[f.width - 1], w - f.width);
		}
		const int cw = f.width / 2, ch = f.height / 2;
		for (int row = 0; row < h / 2; row++) {
			const size_t from = (size_t)std::min(row, ch - 1) * cw;
			u8 *ob = cb.data() + (size_t)row * (w / 2), *orr = cr.data() + (size_t)row * (w / 2);
			memcpy(ob, f.cb.data() + from, cw);
			memcpy(orr, f.cr.data() + from, cw);
			memset(ob + cw, f.cb[from + cw - 1], w / 2 - cw);
			memset(orr + cw, f.cr[from + cw - 1], w / 2 - cw);
		}
		media::TileYCbCr(dst, sizes, y.data(), cb.data(), cr.data(), w, w / 2, w, h);
	}
	NalWriteDetail(ctx, buffers, w, h, w - f.width, h - f.height, 1, f.pts);
	return true;
}

// The next picture the decoder has ready, if any, published. Returns the frame status.
int NalTakePicture(MpegContext *ctx) {
	NalState &st = Nal(ctx);
	media::YuvFrame f;
	int status = 0;
	if (st.decoder.IsOpen() && st.decoder.PopFrame(&f) && f.Valid() && NalPublish(ctx, f)) status = 1;
	if (status) {
		ctx->videoFrameCount++;
	} else {
		NalWriteDetail(ctx, nullptr, 0, 0, 0, 0, 0, 0);
	}
	ctx->avc.avcFrameStatus = status;
	ctx->avc.avcDecodeResult = MPEG_AVC_DECODE_SUCCESS;
	return status;
}

u32 sceMpegGetAvcNalAu(u32 mpeg, u32 nalAddr, u32 auAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	if (!mem::IsValidRange(nalAddr, sizeof(SceMpegAvcNal)) || !mem::IsValidRange(auAddr, sizeof(SceMpegAu)))
		return SCE_MPEG_ERROR_INVALID_VALUE;
	SceMpegAvcNal n;
	memcpy(&n, GPtr(nalAddr), sizeof(n));
	GWrite32(auAddr + 0x14, 0);  // esSize
	if (n.prefixSize == 0 || (n.prefixSize > 2 && n.prefixSize != 4)) return SCE_MPEG_ERROR_INVALID_VALUE;
	NalState &st = Nal(ctx);
	st.au.clear();
	static const u8 kStartCode[4] = {0, 0, 0, 1};
	auto put = [&](const u8 *p, u32 n) { st.au.insert(st.au.end(), p, p + n); };
	bool started = false;
	if ((n.mode & 1) && n.sps) {
		const u8 *sps = mem::GetPointer(n.sps, n.spsSize);
		if (!sps) return SCE_MPEG_ERROR_INVALID_VALUE;
		put(kStartCode, 4);
		put(sps, n.spsSize);
		started = true;
	}
	if ((n.mode & 2) && n.pps) {
		const u8 *pps = mem::GetPointer(n.pps, n.ppsSize);
		if (!pps) return SCE_MPEG_ERROR_INVALID_VALUE;
		put(kStartCode, 4);
		put(pps, n.ppsSize);
		started = true;
	}
	int left = (int)n.nalSize;
	const u8 *p = left > 0 ? mem::GetPointer(n.nal, n.nalSize) : nullptr;
	if (left > 0 && !p) return SCE_MPEG_ERROR_INVALID_VALUE;
	while (left > 0) {
		u32 len = 0;
		for (u32 i = 0; i < n.prefixSize; i++) len = len << 8 | p[i];
		left -= (int)(n.prefixSize + len);
		if (left < 0 || len == 0) {
			st.au.clear();
			return 0x80618007;
		}
		const u8 *unit = p + n.prefixSize;
		// A four byte start code at the front and before parameter sets, three elsewhere.
		const int skip = started && (unsigned)((unit[0] & 0x1f) - 7) >= 2 ? 1 : 0;
		put(kStartCode + skip, 4 - skip);
		put(unit, len);
		started = true;
		p = unit + len;
	}
	GWrite32(auAddr + 0x14, (u32)st.au.size());
	return 0;
}

u32 NalDecode(MpegContext *ctx, u32 initAddr) {
	NalState &st = Nal(ctx);
	if (!st.au.empty() && !st.decoderFailed) {
		if (!st.decoder.IsOpen() && !st.decoder.Open()) {
			LOGE("MPEG", "No H.264 decoder for Memory Stick Video");
			st.decoderFailed = true;
		} else {
			// msvideo leaves the AU's timestamps alone: number the access units instead.
			st.decoder.Decode(st.au.data(), (int)st.au.size(), (s64)st.decoded * videoTimestampStep);
			st.decoded++;
		}
	}
	st.au.clear();
	const int status = NalTakePicture(ctx);
	W32(initAddr, (u32)status);
	return MEDelay(0, st.decoded <= 1 ? avcFirstDelayUs : avcDecodeDelayUs);
}

int sceMpegAvcDecodeDetail2(u32 mpeg, u32 detailPtrAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	if (!IsValid4Aligned(detailPtrAddr)) return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	if (!mem::IsValidRange(NalDetailAddr(ctx), NAL_AREA_SIZE)) return -1;
	if (!IsNalMode(ctx)) LOGW("MPEG", "sceMpegAvcDecodeDetail2 outside MP4 mode (%d): picture not described", ctx->createMode);
	if (GRead32(NalDetailAddr(ctx) + NAL_DETAIL + 0x10) == 0) NalWriteDetail(ctx, nullptr, 0, 0, 0, 0, 0, 0);
	GWrite32(detailPtrAddr, NalDetailAddr(ctx));
	return 0;
}

// One picture of the last decode: result, pts, width, height, crop left/right/top/bottom, pictures.
int sceMpegAvcDecodeDetailIndex(u32 mpeg, int index, u32 outAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	const u32 base = NalDetailAddr(ctx);
	if (!mem::IsValidRange(base, NAL_AREA_SIZE) || !mem::IsValidRange(outAddr, 9 * 4)) return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	if (GRead32(base + NAL_DETAIL + 0x10) == 0) NalWriteDetail(ctx, nullptr, 0, 0, 0, 0, 0, 0);
	if (index < 0 || index >= (int)GRead32(base + NAL_INFO + 0x20)) return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	GWrite32(outAddr, GRead32(base + NAL_DETAIL + 8));
	GWrite32(outAddr + 4, GRead32(base + NAL_BUFFERS + 0x20));
	for (int i = 0; i < 7; i++) GWrite32(outAddr + 8 + i * 4, GRead32(base + NAL_INFO + 8 + i * 4));
	return 0;
}

// Which of the described pictures the last decode produced (mpeg_vsh takes it from its state for
// modes 3 to 6); always the first here.
int sceMpeg_6F314410(u32 mpeg, u32 outAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	if (ctx->createMode < 3 || ctx->createMode > 6) return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	if (!IsValid4Aligned(outAddr)) return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	GWrite32(outAddr, 0);
	return 0;
}

u32 sceMpegAvcDecode(u32 mpeg, u32 auAddr, u32 frameWidth, u32 bufferAddr, u32 initAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	if (IsNalMode(ctx)) return NalDecode(ctx, initAddr);
	if (frameWidth == 0) {  // Go Sudoku passes 0xcccccccc
		frameWidth = ctx->defaultFrameWidth ? ctx->defaultFrameWidth : (u32)ctx->avc.avcDetailFrameWidth;
	}
	SceMpegAu avcAu;
	avcAu.read(auAddr);

	SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
	if (!ring) return (u32)-1;
	if (!mem::IsValidRange(bufferAddr, 4) || !mem::IsValidRange(initAddr, 4)) return (u32)-1;
	const u32 buffer = GRead32(bufferAddr);

	if (ring->packetsRead == 0 || ctx->mediaengine->IsVideoEnd()) {
		return Delay(SCE_MPEG_ERROR_AVC_DECODE_FATAL, avcEmptyDelayUs);
	}

	const s32 beforeAvail = ring->packets - ctx->mediaengine->getRemainSize() / 2048;
	// We stored the video stream number here in sceMpegGetAvcAu().
	ctx->mediaengine->setVideoStream((int)avcAu.esBuffer);

	if (ctx->mediaengine->stepVideo(ctx->videoPixelMode)) {
		ctx->mediaengine->writeVideoImage(buffer, (int)frameWidth, ctx->videoPixelMode);
		ctx->avc.avcFrameStatus = 1;
		ctx->videoFrameCount++;
	} else {
		ctx->avc.avcFrameStatus = 0;
	}
	UpdateAvail(ctx, ring, beforeAvail);

	avcAu.pts = ctx->mediaengine->getVideoTimeStamp() + ctx->mpegFirstTimestamp;
	avcAu.write(auAddr);

	if (g_mpegLibVersion >= 0x0105 && g_mpegLibVersion < 0x010a) {
		// Killzone - Liberation expects this (PPSSPP issue #16727).
		W32(initAddr, 1);
	} else {
		W32(initAddr, (u32)ctx->avc.avcFrameStatus);
	}
	ctx->avc.avcDecodeResult = MPEG_AVC_DECODE_SUCCESS;
	return MEDelay(0, ctx->videoFrameCount <= 1 ? avcFirstDelayUs : avcDecodeDelayUs);
}

u32 sceMpegAvcDecodeStop(u32 mpeg, u32 frameWidth, u32 bufferAddr, u32 statusAddr) {
	(void)frameWidth;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (ctx && IsNalMode(ctx) && IsValid4Aligned(statusAddr)) {
		// The pictures held back for reordering, one per call.
		NalState &st = Nal(ctx);
		if (st.decoder.IsOpen() && st.decoder.PendingFrames() == 0) st.decoder.Drain();
		W32(statusAddr, (u32)NalTakePicture(ctx));
		return 0;
	}
	if (!mem::IsValidAddress(bufferAddr) || !mem::IsValidAddress(statusAddr)) return (u32)-1;
	if (!ctx) return (u32)-1;
	// No last frame generated.
	W32(statusAddr, 0);
	return 0;
}

u32 sceMpegUnRegistStream(u32 mpeg, int streamUid) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	auto it = ctx->streamMap.find((u32)streamUid);
	const int type = it != ctx->streamMap.end() ? it->second.type : -1;
	switch (type) {
	case MPEG_AVC_STREAM: ctx->avcRegistered = false; break;
	case MPEG_AUDIO_STREAM:
	case MPEG_ATRAC_STREAM: ctx->atracRegistered = false; break;
	case MPEG_PCM_STREAM: ctx->pcmRegistered = false; break;
	case MPEG_DATA_STREAM: ctx->dataRegistered = false; break;
	default: break;
	}
	StreamInfo info;
	info.type = -1;
	info.sid = -1;
	info.needsReset = true;
	ctx->streamMap[(u32)streamUid] = info;
	ctx->isAnalyzed = false;
	return 0;
}

int sceMpegAvcDecodeDetail(u32 mpeg, u32 detailAddr) {
	if (!mem::IsValidRange(detailAddr, 36)) return -1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	GWrite32(detailAddr + 0, (u32)ctx->avc.avcDecodeResult);
	GWrite32(detailAddr + 4, (u32)ctx->videoFrameCount);
	GWrite32(detailAddr + 8, (u32)ctx->avc.avcDetailFrameWidth);
	GWrite32(detailAddr + 12, (u32)ctx->avc.avcDetailFrameHeight);
	GWrite32(detailAddr + 16, 0);
	GWrite32(detailAddr + 20, 0);
	GWrite32(detailAddr + 24, 0);
	GWrite32(detailAddr + 28, 0);
	GWrite32(detailAddr + 32, (u32)ctx->avc.avcFrameStatus);
	return 0;
}

u32 sceMpegAvcDecodeStopYCbCr(u32 mpeg, u32 bufferAddr, u32 statusAddr) {
	if (!mem::IsValidAddress(bufferAddr) || !IsValid4Aligned(statusAddr)) return (u32)-1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	GWrite32(statusAddr, 0);
	return 0;
}

int sceMpegAvcDecodeYCbCr(u32 mpeg, u32 auAddr, u32 bufferAddr, u32 initAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	SceMpegAu avcAu;
	avcAu.read(auAddr);

	SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
	if (!ring) return -1;
	if (ring->packetsRead == 0 || ctx->mediaengine->IsVideoEnd()) {
		return (int)Delay(SCE_MPEG_ERROR_AVC_DECODE_FATAL, avcEmptyDelayUs);
	}

	const s32 beforeAvail = ring->packets - ctx->mediaengine->getRemainSize() / 2048;
	ctx->mediaengine->setVideoStream((int)avcAu.esBuffer);

	if (!mem::IsValidRange(bufferAddr, 4) || !mem::IsValidRange(initAddr, 4)) return -1;

	if (ctx->mediaengine->stepVideo(ctx->videoPixelMode)) {
		// Don't draw here, sceMpegAvcCsc does.
		ctx->avc.avcFrameStatus = 1;
		ctx->videoFrameCount++;
	} else {
		ctx->avc.avcFrameStatus = 0;
	}
	UpdateAvail(ctx, ring, beforeAvail);

	avcAu.pts = ctx->mediaengine->getVideoTimeStamp() + ctx->mpegFirstTimestamp;
	avcAu.write(auAddr);

	if (g_mpegLibVersion >= 0x010A) {
		// Sunday Vs Magazine Shuuketsu! Choujou Daikessen expects this (PPSSPP issue #11060).
		GWrite32(initAddr, 1);
	} else {
		GWrite32(initAddr, (u32)ctx->avc.avcFrameStatus);
	}
	ctx->avc.avcDecodeResult = MPEG_AVC_DECODE_SUCCESS;
	return (int)MEDelay(0, ctx->videoFrameCount <= 1 ? avcFirstDelayUs : avcDecodeDelayUs);
}

u32 sceMpegAvcDecodeFlush(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	if (ctx->nal && ctx->nal->decoder.IsOpen()) ctx->nal->decoder.Flush();
	return 0;
}

// Seeking in MP4 mode: what was fed in and not yet shown is dropped.
u32 sceMpegFlushAu(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	if (ctx->nal) {
		ctx->nal->au.clear();
		if (ctx->nal->decoder.IsOpen()) ctx->nal->decoder.Flush();
	}
	return 0;
}

int sceMpegInitAu(u32 mpeg, u32 bufferAddr, u32 auPointer) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	SceMpegAu sceAu;
	sceAu.read(auPointer);
	if (bufferAddr >= 1 && bufferAddr <= (u32)MPEG_DATA_ES_BUFFERS && ctx->esBuffers[bufferAddr - 1]) {
		// This esbuffer has been allocated for Avc. We stuff the stream id in esBuffer.
		sceAu.esBuffer = 0;
		sceAu.esSize = MPEG_AVC_ES_SIZE;
		sceAu.dts = 0;
		sceAu.pts = 0;
	} else {
		// This esbuffer has been left as Atrac.
		sceAu.esBuffer = 0;
		sceAu.esSize = MPEG_ATRAC_ES_SIZE;
		sceAu.pts = 0;
		sceAu.dts = UNKNOWN_TIMESTAMP;
	}
	sceAu.write(auPointer);
	return 0;
}

int sceMpegQueryAtracEsSize(u32 mpeg, u32 esSizeAddr, u32 outSizeAddr) {
	if (!IsValid4Aligned(esSizeAddr) || !IsValid4Aligned(outSizeAddr)) return -1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	GWrite32(esSizeAddr, MPEG_ATRAC_ES_SIZE);
	GWrite32(outSizeAddr, MPEG_ATRAC_ES_OUTPUT_SIZE);
	return 0;
}

int sceMpegRingbufferAvailableSize(u32 ringbufferAddr) {
	SceMpegRingBuffer *ring = RingPtr(ringbufferAddr);
	if (!ring) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	MpegContext *ctx = getMpegCtx(ring->mpeg);
	if (!ctx) return (int)SCE_MPEG_ERROR_NOT_YET_INIT;

	ctx->mpegRingbufferAddr = ringbufferAddr;
	// Without a fully asynchronous Media Engine draining the buffer, games like Ridge Racer 2
	// rely on this to update the available size. Not before the stream was analyzed, though, to
	// keep manual changes to the struct (video/mpeg/ringbuffer/avail).
	if (ctx->mediaengine && ctx->isAnalyzed) {
		ring->packetsAvail = ring->packets - ctx->mediaengine->getRemainSize() / 2048;
	}
	sched::ChargeCycles(2020);
	sched::Reschedule();
	return ring->packets - ring->packetsAvail;
}

// What PPSSPP's PostPutAction does after each callback. Returns false to stop the put with
// *errorOut as its result.
bool PostPut(u32 ringAddr, int packetsAddedThisRound, int writeOffset, u32 *errorOut) {
	SceMpegRingBuffer *ring = RingPtr(ringAddr);
	if (!ring) return false;
	MpegContext *ctx = getMpegCtx(ring->mpeg);
	if (!ctx) {
		LOGW("MPEG", "sceMpegRingbufferPut: bad mpeg handle %08x", ring->mpeg);
		return false;
	}

	// Validation is done only by older mpeg libs.
	if (g_mpegLibVersion < 0x0105 && packetsAddedThisRound > 0) {
		media::MpegDemux demuxer(packetsAddedThisRound * 2048, 0);
		const int readOffset = ring->packetsRead % ring->packets;
		const u32 bufSize = audiohle::ClampValidSize(ring->data + readOffset * 2048, (u32)packetsAddedThisRound * 2048);
		const u8 *buf = mem::GetPointer(ring->data + readOffset * 2048, bufSize);
		bool invalid = false;
		for (u32 i = 0; buf && i < bufSize / 2048; ++i) {
			demuxer.addStreamData(buf, 2048);
			buf += 2048;
			if (!demuxer.demux(0xFFFF)) invalid = true;
		}
		if (invalid) {
			// Don't accept any of the packets, even the good ones (legit in the Burnout Legends menu).
			LOGW("MPEG", "sceMpegRingbufferPut(): invalid mpeg data");
			*errorOut = SCE_MPEG_ERROR_INVALID_VALUE;
			if (g_mpegLibVersion <= 0x0103) {
				// Act like they were added, but don't increment the read pos.
				ring->packetsWritePos += packetsAddedThisRound;
				ring->packetsAvail += packetsAddedThisRound;
			}
			return false;
		}
	}

	if (ring->packetsRead == 0 && ctx->mediaengine && packetsAddedThisRound > 0) {
		// Initialize the media engine.
		AnalyzeMpeg(ctx->mpegheader, 2048, ctx);
		ctx->mediaengine->loadStream(ctx->mpegheader, 2048, ring->packets * ring->packetSize);
	}
	if (packetsAddedThisRound > 0) {
		if (packetsAddedThisRound > ring->packets - ring->packetsAvail) {
			LOGW("MPEG", "sceMpegRingbufferPut clamping packetsAdded old=%i new=%i", packetsAddedThisRound,
			     ring->packets - ring->packetsAvail);
			packetsAddedThisRound = ring->packets - ring->packetsAvail;
		}
		const u32 at = ring->data + (u32)writeOffset * 2048;
		const u32 dataSize = audiohle::ClampValidSize(at, (u32)packetsAddedThisRound * 2048);
		const u8 *data = mem::GetPointer(at, dataSize);
		const int actuallyAdded = data ? ctx->mediaengine->addStreamData(data, (int)dataSize) / 2048 : 0;
		if (actuallyAdded != packetsAddedThisRound) {
			LOGW("MPEG", "sceMpegRingbufferPut(): unable to enqueue all added packets (%d != %d)", actuallyAdded,
			     packetsAddedThisRound);
		}
		ring->packetsRead += packetsAddedThisRound;
		ring->packetsWritePos += packetsAddedThisRound;
		ring->packetsAvail += packetsAddedThisRound;
	}
	return true;
}

u32 sceMpegRingbufferPut(u32 ringbufferAddr, int numPackets, int available) {
	SceMpegRingBuffer *ring = RingPtr(ringbufferAddr);
	if (!ring) return (u32)-1;

	numPackets = std::min(numPackets, available);
	// Games generally call sceMpegRingbufferAvailableSize() first, but still check (Patapon 3).
	numPackets = std::min(numPackets, ring->packets - ring->packetsAvail);
	if (numPackets <= 0) return 0;

	MpegContext *ctx = getMpegCtx(ring->mpeg);
	if (!ctx) return (u32)-1;
	if (ring->callback_addr == 0) {
		LOGW("MPEG", "sceMpegRingbufferPut: callback_addr zero");
		return 0;
	}

	Ctx *c = cpu::CurrentCtx();
	int total = 0;
	int remaining = numPackets;
	int lastResult = 0;
	while (remaining > 0) {
		ring = RingPtr(ringbufferAddr);
		if (!ring || ring->packets <= 0) break;
		const int writeOffset = ring->packetsWritePos % ring->packets;
		const int desired = std::min(remaining, ring->packets - writeOffset);
		const u32 callback = ring->callback_addr;
		const u32 args[3] = {ring->data + (u32)writeOffset * 2048, (u32)desired, (u32)ring->callback_args};
		lastResult = (int)cpu::CallGuest(c, callback, {args[0], args[1], args[2]});
		u32 error = 0;
		if (!PostPut(ringbufferAddr, lastResult, writeOffset, &error)) {
			if (error) return error;
			break;
		}
		if (lastResult <= 0) break;
		total += lastResult;
		remaining -= std::min(lastResult, remaining);
	}
	if (lastResult < 0 && total == 0) return (u32)lastResult;
	return (u32)total;
}

int sceMpegGetAvcAu(u32 mpeg, u32 streamId, u32 auAddr, u32 attrAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
	if (!ring) return -1;

	SceMpegAu avcAu;
	avcAu.read(auAddr);

	if (ring->packetsRead == 0 || ring->packetsAvail == 0) {
		avcAu.pts = 0;
		avcAu.dts = 0;
		avcAu.write(auAddr);
		return (int)Delay(SCE_MPEG_ERROR_NO_DATA, 2000);
	}

	auto streamInfo = ctx->streamMap.find(streamId);
	if (streamInfo == ctx->streamMap.end()) return -1;

	if (ctx->streamIgnoreMap[streamId]) {
		avcAu.pts = ctx->mediaengine->getVideoTimeStamp() + ctx->mpegFirstTimestamp;
		avcAu.dts = avcAu.pts - videoTimestampStep;
		avcAu.esBuffer = (u32)streamInfo->second.num;
		avcAu.write(auAddr);
		if (IsValid4Aligned(attrAddr)) GWrite32(attrAddr, 1);
		return (int)Delay(0, 100);
	}

	if (streamInfo->second.needsReset) {
		avcAu.pts = 0;
		streamInfo->second.needsReset = false;
	}

	// esBuffer is where this AU's data goes. We don't write the data; we use it to keep track of
	// the stream number.
	avcAu.esBuffer = (u32)streamInfo->second.num;

	int result = 0;
	avcAu.pts = ctx->mediaengine->getVideoTimeStamp() + ctx->mpegFirstTimestamp;
	avcAu.dts = avcAu.pts - videoTimestampStep;
	if (ctx->mediaengine->IsVideoEnd()) {
		avcAu.dts = -1;
		result = (int)SCE_MPEG_ERROR_NO_DATA;
	}
	avcAu.write(auAddr);

	// Jeanne d'Arc passes 0 as attrAddr.
	if (result == 0 && mem::IsValidAddress(attrAddr)) W32(attrAddr, 1);
	return (int)Delay((u32)result, 100);
}

u32 sceMpegFinish() {
	if (!g_isMpegInit) LOGD("MPEG", "sceMpegFinish(...): not initialized");
	g_isMpegInit = false;
	return Delay(0, 250);
}

u32 sceMpegQueryMemSize() { return MpegRequiredMem(); }

int sceMpegGetAtracAu(u32 mpeg, u32 streamId, u32 auAddr, u32 attrAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
	if (!ring) return -1;

	SceMpegAu atracAu;
	atracAu.read(auAddr);

	auto streamInfo = ctx->streamMap.find(streamId);
	if (streamInfo != ctx->streamMap.end() && streamInfo->second.needsReset) {
		atracAu.pts = 0;
		streamInfo->second.needsReset = false;
	}
	if (streamInfo == ctx->streamMap.end()) LOGW("MPEG", "sceMpegGetAtracAu: invalid audio stream %08x", streamId);

	if (streamInfo != ctx->streamMap.end() && ctx->streamIgnoreMap[streamId]) {
		atracAu.pts = ctx->mediaengine->getAudioTimeStamp() + ctx->mpegFirstTimestamp;
		atracAu.dts = atracAu.pts;
		atracAu.esBuffer = (u32)streamInfo->second.num;
		atracAu.write(auAddr);
		if (IsValid4Aligned(attrAddr)) GWrite32(attrAddr, 0);
		return (int)Delay(0, 100);
	}

	// The audio can end earlier than the video does.
	if (ring->packetsAvail == 0) {
		atracAu.pts = 0;
		atracAu.dts = 0;
		atracAu.write(auAddr);
		return (int)Delay(SCE_MPEG_ERROR_NO_DATA, mpegDecodeErrorDelayUs);
	}

	if (streamInfo != ctx->streamMap.end()) {
		atracAu.esBuffer = (u32)streamInfo->second.num;
		ctx->mediaengine->setAudioStream(streamInfo->second.num);
	}

	int result = 0;
	atracAu.pts = ctx->mediaengine->getAudioTimeStamp() + ctx->mpegFirstTimestamp;
	atracAu.dts = atracAu.pts;
	if (ctx->mediaengine->IsNoAudioData()) {
		atracAu.dts = -1;
		result = (int)SCE_MPEG_ERROR_NO_DATA;
	}
	if (ctx->atracRegistered && ctx->mediaengine->IsNoAudioData() && !ctx->endOfAudioReached) {
		ctx->endOfAudioReached = true;
	}
	atracAu.write(auAddr);

	// The 3rd Birthday passes 0 as attrAddr.
	if (result == 0 && IsValid4Aligned(attrAddr)) GWrite32(attrAddr, 0);
	return (int)Delay((u32)result, 100);
}

int sceMpegQueryPcmEsSize(u32 mpeg, u32 esSizeAddr, u32 outSizeAddr) {
	if (!IsValid4Aligned(esSizeAddr) || !IsValid4Aligned(outSizeAddr)) return -1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	GWrite32(esSizeAddr, MPEG_PCM_ES_SIZE);
	GWrite32(outSizeAddr, MPEG_PCM_ES_OUTPUT_SIZE);
	return 0;
}

u32 sceMpegChangeGetAuMode(u32 mpeg, int streamUid, int mode) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return SCE_MPEG_ERROR_INVALID_VALUE;
	if (mode != MPEG_AU_MODE_DECODE && mode != MPEG_AU_MODE_SKIP) return SCE_MPEG_ERROR_INVALID_VALUE;
	auto stream = ctx->streamMap.find((u32)streamUid);
	if (stream == ctx->streamMap.end()) return SCE_MPEG_ERROR_INVALID_VALUE;
	ctx->streamIgnoreMap[(u32)streamUid] = mode == MPEG_AU_MODE_SKIP;
	return 0;
}

u32 sceMpegChangeGetAvcAuMode(u32 mpeg, u32 stream_addr, int mode) {
	(void)mode;
	if (!mem::IsValidAddress(stream_addr)) return (u32)-1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	return 0;
}

u32 sceMpegGetPcmAu(u32 mpeg, int streamUid, u32 auAddr, u32 attrAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
	if (!ring) return (u32)-1;
	if (!mem::IsValidAddress((u32)streamUid)) return SCE_MPEG_ERROR_INVALID_ADDR;
	SceMpegAu atracAu;
	atracAu.read(auAddr);
	auto streamInfo = ctx->streamMap.find((u32)streamUid);
	if (streamInfo == ctx->streamMap.end()) return (u32)-1;
	atracAu.write(auAddr);
	u32 attr = 1 << 7;  // sampling rate (1 = 44.1kHz)
	attr |= 2;          // number of channels (1 - mono / 2 - stereo)
	W32(attrAddr, attr);
	return 0;
}

int sceMpegRingbufferQueryPackNum(u32 memorySize) { return (int)(memorySize / (2048 + 104)); }

u32 sceMpegFlushAllStream(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	ctx->isAnalyzed = false;
	SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
	if (ring) {
		ring->packetsAvail = 0;
		ring->packetsRead = 0;
		ring->packetsWritePos = 0;
	}
	return 0;
}

u32 sceMpegFlushStream(u32 mpeg, int stream_addr) {
	if (!mem::IsValidAddress((u32)stream_addr)) return (u32)-1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	return 0;
}

u32 sceMpegAvcCopyYCbCr(u32 mpeg, u32 sourceAddr, u32 YCbCrAddr) {
	if (!mem::IsValidAddress(sourceAddr) || !mem::IsValidAddress(YCbCrAddr)) return (u32)-1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	return 0;
}

u32 sceMpegAtracDecode(u32 mpeg, u32 auAddr, u32 bufferAddr, int init) {
	(void)init;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	if (!mem::IsValidAddress(bufferAddr)) return (u32)-1;

	SceMpegAu atracAu;
	atracAu.read(auAddr);
	// We kept track of the stream number here in sceMpegGetAtracAu().
	ctx->mediaengine->setAudioStream((int)atracAu.esBuffer);

	mem::Memset(bufferAddr, 0, audiohle::ClampValidSize(bufferAddr, MPEG_ATRAC_ES_OUTPUT_SIZE));
	ctx->mediaengine->getAudioSamples(bufferAddr);
	atracAu.pts = ctx->mediaengine->getAudioTimeStamp() + ctx->mpegFirstTimestamp;
	atracAu.write(auAddr);

	SceMpegRingBuffer *ring = RingPtr(ctx->mpegRingbufferAddr);
	if (ring) ring->packetsAvail = ring->packets - ctx->mediaengine->getRemainSize() / 2048;
	return MEDelay(0, atracDecodeDelayUs);
}

// YCbCr -> RGB colour space conversion.
u32 sceMpegAvcCsc(u32 mpeg, u32 sourceAddr, u32 rangeAddr, int frameWidth, u32 destAddr) {
	(void)sourceAddr;
	if (!mem::IsValidAddress(sourceAddr) || !mem::IsValidRange(rangeAddr, 16) || !mem::IsValidAddress(destAddr))
		return (u32)-1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	if (frameWidth == 0) {
		frameWidth = ctx->defaultFrameWidth ? (int)ctx->defaultFrameWidth : ctx->avc.avcDetailFrameWidth;
	}
	const int x = (int)GRead32(rangeAddr);
	const int y = (int)GRead32(rangeAddr + 4);
	const int width = (int)GRead32(rangeAddr + 8);
	const int height = (int)GRead32(rangeAddr + 12);
	if (x < 0 || y < 0 || width < 0 || height < 0) return SCE_KERNEL_ERROR_INVALID_VALUE;

	ctx->mediaengine->writeVideoImageWithRange(destAddr, frameWidth, ctx->videoPixelMode, x, y, width, height);
	// A delay is needed: without it, flickering in Dengeki no Pilot (PPSSPP #7549); and it can't be
	// the decode delay (screen dislocation in Bleach Heat of Soul 6, #5535).
	return MEDelay(0, avcCscDelayUs);
}

u32 sceMpegRingbufferDestruct(u32 ringbufferAddr) {
	(void)ringbufferAddr;
	// Apparently does nothing.
	return 0;
}

u32 sceMpegAvcInitYCbCr(u32 mpeg, int mode, int width, int height, u32 ycbcr_addr) {
	(void)mode;
	(void)width;
	(void)height;
	if (!mem::IsValidAddress(ycbcr_addr)) return (u32)-1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	return 0;
}

int sceMpegAvcQueryYCbCrSize(u32 mpeg, u32 mode, u32 width, u32 height, u32 resultAddr) {
	(void)mpeg;
	(void)mode;
	if ((width & 15) != 0 || (height & 15) != 0 || height > 272 || width > 480) {
		return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	}
	const int size = (int)((width / 2) * (height / 2) * 6 + 128);
	W32(resultAddr, (u32)size);
	return 0;
}

u32 sceMpegQueryUserdataEsSize(u32 mpeg, u32 esSizeAddr, u32 outSizeAddr) {
	if (!mem::IsValidAddress(esSizeAddr) || !mem::IsValidAddress(outSizeAddr)) return (u32)-1;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	W32(esSizeAddr, MPEG_DATA_ES_SIZE);
	W32(outSizeAddr, MPEG_DATA_ES_OUTPUT_SIZE);
	return 0;
}

u32 sceMpegAvcResourceGetAvcDecTopAddr(u32 mpeg) {
	(void)mpeg;
	if ((g_avcResourceFlags & MPEG_AVC_RESOURCE_FLAG) == 0) return 0;
	return g_avcResourceDataAddr & 0xFFC00000;
}

u32 sceMpegAvcResourceFinish(u32 mpeg) {
	(void)mpeg;
	return 0;
}

u32 sceMpegAvcResourceGetAvcEsBuf(u32 mpeg) {
	(void)mpeg;
	if ((g_avcResourceFlags & MPEG_AVC_RESOURCE_FLAG) == 0) return 0;
	return g_avcResourceDataAddr;
}

u32 sceMpegAvcResourceInit(u32 mpeg) {
	if (mpeg != 1) return SCE_MPEG_ERROR_INVALID_VALUE;
	if ((g_avcResourceFlags & MPEG_AVC_RESOURCE_FLAG) != 0) return SCE_MPEG_ERROR_ALREADY_INIT;
	// On hardware this comes from an FPL; simulated like PPSSPP does.
	g_avcResourceAddr = 0x10000000;
	g_avcResourceDataAddr = g_avcResourceAddr + 8;
	g_avcResourceFlags |= MPEG_AVC_RESOURCE_FLAG;
	return 0;
}

int sceMpegAvcConvertToYuv420(u32 mpeg, u32 bufferOutputAddr, u32 bufferAddr, int unknown2) {
	(void)bufferAddr;
	(void)unknown2;
	if (!mem::IsValidAddress(bufferOutputAddr)) return (int)SCE_MPEG_ERROR_INVALID_VALUE;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	if (!ctx->mediaengine->HasFrame()) return (int)SCE_MPEG_ERROR_AVC_INVALID_VALUE;

	// Planar Y, then Cb, then Cr (4:2:0).
	const media::YuvFrame &f = ctx->mediaengine->CurrentFrame();
	const int width = ctx->mediaengine->m_desWidth;
	const int height = ctx->mediaengine->m_desHeight;
	const u32 sizeY = (u32)(f.width * f.height);
	const u32 sizeC = sizeY >> 2;
	if (u8 *out = mem::GetPointer(bufferOutputAddr, sizeY + sizeC * 2)) {
		memcpy(out, f.y.data(), sizeY);
		memcpy(out + sizeY, f.cb.data(), std::min<size_t>(sizeC, f.cb.size()));
		memcpy(out + sizeY + sizeC, f.cr.data(), std::min<size_t>(sizeC, f.cr.size()));
	} else {
		LOGW("MPEG", "Bad output buffer pointer for yuv conv: %08x", bufferOutputAddr);
	}
	return (width << 16) | height;
}

int sceMpegGetUserdataAu(u32 mpeg, u32 streamUid, u32 auAddr, u32 resultAddr) {
	(void)streamUid;
	(void)auAddr;
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return -1;
	if (mem::IsValidRange(resultAddr, 8)) {
		// Seen in Phantasy Star Portable 2.
		GWrite32(resultAddr, 0);
		GWrite32(resultAddr + 4, 0);
	}
	// Userdata isn't demuxed.
	return (int)SCE_MPEG_ERROR_NO_DATA;
}

u32 sceMpegCtxOnlyStub(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) return (u32)-1;
	LOGW("MPEG", "UNIMPL %s(%08x)", hle::CurrentFunctionName(), mpeg);
	return 0;
}

u32 sceMpegNextAvcRpAu(u32 mpeg, u32 streamUid) {
	(void)streamUid;
	return sceMpegCtxOnlyStub(mpeg);
}

// ---- sceMpegbase ----

// Set by sceMpegBaseCscInit / sceMpegBaseCscSetPixelMode, used when the caller passes 0.
int g_mpegBaseBufferWidth = 512;
int g_mpegBasePixelMode = GE_CMODE_32BIT_ABGR8888;
std::vector<u8> g_untileScratch;

struct SceMpegLLI {
	u32 pSrc;
	u32 pDst;
	u32 Next;
	s32 iSize;
};

// The descriptor mpeg.prx builds before each conversion (48 bytes): dimensions in macroblocks
// and the eight buffers, four luma then four chroma (see media::FrameBufferLayout).
struct SceMp4AvcCscStruct {
	s32 height;  // 0x00 in macroblocks
	s32 width;   // 0x04
	s32 unk08;
	s32 unk0c;
	u32 buffer[8];  // 0x10
};
static_assert(sizeof(SceMp4AvcCscStruct) == 0x30, "SceMp4AvcCscStruct layout");

u32 sceMpegBasePESpacketCopy(u32 p) {
	int nBlocks = 0;
	u32 at = p;
	while (mem::IsValidRange(at, sizeof(SceMpegLLI))) {
		nBlocks++;
		if (GRead32(at + 8) == 0) break;  // last block
		at += sizeof(SceMpegLLI);
	}
	MpegSetPmpVideoSource(p, nBlocks);

	// The DMA copy. Each block says where in ME memory it lands.
	at = p;
	for (int i = 0; i < nBlocks; i++, at += sizeof(SceMpegLLI)) {
		SceMpegLLI lli;
		memcpy(&lli, GPtr(at), sizeof(lli));
		if (lli.iSize <= 0) continue;
		const u8 *src = mem::GetPointer(lli.pSrc, (u32)lli.iSize);
		if (!src) continue;
		if (u8 *me = MEGetPointerRange(lli.pDst, (u32)lli.iSize)) {
			memcpy(me, src, lli.iSize);
		} else if (u8 *dst = mem::GetPointer(lli.pDst, (u32)lli.iSize)) {
			memmove(dst, src, lli.iSize);
		}
	}
	return 0;
}

bool ReadTiledYCbCr(const u32 *buffers, int width, int height, const u8 **luma, const u8 **cb, const u8 **cr) {
	int sizes[8];
	media::FrameBufferLayout(width, height, sizes, nullptr);
	const u8 *src[8]{};
	for (int i = 0; i < 8; i++) {
		if (sizes[i] > 0) {
			src[i] = MEOrGuestPointer(buffers[i], (u32)sizes[i]);
			if (!src[i]) return false;
		}
	}
	const size_t lumaBytes = (size_t)width * height;
	const size_t chromaBytes = (size_t)(width >> 1) * (height >> 1);
	if (g_untileScratch.size() < lumaBytes + chromaBytes * 2) g_untileScratch.resize(lumaBytes + chromaBytes * 2);
	u8 *l = g_untileScratch.data();
	u8 *b = l + lumaBytes;
	u8 *r = b + chromaBytes;
	media::UntileYCbCr(l, b, r, src, sizes, width, height);
	*luma = l;
	*cb = b;
	*cr = r;
	return true;
}

int MpegBaseCscRange(u32 bufferRGB, u32 cscAddr, int bufferWidth, int rangeX, int rangeY, int rangeWidth,
                     int rangeHeight) {
	if (!mem::IsValidRange(cscAddr, sizeof(SceMp4AvcCscStruct))) return -1;
	SceMp4AvcCscStruct csc;
	memcpy(&csc, GPtr(cscAddr), sizeof(csc));
	if (bufferWidth == 0) bufferWidth = g_mpegBaseBufferWidth;
	const int width = csc.width << 4;
	const int height = csc.height << 4;
	if (width <= 0 || height <= 0 || width > 1024 || height > 1024) return -1;
	if (rangeWidth <= 0 || rangeHeight <= 0) return 0;
	rangeWidth = std::min(rangeWidth, width - rangeX);
	rangeHeight = std::min(rangeHeight, height - rangeY);
	rangeWidth = std::min(rangeWidth, bufferWidth);
	if (rangeX < 0 || rangeY < 0 || rangeWidth <= 0 || rangeHeight <= 0) return -1;

	const u8 *luma, *cb, *cr;
	if (!ReadTiledYCbCr(csc.buffer, width, height, &luma, &cb, &cr)) return -1;
	const int bpp = g_mpegBasePixelMode == GE_CMODE_32BIT_ABGR8888 ? 4 : 2;
	const u32 destSize = (u32)(rangeHeight * bufferWidth * bpp);
	u8 *dest = mem::GetPointer(bufferRGB, destSize);
	if (!dest) return -1;
	media::YCbCr420ToPixels(dest, bufferWidth, g_mpegBasePixelMode, luma, cb, cr, width, rangeX, rangeY, rangeWidth,
	                        rangeHeight);
	// Runs on the DMACPLUS and takes real time: 2.4ms for 480x272 to 8888 (video/mpeg/playertiming).
	const int cscUs = std::max(1, (int)(2400LL * rangeWidth * rangeHeight / (480 * 272)));
	return (int)Delay(0, cscUs);
}

int sceMpegBaseCscInit(int bufferWidth) {
	g_mpegBaseBufferWidth = bufferWidth ? bufferWidth : 512;
	return 0;
}

// mpeg.prx indexes {1, 2, 3, 0} with the game's pixel mode to get this value.
int sceMpegBaseCscSetPixelMode(int pixelMode) {
	static const int toGeMode[4] = {3, 0, 1, 2};
	if (pixelMode < 0 || pixelMode >= 4) return -1;
	g_mpegBasePixelMode = toGeMode[pixelMode];
	return 0;
}

int sceMpegBaseCscAvc(u32 bufferRGB, u32 unknown, int bufferWidth, u32 cscAddr) {
	(void)unknown;
	return MpegBaseCscRange(bufferRGB, cscAddr, bufferWidth, 0, 0, 1024, 1024);
}

u32 sceMpegBaseCscAvcRange(u32 bufferRGB, u32 unknown, u32 rangeAddr, int bufferWidth, u32 cscAddr) {
	(void)unknown;
	if (!mem::IsValidRange(rangeAddr, 16)) return (u32)-1;
	// In macroblocks.
	const int rangeX = (int)GRead32(rangeAddr + 0) << 4;
	const int rangeY = (int)GRead32(rangeAddr + 4) << 4;
	const int rangeWidth = (int)GRead32(rangeAddr + 8) << 4;
	const int rangeHeight = (int)GRead32(rangeAddr + 12) << 4;
	return (u32)MpegBaseCscRange(bufferRGB, cscAddr, bufferWidth, rangeX, rangeY, rangeWidth, rangeHeight);
}

// Moves a decoded frame between two sets of buffers (both descriptors). Flag bit 0 selects
// buffers 0,1,4,5, bit 1 selects 2,3,6,7.
int sceMpegBaseYCrCbCopy(u32 dstAddr, u32 srcAddr, int flags) {
	if (!mem::IsValidRange(dstAddr, sizeof(SceMp4AvcCscStruct)) || !mem::IsValidRange(srcAddr, sizeof(SceMp4AvcCscStruct)))
		return -1;
	SceMp4AvcCscStruct dst, src;
	memcpy(&dst, GPtr(dstAddr), sizeof(dst));
	memcpy(&src, GPtr(srcAddr), sizeof(src));
	// The callers build the source descriptor on the stack with the size in pixels; the
	// destination states it in macroblocks, when at all (msvideo_main_plugin leaves it unset).
	int width = src.width, height = src.height;
	if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
		width = dst.width << 4;
		height = dst.height << 4;
	}
	width = (width + 15) & ~15;
	height = (height + 15) & ~15;
	if (width <= 0 || height <= 0 || width > 1024 || height > 1024) return -1;
	int sizes[8];
	media::FrameBufferLayout(width, height, sizes, nullptr);
	for (int i = 0; i < 8; i++) {
		const int bit = ((i & 3) < 2) ? 1 : 2;
		if (!(flags & bit) || sizes[i] <= 0) continue;
		const u8 *from = MEOrGuestPointer(src.buffer[i], (u32)sizes[i]);
		if (!from) return -1;
		u8 *to = mem::GetPointer(dst.buffer[i], (u32)sizes[i]);
		if (!to) return -1;
		memmove(to, from, sizes[i]);
	}
	return 0;
}

}  // namespace

void MpegSetPmpVideoSource(u32 addr, int blocks) {
	g_pmpVideoSource = addr;
	g_pmpNBlocks = blocks;
}

void MpegNotifyLoadModule(int version) {
	g_mpegLibVersion = version;
	LOGI("MPEG", "sceMpeg library version %04x", version);
}

void Register_sceMpeg() {
	static const hle::HleFunction funcs[] = {
	    HLEFN(0xE1CE83A7, sceMpegGetAtracAu),
	    HLEFN(0xFE246728, sceMpegGetAvcAu),
	    HLEFN(0xD8C5F121, sceMpegCreate),
	    HLEFN(0xF8DCB679, sceMpegQueryAtracEsSize),
	    HLEFN(0xC132E22F, sceMpegQueryMemSize),
	    HLEFN(0x21FF80E4, sceMpegQueryStreamOffset),
	    HLEFN(0x611E9E11, sceMpegQueryStreamSize),
	    HLEFN(0x42560F23, sceMpegRegistStream),
	    HLEFN(0x591A4AA2, sceMpegUnRegistStream),
	    HLEFN(0x707B7629, sceMpegFlushAllStream),
	    HLEFN(0x500F0429, sceMpegFlushStream),
	    HLEFN(0xA780CF7E, sceMpegMallocAvcEsBuf),
	    HLEFN(0xCEB870B1, sceMpegFreeAvcEsBuf),
	    HLEFN(0x167AFD9E, sceMpegInitAu),
	    HLEFN(0x682A619B, sceMpegInit),
	    HLEFN(0x606A4649, sceMpegDelete),
	    HLEFN(0x874624D6, sceMpegFinish),
	    HLEFN(0x800C44DF, sceMpegAtracDecode),
	    HLEFN(0x0E3C2E9D, sceMpegAvcDecode),
	    HLEFN(0x740FCCD1, sceMpegAvcDecodeStop),
	    HLEFN(0x4571CC64, sceMpegAvcDecodeFlush),
	    HLEFN(0x0F6C18D7, sceMpegAvcDecodeDetail),
	    HLEFN(0xA11C7026, sceMpegAvcDecodeMode),
	    HLEFN(0x37295ED8, sceMpegRingbufferConstruct),
	    HLEFN(0x13407F13, sceMpegRingbufferDestruct),
	    HLEFN(0xB240A59E, sceMpegRingbufferPut),
	    HLEFN(0xB5F6DC87, sceMpegRingbufferAvailableSize),
	    HLEFN(0xD7A29F46, sceMpegRingbufferQueryMemSize),
	    HLEFN(0x769BEBB6, sceMpegRingbufferQueryPackNum),
	    HLEFN(0x211A057C, sceMpegAvcQueryYCbCrSize),
	    HLEFN(0xF0EB1125, sceMpegAvcDecodeYCbCr),
	    HLEFN(0xF2930C9C, sceMpegAvcDecodeStopYCbCr),
	    HLEFN(0x67179B1B, sceMpegAvcInitYCbCr),
	    HLEFN(0x0558B075, sceMpegAvcCopyYCbCr),
	    HLEFN(0x31BD0272, sceMpegAvcCsc),
	    HLEFN(0x9DCFB7EA, sceMpegChangeGetAuMode),
	    HLEFN(0x8C1E027D, sceMpegGetPcmAu),
	    HLEFN(0xC02CF6B5, sceMpegQueryPcmEsSize),
	    HLEFN(0xC45C99CC, sceMpegQueryUserdataEsSize),
	    HLEFN(0x234586AE, sceMpegChangeGetAvcAuMode),
	    HLEFN(0x63B9536A, sceMpegAvcResourceGetAvcDecTopAddr),
	    HLEFN(0x8160A2FE, sceMpegAvcResourceFinish),
	    HLEFN(0xAF26BB01, sceMpegAvcResourceGetAvcEsBuf),
	    HLEFN(0xFCBDB5AD, sceMpegAvcResourceInit),
	    HLEFN(0xF5E7EA31, sceMpegAvcConvertToYuv420),
	    HLEFN(0x01977054, sceMpegGetUserdataAu),
	    HLEFN(0x3C37A7A6, sceMpegNextAvcRpAu),
	    HLEFN(0x11F95CF1, sceMpegGetAvcNalAu),
	    HLEFN(0xAB0E9556, sceMpegAvcDecodeDetailIndex),
	    HLEFN(0xCF3547A2, sceMpegAvcDecodeDetail2),
	    HLEFN(0x6F314410, sceMpeg_6F314410),
	    hle::HleFunction{0x921FCCCF, &hle::Wrap<&sceMpegCtxOnlyStub>, "sceMpegGetAvcEsAu", 0},
	    hle::HleFunction{0xE95838F6, &hle::Wrap<&sceMpegCtxOnlyStub>, "sceMpegAvcCscInfo", 0},
	    hle::HleFunction{0xD1CE4950, &hle::Wrap<&sceMpegCtxOnlyStub>, "sceMpegAvcCscMode", 0},
	    HLEFN(0xDBB60658, sceMpegFlushAu),
	    HLESTUB(0xD4DD6E75, "sceMpeg_D4DD6E75"),
	    HLESTUB(0x11CAB459, "sceMpeg_11CAB459"),
	    HLESTUB(0xC345DED2, "sceMpeg_C345DED2"),
	    HLESTUB(0xB27711A8, "sceMpeg_B27711A8"),
	    HLESTUB(0x988E9E12, "sceMpeg_988E9E12"),
	};
	hle::RegisterModule("sceMpeg", funcs);
}

void Register_sceMpegbase() {
	static const hle::HleFunction funcs[] = {
	    HLEFN(0xBEA18F91, sceMpegBasePESpacketCopy),
	    HLEFN(0x492B5E4B, sceMpegBaseCscInit),
	    hle::HleFunction{0x0530BE4E, &hle::Wrap<&sceMpegBaseCscSetPixelMode>, "sceMpegbase_0530BE4E", 0},
	    HLEFN(0x91929A21, sceMpegBaseCscAvc),
	    HLEFN(0x304882E1, sceMpegBaseCscAvcRange),
	    HLEFN(0x7AC0321A, sceMpegBaseYCrCbCopy),
	};
	hle::RegisterModule("sceMpegbase", funcs);
}

}  // namespace mediahle
