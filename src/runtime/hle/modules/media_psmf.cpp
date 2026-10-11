// SPDX-License-Identifier: GPL-2.0-or-later
//
// scePsmf (PSMF header parsing) and scePsmfPlayer (a complete movie player around the Media
// Engine). Ported from PPSSPP's Core/HLE/scePsmf.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later), itself based on JPCSP.
#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "common/log.h"
#include "loader/elf.h"
#include "loader/psp_header.h"
#include "media/media_engine.h"
#include "media/mpeg_demux.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/hle/modules/media_hle.h"
#include "runtime/io/io.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

using namespace kernel;
using media::MediaEngine;
using media::ReadBE16;
using media::ReadBE32;

namespace mediahle {

namespace {

constexpr int PSMF_AVC_STREAM = 0;
constexpr int PSMF_ATRAC_STREAM = 1;
constexpr int PSMF_PCM_STREAM = 2;
constexpr int PSMF_AUDIO_STREAM = 15;
constexpr int PSMF_PLAYER_VERSION_FULL = 0;
constexpr int PSMF_PLAYER_VERSION_BASIC = 1;
constexpr int PSMF_PLAYER_CONFIG_LOOP = 0;
constexpr int PSMF_PLAYER_CONFIG_NO_LOOP = 1;
constexpr int PSMF_PLAYER_CONFIG_MODE_LOOP = 0;
constexpr int PSMF_PLAYER_CONFIG_MODE_PIXEL_TYPE = 1;
constexpr int PSMF_PLAYER_WARMUP_FRAMES = 3;
constexpr int VIDEO_FRAME_DURATION_TS = 3003;
constexpr int audioSamples = 2048;
constexpr int audioSamplesBytes = audioSamples * 4;
constexpr int GE_CMODE_32BIT_ABGR8888 = 3;

int g_videoPixelMode = GE_CMODE_32BIT_ABGR8888;
int g_videoLoopStatus = PSMF_PLAYER_CONFIG_NO_LOOP;
int g_psmfPlayerLibVersion = 0;

enum PsmfPlayerStatus {
	PSMF_PLAYER_STATUS_NONE = 0x0,
	PSMF_PLAYER_STATUS_INIT = 0x1,
	PSMF_PLAYER_STATUS_STANDBY = 0x2,
	PSMF_PLAYER_STATUS_PLAYING = 0x4,
	PSMF_PLAYER_STATUS_ERROR = 0x100,
	PSMF_PLAYER_STATUS_PLAYING_FINISHED = 0x200,
};

enum PsmfPlayerMode {
	PSMF_PLAYER_MODE_PLAY = 0,
	PSMF_PLAYER_MODE_SLOWMOTION = 1,
	PSMF_PLAYER_MODE_STEPFRAME = 2,
	PSMF_PLAYER_MODE_PAUSE = 3,
	PSMF_PLAYER_MODE_FORWARD = 4,
	PSMF_PLAYER_MODE_REWIND = 5,
};

struct PsmfData {
	u32 version;
	u32 headerSize;
	u32 headerOffset;
	u32 streamSize;
	u32 streamOffset;
	u32 streamNum;
	u32 unk1;
	u32 unk2;
};

struct PsmfPlayerCreateData {
	u32 buffer;
	u32 bufferSize;
	s32 threadPriority;
};

struct PsmfPlayerData {
	s32 videoCodec;
	s32 videoStreamNum;
	s32 audioCodec;
	s32 audioStreamNum;
	s32 playMode;
	s32 playSpeed;
};

struct PsmfInfo {
	u32 lastFrameTS;
	s32 numVideoStreams;
	s32 numAudioStreams;
	s32 numPCMStreams;
	s32 playerVersion;
};

struct PsmfVideoData {
	s32 frameWidth;
	u32 displaybuf;
	u32 displaypts;
};

struct PsmfEntry {
	int EPPts;
	int EPOffset;
	int EPIndex;
	int EPPicOffset;
};

u32 R32(u32 addr) { return mem::IsValidRange(addr, 4) ? GRead32(addr) : 0; }
void W32(u32 addr, u32 v) {
	if (mem::IsValidRange(addr, 4)) GWrite32(addr, v);
}
u32 Delay(u32 result, int us) { return audiohle::DelayResult(result, us); }

class Psmf;

class PsmfStream {
public:
	enum { INVALID = -1 };
	PsmfStream(int type, int channel) : type_(type), channel_(channel) {}

	void readMPEGVideoStreamParams(const u8 *addr, const u8 *data, u32 dataSize, Psmf *psmf);
	void readPrivateAudioStreamParams(const u8 *addr) {
		audioChannels_ = addr[14];
		// "Frequency" is usually 2, but that's what scePsmfGetAudioInfo() writes too.
		audioFrequency_ = addr[15];
	}
	bool matchesType(int ty) const {
		if (ty == PSMF_AUDIO_STREAM) return type_ == PSMF_ATRAC_STREAM || type_ == PSMF_PCM_STREAM;
		return type_ == ty;
	}

	int type_;
	int channel_;
	int videoWidth_ = INVALID;
	int videoHeight_ = INVALID;
	int audioChannels_ = INVALID;
	int audioFrequency_ = INVALID;
};

class Psmf {
public:
	// data is the guest address of the header (0 for a host copy); dataSize how much is readable.
	Psmf(const u8 *ptr, u32 data, u32 dataSize);
	~Psmf() {
		for (auto &it : streamMap) delete it.second;
	}

	bool isValidCurrentStreamNumber() const {
		return currentStreamNum >= 0 && streamMap.find(currentStreamNum) != streamMap.end();
	}
	bool setStreamNum(u32 psmfStruct, int num, bool updateCached = true);
	bool setStreamWithType(u32 psmfStruct, int type, int channel);
	bool setStreamWithTypeNumber(u32 psmfStruct, int type, int n);
	int FindEPWithTimestamp(int pts) const;

	u32 magic = 0;
	u32 version = 0;
	u32 streamOffset = 0;
	u32 streamSize = 0;
	u32 headerSize = 0;
	u32 headerOffset = 0;
	u32 streamDataTotalSize = 0;
	u32 presentationStartTime = 0;
	u32 presentationEndTime = 0;
	u32 streamDataNextBlockSize = 0;
	u32 streamDataNextInnerBlockSize = 0;

	int numStreams = 0;
	int currentStreamNum = 0;
	int currentStreamType = 0;
	int currentStreamChannel = 0;

	u32 EPMapOffset = 0;
	u32 EPMapEntriesNum = 0;
	std::vector<PsmfEntry> EPMap;
	std::map<int, PsmfStream *> streamMap;
};

void PsmfStream::readMPEGVideoStreamParams(const u8 *addr, const u8 *data, u32 dataSize, Psmf *psmf) {
	psmf->EPMapOffset = ReadBE32(&addr[4]);
	psmf->EPMapEntriesNum = ReadBE32(&addr[8]);
	videoWidth_ = addr[12] * 16;
	videoHeight_ = addr[13] * 16;

	const u32 EP_MAP_STRIDE = 1 + 1 + 4 + 4;
	const u64 epMapBytes = EP_MAP_STRIDE * (u64)psmf->EPMapEntriesNum;
	if ((u64)psmf->EPMapOffset + epMapBytes > dataSize) {
		const u64 avail = dataSize > psmf->EPMapOffset ? dataSize - psmf->EPMapOffset : 0;
		psmf->EPMapEntriesNum = (u32)(avail / EP_MAP_STRIDE);
	}
	psmf->EPMap.clear();
	for (u32 i = 0; i < psmf->EPMapEntriesNum; i++) {
		const u8 *entryAddr = data + psmf->EPMapOffset + EP_MAP_STRIDE * i;
		PsmfEntry entry;
		entry.EPIndex = entryAddr[0];
		entry.EPPicOffset = entryAddr[1];
		entry.EPPts = (int)ReadBE32(&entryAddr[2]);
		entry.EPOffset = (int)ReadBE32(&entryAddr[6]);
		psmf->EPMap.push_back(entry);
	}
}

Psmf::Psmf(const u8 *ptr, u32 data, u32 dataSize) {
	headerOffset = data;
	magic = (u32)ptr[0] | ((u32)ptr[1] << 8) | ((u32)ptr[2] << 16) | ((u32)ptr[3] << 24);
	version = (u32)ptr[4] | ((u32)ptr[5] << 8) | ((u32)ptr[6] << 16) | ((u32)ptr[7] << 24);
	streamOffset = ReadBE32(&ptr[8]);
	streamSize = ReadBE32(&ptr[12]);
	streamDataTotalSize = ReadBE32(&ptr[0x50]);
	presentationStartTime = (u32)media::GetMpegTimeStamp(ptr + media::PSMF_FIRST_TIMESTAMP_OFFSET);
	presentationEndTime = (u32)media::GetMpegTimeStamp(ptr + media::PSMF_LAST_TIMESTAMP_OFFSET);
	streamDataNextBlockSize = ReadBE32(&ptr[0x6A]);
	streamDataNextInnerBlockSize = ReadBE32(&ptr[0x7C]);
	numStreams = ReadBE16(&ptr[0x80]);
	headerSize = 0x800;

	currentStreamNum = -1;
	currentStreamType = -1;
	currentStreamChannel = -1;

	if (0x82 + (u64)numStreams * 16 > dataSize) {
		LOGW("PSMF", "Invalid PSMF stream count: %d", numStreams);
		numStreams = dataSize > 0x82 ? (int)((dataSize - 0x82) / 16) : 0;
	}
	for (int i = 0; i < numStreams; i++) {
		PsmfStream *stream = nullptr;
		const u8 *currentStreamAddr = ptr + 0x82 + i * 16;
		const int streamId = currentStreamAddr[0];
		if ((streamId & media::PSMF_VIDEO_STREAM_ID) == media::PSMF_VIDEO_STREAM_ID) {
			stream = new PsmfStream(PSMF_AVC_STREAM, streamId & 0x0F);
			stream->readMPEGVideoStreamParams(currentStreamAddr, ptr, dataSize, this);
		} else if ((streamId & media::PSMF_AUDIO_STREAM_ID) == media::PSMF_AUDIO_STREAM_ID) {
			int type = PSMF_ATRAC_STREAM;
			const int privateStreamId = currentStreamAddr[1];
			if ((privateStreamId & 0xF0) != 0) {
				LOGW("PSMF", "Unknown private stream type, assuming PCM: %02x", privateStreamId);
				type = PSMF_PCM_STREAM;
			}
			stream = new PsmfStream(type, privateStreamId & 0x0F);
			stream->readPrivateAudioStreamParams(currentStreamAddr);
		}
		if (stream) {
			currentStreamNum++;
			streamMap[currentStreamNum] = stream;
		}
	}
	// Default to the first stream.
	currentStreamNum = 0;
}

bool Psmf::setStreamNum(u32 psmfStruct, int num, bool updateCached) {
	currentStreamNum = num;
	if (mem::IsValidRange(psmfStruct, sizeof(PsmfData))) ((PsmfData *)GPtr(psmfStruct))->streamNum = (u32)num;
	// One of the functions can set this to invalid without invalidating these values.
	if (updateCached) {
		currentStreamType = -1;
		currentStreamChannel = -1;
	}
	if (!isValidCurrentStreamNumber()) return false;
	auto iter = streamMap.find(currentStreamNum);
	if (iter == streamMap.end()) return false;
	// This information seems to only be for the scePsmf lookups.
	currentStreamType = iter->second->type_;
	currentStreamChannel = iter->second->channel_;
	return true;
}

bool Psmf::setStreamWithType(u32 psmfStruct, int type, int channel) {
	for (const auto &iter : streamMap) {
		// Note: this does NOT support PSMF_AUDIO_STREAM.
		if (iter.second->type_ == type && iter.second->channel_ == channel) return setStreamNum(psmfStruct, iter.first);
	}
	return false;
}

bool Psmf::setStreamWithTypeNumber(u32 psmfStruct, int type, int n) {
	for (const auto &iter : streamMap) {
		if (iter.second->matchesType(type)) {
			if (n != 0) {
				n--;
				continue;
			}
			return setStreamNum(psmfStruct, iter.first);
		}
	}
	return false;
}

int Psmf::FindEPWithTimestamp(int pts) const {
	int best = -1;
	int bestPts = 0;
	for (int i = 0; i < (int)EPMap.size(); ++i) {
		const int matchPts = EPMap[i].EPPts;
		if (matchPts == pts) return i;
		if (matchPts < pts && matchPts >= bestPts) {
			best = i;
			bestPts = matchPts;
		}
	}
	return best;
}

class PsmfPlayer {
public:
	explicit PsmfPlayer(const PsmfPlayerCreateData &data) {
		psmfPlayerAtracAu = {-1, -1};
		psmfPlayerAvcAu = {-1, -1};
		displayBuffer = data.buffer;
		displayBufferSize = (int)data.bufferSize;
		playbackThreadPriority = data.threadPriority;
		mediaengine = std::make_unique<MediaEngine>();
	}
	~PsmfPlayer() { CloseFile(); }

	void CloseFile() {
		if (dev && filehandle >= 0) dev->Close(filehandle);
		dev.reset();
		filehandle = -1;
	}

	bool HasReachedEnd() {
		// The pts are ignored: the end is when we're out of data. On hardware that's as soon as the
		// last picture is out (video/psmfplayer/update, getvideodata), and a declared audio stream
		// that has no data at all ends it too (video/psmfplayer/start with test_streams.pmf).
		MediaEngine *me = mediaengine.get();
		const bool audioDone = me->IsNoAudioData() || !me->IsActuallyPlayingAudio();
		if ((me->IsVideoEnd() || me->IsLastFrameDecoded()) && audioDone) return true;
		// Audio is interleaved from the start, so 64KB without any says there is none.
		return totalAudioStreams > 0 && readSize >= std::min(streamSize, 0x10000) && audioStreamNum >= 0 &&
		       !me->HasAudioStreamData(audioStreamNum) && me->IsNoAudioData();
	}

	std::shared_ptr<io::Device> dev;
	int filehandle = -1;
	u32 fileoffset = 0;
	int readSize = 0;
	int streamSize = 0;
	std::vector<u8> tempbuf = std::vector<u8>(0x10000);

	int videoCodec = -1;
	int videoStreamNum = -1;
	int audioCodec = -1;
	int audioStreamNum = -1;
	int playMode = 0;
	int playSpeed = 1;
	u64 totalDurationTimestamp = 0;

	u32 displayBuffer = 0;
	int displayBufferSize = 0;
	int playbackThreadPriority = 0;
	int totalVideoStreams = 0;
	int totalAudioStreams = 0;
	int playerVersion = PSMF_PLAYER_VERSION_FULL;
	int videoStep = 0;
	int warmUp = 0;
	s64 seekDestTimeStamp = 0;

	int videoWidth = 480;
	int videoHeight = 272;

	struct Au {
		s64 pts;
		s64 dts;
	};
	Au psmfPlayerAtracAu;
	Au psmfPlayerAvcAu;
	PsmfPlayerStatus status = PSMF_PLAYER_STATUS_INIT;
	bool finishScheduled = false;
	u64 finishEvent = 0;
	u32 generation = 0;

	std::unique_ptr<MediaEngine> mediaengine;
};

std::map<u32, Psmf *> g_psmfMap;
std::map<u32, PsmfPlayer *> g_psmfPlayerMap;
u32 g_playerGeneration = 0;

Psmf *getPsmf(u32 psmf) {
	if (!mem::IsValidRange(psmf, sizeof(PsmfData))) return nullptr;
	const PsmfData *psmfstruct = (const PsmfData *)GPtr(psmf);
	auto iter = g_psmfMap.find(psmfstruct->headerOffset);
	if (iter == g_psmfMap.end()) return nullptr;
	// Each instance can have its own selected stream.
	iter->second->currentStreamNum = (int)psmfstruct->streamNum;
	return iter->second;
}

PsmfPlayer *getPsmfPlayer(u32 psmfplayer) {
	if (!mem::IsValidRange(psmfplayer, 4)) return nullptr;
	auto iter = g_psmfPlayerMap.find(GRead32(psmfplayer));
	return iter != g_psmfPlayerMap.end() ? iter->second : nullptr;
}

PsmfPlayer *findPlayerByKey(u32 key, u32 generation) {
	auto iter = g_psmfPlayerMap.find(key);
	if (iter == g_psmfPlayerMap.end() || iter->second->generation != generation) return nullptr;
	return iter->second;
}

// Waits delayUs, and the player's status changes when it's over (PPSSPP schedules an event for
// the same moment the delay ends).
u32 DelayWithStateChange(u32 psmfPlayer, PsmfPlayerStatus newState, int delayUs) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return Delay(0, delayUs);
	const u32 key = GRead32(psmfPlayer);
	const u32 generation = player->generation;
	if (delayUs <= 0 || sched::InInterrupt() || !sched::DispatchEnabled() || !sched::Current()) {
		// No waiting possible here: the change comes after the time passes.
		sched::ScheduleEvent(sched::NowUs() + (u64)std::max(delayUs, 0), [key, generation, newState]() {
			if (PsmfPlayer *p = findPlayerByKey(key, generation)) p->status = newState;
		});
		return 0;
	}
	Delay(0, delayUs);
	if (PsmfPlayer *p = findPlayerByKey(key, generation)) p->status = newState;
	return 0;
}

void AbortFinish(PsmfPlayer *player) {
	if (player->finishScheduled) {
		if (player->finishEvent) sched::CancelEvent(player->finishEvent);
		player->finishEvent = 0;
		player->finishScheduled = false;
	}
}

void PlayerFinish(PsmfPlayer *player) {
	player->finishEvent = 0;
	if (player->status == PSMF_PLAYER_STATUS_PLAYING) player->status = PSMF_PLAYER_STATUS_PLAYING_FINISHED;
}

// PPSSPP starts a helper thread at the playback priority that marks the player finished. A
// better priority than the caller's runs as soon as the call returns; a worse one when the
// caller next gives up the CPU.
void ScheduleFinish(u32 psmfPlayer, PsmfPlayer *player) {
	if (player->finishScheduled) return;
	player->finishScheduled = true;
	PspThread *cur = sched::Current();
	if (!cur || player->playbackThreadPriority < cur->currentPriority) {
		PlayerFinish(player);
		return;
	}
	// Approximated by the next scheduling point after a short while: the caller normally waits
	// (sceKernelDelayThread, a decode) soon after.
	const u32 key = GRead32(psmfPlayer);
	const u32 generation = player->generation;
	player->finishEvent = sched::ScheduleEvent(sched::NowUs() + 1000, [key, generation]() {
		if (PsmfPlayer *p = findPlayerByKey(key, generation)) PlayerFinish(p);
	});
}

// ---- scePsmf ----

u32 scePsmfSetPsmf(u32 psmfStruct, u32 psmfData) {
	if (!mem::IsValidAddress(psmfStruct) || !mem::IsValidAddress(psmfData)) return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	const u32 avail = audiohle::ClampValidSize(psmfData, 0x100000);
	if (avail < 0x90) return SCE_PSMF_ERROR_INVALID_PSMF;
	Psmf *psmf = new Psmf(GPtr(psmfData), psmfData, avail);
	if (psmf->magic != media::PSMF_MAGIC) {
		delete psmf;
		return SCE_PSMF_ERROR_INVALID_PSMF;
	}
	// devkit 00000000 supports only '0012', '0013' and '0014'; 03000310+ supports '0015'.
	if (psmf->version == 0) {
		delete psmf;
		return SCE_PSMF_ERROR_BAD_VERSION;
	}
	if (psmf->streamOffset == 0) {
		delete psmf;
		return SCE_PSMF_ERROR_INVALID_VALUE;
	}
	if (!mem::IsValidRange(psmfStruct, sizeof(PsmfData))) {
		delete psmf;
		return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	}
	// This structure changes between versions; these values aren't right, but probably unread.
	PsmfData *data = (PsmfData *)GPtr(psmfStruct);
	memset(data, 0, sizeof(PsmfData));
	data->version = psmf->version;
	data->headerSize = 0x800;
	data->streamSize = psmf->streamSize;
	// This should be, and needs to be, the current stream.
	data->streamNum = (u32)psmf->currentStreamNum;
	data->headerOffset = psmf->headerOffset;

	// The struct is sometimes copied, so a value inside it is the id.
	auto iter = g_psmfMap.find(data->headerOffset);
	if (iter != g_psmfMap.end()) delete iter->second;
	g_psmfMap[data->headerOffset] = psmf;
	return 0;
}

u32 scePsmfGetNumberOfStreams(u32 psmfStruct) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	return (u32)psmf->numStreams;
}

u32 scePsmfGetNumberOfSpecificStreams(u32 psmfStruct, int streamType) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	int streamNum = 0;
	for (const auto &it : psmf->streamMap) {
		if (it.second->matchesType(streamType)) streamNum++;
	}
	return (u32)streamNum;
}

u32 scePsmfSpecifyStreamWithStreamType(u32 psmfStruct, u32 streamType, u32 channel) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!psmf->setStreamWithType(psmfStruct, (int)streamType, (int)channel)) {
		// An invalid type makes the stream number invalid, but keeps the old type/channel.
		psmf->setStreamNum(psmfStruct, (int)SCE_PSMF_ERROR_INVALID_ID, false);
		// Returns 0 even when no stream is found.
		return 0;
	}
	return 0;
}

u32 scePsmfSpecifyStreamWithStreamTypeNumber(u32 psmfStruct, u32 streamType, u32 typeNum) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!psmf->setStreamWithTypeNumber(psmfStruct, (int)streamType, (int)typeNum)) return SCE_PSMF_ERROR_INVALID_ID;
	return 0;
}

u32 scePsmfSpecifyStream(u32 psmfStruct, int streamNum) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!psmf->setStreamNum(psmfStruct, streamNum)) {
		psmf->setStreamNum(psmfStruct, (int)SCE_PSMF_ERROR_NOT_INITIALIZED);
		return SCE_PSMF_ERROR_INVALID_ID;
	}
	return 0;
}

u32 scePsmfGetVideoInfo(u32 psmfStruct, u32 videoInfoAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!psmf->isValidCurrentStreamNumber()) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(videoInfoAddr, 8)) return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	PsmfStream *info = psmf->streamMap[psmf->currentStreamNum];
	if (info->videoWidth_ == PsmfStream::INVALID) return SCE_PSMF_ERROR_INVALID_ID;
	GWrite32(videoInfoAddr, (u32)info->videoWidth_);
	GWrite32(videoInfoAddr + 4, (u32)info->videoHeight_);
	return 0;
}

u32 scePsmfGetAudioInfo(u32 psmfStruct, u32 audioInfoAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!psmf->isValidCurrentStreamNumber()) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(audioInfoAddr, 8)) return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	PsmfStream *info = psmf->streamMap[psmf->currentStreamNum];
	if (info->audioChannels_ == PsmfStream::INVALID) return SCE_PSMF_ERROR_INVALID_ID;
	GWrite32(audioInfoAddr, (u32)info->audioChannels_);
	GWrite32(audioInfoAddr + 4, (u32)info->audioFrequency_);
	return 0;
}

u32 scePsmfGetCurrentStreamType(u32 psmfStruct, u32 typeAddr, u32 channelAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (psmf->currentStreamNum == (int)SCE_PSMF_ERROR_NOT_INITIALIZED) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidAddress(typeAddr) || !mem::IsValidAddress(channelAddr)) return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	if (psmf->currentStreamType != -1) {
		W32(typeAddr, (u32)psmf->currentStreamType);
		W32(channelAddr, (u32)psmf->currentStreamChannel);
	}
	return 0;
}

u32 scePsmfGetStreamSize(u32 psmfStruct, u32 sizeAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	W32(sizeAddr, psmf->streamSize);
	return 0;
}

u32 scePsmfQueryStreamOffset(u32 bufferAddr, u32 offsetAddr) {
	if (mem::IsValidAddress(offsetAddr) && mem::IsValidRange(bufferAddr, 12))
		W32(offsetAddr, ReadBE32(GPtr(bufferAddr + media::PSMF_STREAM_OFFSET_OFFSET)));
	return 0;
}

u32 scePsmfQueryStreamSize(u32 bufferAddr, u32 sizeAddr) {
	if (mem::IsValidAddress(sizeAddr) && mem::IsValidRange(bufferAddr, 16))
		W32(sizeAddr, ReadBE32(GPtr(bufferAddr + media::PSMF_STREAM_SIZE_OFFSET)));
	return 0;
}

u32 scePsmfGetHeaderSize(u32 psmfStruct, u32 sizeAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	W32(sizeAddr, psmf->headerSize);
	return 0;
}

u32 scePsmfGetPsmfVersion(u32 psmfStruct) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	return psmf->version;
}

void scePsmfVerifyPsmf(Ctx *c) {
	const u32 psmfAddr = c->r[4];
	u32 result = 0;
	if ((psmfAddr & 3) != 0 || !mem::IsValidRange(psmfAddr, 12)) {
		result = SCE_PSMF_ERROR_NOT_FOUND;
	} else if (GRead32(psmfAddr) != media::PSMF_MAGIC) {
		result = SCE_PSMF_ERROR_NOT_FOUND;
	} else if ((int)GRead32(psmfAddr + media::PSMF_STREAM_VERSION_OFFSET) < 0) {
		result = SCE_PSMF_ERROR_NOT_FOUND;
	} else {
		// Kurohyou 2 (the demo at least) uses an uninitialized value that is zero on the PSP; it
		// seems to be written by this function, so clear some of the stack below.
		const u32 sp = c->r[29];
		if (mem::IsValidRange(sp - 0x20, 0x20)) memset(GPtr(sp - 0x20), 0, 0x20);
	}
	c->r[2] = result;
}

u32 scePsmfGetNumberOfEPentries(u32 psmfStruct) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	return psmf->EPMapEntriesNum;
}

u32 scePsmfGetPresentationStartTime(u32 psmfStruct, u32 startTimeAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	if ((startTimeAddr & 3) == 0) W32(startTimeAddr, psmf->presentationStartTime);
	return 0;
}

u32 scePsmfGetPresentationEndTime(u32 psmfStruct, u32 endTimeAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	if ((endTimeAddr & 3) == 0) W32(endTimeAddr, psmf->presentationEndTime);
	return 0;
}

u32 scePsmfGetCurrentStreamNumber(u32 psmfStruct) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (psmf->currentStreamNum < 0) return (u32)psmf->currentStreamNum;
	return (u32)psmf->currentStreamNum;
}

u32 scePsmfCheckEPMap(u32 psmfStruct) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	return psmf->EPMap.empty() ? SCE_PSMF_ERROR_NOT_FOUND : 0;
}

void WriteEntry(u32 entryAddr, const PsmfEntry &e) {
	if (mem::IsValidRange(entryAddr, sizeof(PsmfEntry))) memcpy(GPtr(entryAddr), &e, sizeof(PsmfEntry));
}

u32 scePsmfGetEPWithId(u32 psmfStruct, int epid, u32 entryAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (epid < 0 || epid >= (int)psmf->EPMap.size()) return SCE_PSMF_ERROR_NOT_FOUND;
	WriteEntry(entryAddr, psmf->EPMap[epid]);
	return 0;
}

u32 scePsmfGetEPWithTimestamp(u32 psmfStruct, u32 ts, u32 entryAddr) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_INITIALIZED;
	if (ts < psmf->presentationStartTime) return SCE_PSMF_ERROR_NOT_FOUND;
	const int epid = psmf->FindEPWithTimestamp((int)ts);
	if (epid < 0 || epid >= (int)psmf->EPMap.size()) return SCE_PSMF_ERROR_NOT_FOUND;
	WriteEntry(entryAddr, psmf->EPMap[epid]);
	return 0;
}

u32 scePsmfGetEPidWithTimestamp(u32 psmfStruct, u32 ts) {
	Psmf *psmf = getPsmf(psmfStruct);
	if (!psmf) return SCE_PSMF_ERROR_NOT_FOUND;
	if (psmf->EPMap.empty()) return SCE_PSMF_ERROR_NOT_FOUND;
	if (ts < psmf->presentationStartTime) return SCE_PSMF_ERROR_INVALID_TIMESTAMP;
	const int epid = psmf->FindEPWithTimestamp((int)ts);
	if (epid < 0 || epid >= (int)psmf->EPMap.size()) return SCE_PSMF_ERROR_INVALID_ID;
	return (u32)epid;
}

// ---- scePsmfPlayer ----

int scePsmfPlayerCreate(u32 psmfPlayer, u32 dataPtr) {
	EnsureInit();
	audiohle::EnsureInit();
	if (!mem::IsValidRange(psmfPlayer, 4) || !mem::IsValidRange(dataPtr, sizeof(PsmfPlayerCreateData)))
		return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	PsmfPlayerCreateData data;
	memcpy(&data, GPtr(dataPtr), sizeof(data));
	if (!mem::IsValidAddress(data.buffer)) {
		// Also crashes on a PSP.
		GWrite32(psmfPlayer, 0);
		return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	}
	if (data.bufferSize < 0x00285800) {
		GWrite32(psmfPlayer, 0);
		return (int)SCE_PSMFPLAYER_ERROR_BUFFER_SIZE;
	}
	if (data.threadPriority < 0x10 || data.threadPriority >= 0x6E) {
		GWrite32(psmfPlayer, 0);
		return (int)SCE_PSMFPLAYER_ERROR_INVALID_PARAM;
	}
	if (!g_psmfPlayerMap.empty()) {
		GWrite32(psmfPlayer, 0);
		return (int)SCE_MPEG_ERROR_ALREADY_INIT;
	}

	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) {
		player = new PsmfPlayer(data);
		player->generation = ++g_playerGeneration;
		auto old = g_psmfPlayerMap.find(psmfPlayer);
		if (old != g_psmfPlayerMap.end()) delete old->second;
		g_psmfPlayerMap[psmfPlayer] = player;
		// Write something there to identify it with.
		GWrite32(psmfPlayer, psmfPlayer);
	}

	// These really shouldn't be globals, but there can only be one psmfplayer anyway.
	g_videoPixelMode = GE_CMODE_32BIT_ABGR8888;
	g_videoLoopStatus = PSMF_PLAYER_CONFIG_NO_LOOP;

	return (int)DelayWithStateChange(psmfPlayer, PSMF_PLAYER_STATUS_INIT, 20000);
}

int scePsmfPlayerStop(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_PLAYING) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	AbortFinish(player);
	return (int)DelayWithStateChange(psmfPlayer, PSMF_PLAYER_STATUS_STANDBY, 3000);
}

int scePsmfPlayerBreak(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	AbortFinish(player);
	return 0;
}

int ReadPlayerFile(PsmfPlayer *player, u8 *buf, int size) {
	if (!player->dev || player->filehandle < 0 || size <= 0) return 0;
	const s64 got = player->dev->Read(player->filehandle, buf, size);
	return got > 0 ? (int)got : 0;
}

void SeekPlayerFile(PsmfPlayer *player, s64 offset) {
	if (player->dev && player->filehandle >= 0) player->dev->Seek(player->filehandle, offset, 0);
}

int PsmfPlayerFillRingbuffer(PsmfPlayer *player) {
	if (!player->dev || player->filehandle < 0) return -1;
	u8 *buf = player->tempbuf.data();
	const int tempbufSize = (int)player->tempbuf.size();
	int size;
	// Let's not burn a bunch of time adding data all at once.
	int addMax = std::max(2048 * 100, tempbufSize);
	do {
		size = std::min(player->mediaengine->getRemainSize(), tempbufSize);
		size = std::min(player->mediaengine->getAudioRemainSize(), size);
		size = std::min(player->streamSize - player->readSize, size);
		if (size <= 0) break;
		size = ReadPlayerFile(player, buf, size);
		player->readSize += size;
		player->mediaengine->addStreamData(buf, size);
		addMax -= size;
		if (addMax <= 0) break;
	} while (size > 0);

	if (player->readSize >= player->streamSize && g_videoLoopStatus == PSMF_PLAYER_CONFIG_LOOP) {
		// Start looping, but only once we've finished.
		if (player->HasReachedEnd()) {
			player->readSize = 0;
			SeekPlayerFile(player, player->fileoffset);
			player->mediaengine->reloadStream();
		}
	}
	return 0;
}

int PsmfPlayerSetPsmfOffset(u32 psmfPlayer, const char *filename, int offset) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player || player->status != PSMF_PLAYER_STATUS_INIT) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (!filename) return (int)SCE_PSMFPLAYER_ERROR_INVALID_PARAM;

	int delayUs = 1100;
	player->CloseFile();
	io::Resolved r;
	int fh = -1;
	if (io::Resolve(filename, &r) >= 0 && r.dev && r.dev->Fs()) fh = r.dev->Open(r.path, io::PSP_O_RDONLY, 0);
	if (fh < 0) return (int)Delay(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT, delayUs);
	player->dev = r.dev;
	player->filehandle = fh;
	if (offset < 0) return (int)Delay(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT, delayUs);

	SeekPlayerFile(player, offset);
	u8 *buf = player->tempbuf.data();
	memset(buf, 0, 2048);
	const int size = ReadPlayerFile(player, buf, 2048);
	delayUs += 2000;

	const u32 magic = (u32)buf[0] | ((u32)buf[1] << 8) | ((u32)buf[2] << 16) | ((u32)buf[3] << 24);
	if (magic != media::PSMF_MAGIC) {
		// Refused on hardware (video/psmfplayer/setpsmfoffset: an offset that isn't a header).
		LOGW("PSMF", "scePsmfPlayerSetPsmf*: incorrect PSMF magic (%08x), bad data", magic);
		player->CloseFile();
		return (int)Delay(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT, delayUs);
	}

	const u16 numStreams = ReadBE16(buf + 0x80);
	if (numStreams > 128) return (int)Delay(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT, delayUs);

	player->totalVideoStreams = 0;
	player->totalAudioStreams = 0;
	player->videoWidth = buf[142] * 16;
	player->videoHeight = buf[143] * 16;
	player->playerVersion = PSMF_PLAYER_VERSION_FULL;
	for (u16 i = 0; i < numStreams; i++) {
		const u8 *currentStreamAddr = buf + 0x82 + i * 16;
		const int streamId = *currentStreamAddr;
		if ((streamId & media::PSMF_VIDEO_STREAM_ID) == media::PSMF_VIDEO_STREAM_ID) {
			++player->totalVideoStreams;
			// Without EP info for any video stream, revert to BASIC.
			const u32 epOffset = ReadBE32(currentStreamAddr + 4);
			const u32 epEntries = ReadBE32(currentStreamAddr + 8);
			if (epOffset == 0 || epEntries == 0) player->playerVersion = PSMF_PLAYER_VERSION_BASIC;
		} else if ((streamId & media::PSMF_AUDIO_STREAM_ID) == media::PSMF_AUDIO_STREAM_ID) {
			++player->totalAudioStreams;
		} else {
			LOGW("PSMF", "scePsmfPlayerSetPsmf*: unexpected streamID %x", streamId);
		}
	}

	const int mpegoffset = (int)ReadBE32(buf + media::PSMF_STREAM_OFFSET_OFFSET);
	player->readSize = size - mpegoffset;
	if (g_psmfPlayerLibVersion >= 0x05050010) {
		player->streamSize = (int)ReadBE32(buf + media::PSMF_STREAM_SIZE_OFFSET);
	} else {
		// Older versions just read until the end of the file.
		io::PspIoStat st{};
		s64 fileSize = 0;
		if (r.dev->GetStat(r.path, st) >= 0) fileSize = st.size;
		// (Capped: a PMF over 2 GB can't come from a UMD, but an image can hold one.)
		player->streamSize = (int)std::min<s64>(fileSize - offset - mpegoffset, INT_MAX);
	}
	player->fileoffset = (u32)(offset + mpegoffset);
	player->mediaengine->loadStream(buf, 2048, std::max(2048 * 500, (int)player->tempbuf.size()));
	PsmfPlayerFillRingbuffer(player);
	player->totalDurationTimestamp = (u64)player->mediaengine->getLastTimeStamp();

	return (int)DelayWithStateChange(psmfPlayer, PSMF_PLAYER_STATUS_STANDBY, delayUs);
}

int scePsmfPlayerSetPsmf(u32 psmfPlayer, const char *filename) { return PsmfPlayerSetPsmfOffset(psmfPlayer, filename, 0); }
int scePsmfPlayerSetPsmfCB(u32 psmfPlayer, const char *filename) {
	return PsmfPlayerSetPsmfOffset(psmfPlayer, filename, 0);
}
int scePsmfPlayerSetPsmfOffset(u32 psmfPlayer, const char *filename, int offset) {
	return PsmfPlayerSetPsmfOffset(psmfPlayer, filename, offset);
}
int scePsmfPlayerSetPsmfOffsetCB(u32 psmfPlayer, const char *filename, int offset) {
	return PsmfPlayerSetPsmfOffset(psmfPlayer, filename, offset);
}

int scePsmfPlayerGetAudioOutSize(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	return audioSamplesBytes;
}

bool PsmfPlayerContinueSeek(PsmfPlayer *player, int tries = 50) {
	if (player->seekDestTimeStamp <= 0) return true;
	while (!player->mediaengine->seekTo(player->seekDestTimeStamp, g_videoPixelMode)) {
		if (--tries <= 0) return false;
		PsmfPlayerFillRingbuffer(player);
		if (player->mediaengine->IsVideoEnd()) break;
	}
	// The seek is done, so forget about it.
	player->seekDestTimeStamp = 0;
	return true;
}

int scePsmfPlayerStart(u32 psmfPlayer, u32 psmfPlayerData, int initPts) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status == PSMF_PLAYER_STATUS_INIT) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (!mem::IsValidRange(psmfPlayerData, sizeof(PsmfPlayerData))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	PsmfPlayerData playerData;
	memcpy(&playerData, GPtr(psmfPlayerData), sizeof(playerData));
	if (playerData.playMode < 0 || playerData.playMode > (int)PSMF_PLAYER_MODE_REWIND)
		return (int)SCE_PSMFPLAYER_ERROR_INVALID_PARAM;
	if (initPts >= player->mediaengine->getLastTimeStamp()) return (int)SCE_PSMFPLAYER_ERROR_INVALID_PARAM;

	if (player->totalAudioStreams > 0) {
		if (playerData.audioCodec != 0x0F && playerData.audioCodec != 0x01) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
		if (playerData.audioStreamNum >= player->totalAudioStreams) return (int)SCE_PSMFPLAYER_ERROR_INVALID_CONFIG;
	}
	if (playerData.videoCodec != 0x0E && playerData.videoCodec != 0x00) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	if (playerData.videoStreamNum < 0 || playerData.videoStreamNum >= player->totalVideoStreams)
		return (int)SCE_PSMFPLAYER_ERROR_INVALID_CONFIG;

	switch ((PsmfPlayerMode)playerData.playMode) {
	case PSMF_PLAYER_MODE_FORWARD:
	case PSMF_PLAYER_MODE_REWIND:
		if (player->playerVersion == PSMF_PLAYER_VERSION_BASIC) return (int)SCE_PSMFPLAYER_ERROR_INVALID_PARAM;
		LOGW("PSMF", "scePsmfPlayerStart(%08x): unsupported playMode %d", psmfPlayer, playerData.playMode);
		break;
	case PSMF_PLAYER_MODE_PLAY:
	case PSMF_PLAYER_MODE_PAUSE: break;
	default: LOGW("PSMF", "scePsmfPlayerStart(%08x): unsupported playMode %d", psmfPlayer, playerData.playMode); break;
	}

	if (player->playerVersion == PSMF_PLAYER_VERSION_BASIC && initPts != 0) return (int)SCE_PSMFPLAYER_ERROR_INVALID_PARAM;

	AbortFinish(player);
	player->mediaengine->setVideoStream(playerData.videoStreamNum);
	player->videoCodec = playerData.videoCodec;
	player->videoStreamNum = playerData.videoStreamNum;
	if (player->totalAudioStreams > 0) {
		player->mediaengine->setAudioStream(playerData.audioStreamNum);
		player->audioCodec = playerData.audioCodec;
		player->audioStreamNum = playerData.audioStreamNum;
	}
	player->playMode = playerData.playMode;
	player->playSpeed = playerData.playSpeed;

	// Doesn't alter the current pts; Update()/etc. catch up when they get there.
	const int delayUs = player->status == PSMF_PLAYER_STATUS_PLAYING ? 3000 : 0;
	if (delayUs == 0) player->status = PSMF_PLAYER_STATUS_PLAYING;
	player->warmUp = 0;

	player->mediaengine->openContext();

	const s64 dist = initPts - player->mediaengine->getVideoTimeStamp();
	if (dist < 0 || dist > (s64)VIDEO_FRAME_DURATION_TS * 60) {
		// Seeking backwards: start populating the stream from the start.
		SeekPlayerFile(player, 0);
		u8 *buf = player->tempbuf.data();
		const int tempbufSize = (int)player->tempbuf.size();
		const int size = ReadPlayerFile(player, buf, tempbufSize);
		player->mediaengine->loadStream(buf, size, std::max(2048 * 500, tempbufSize));

		const int mpegoffset = (int)ReadBE32(buf + media::PSMF_STREAM_OFFSET_OFFSET);
		player->readSize = size - mpegoffset;

		Psmf psmf(player->tempbuf.data(), 0, (u32)tempbufSize);
		int lastOffset = 0;
		for (const PsmfEntry &e : psmf.EPMap) {
			if (initPts <= e.EPPts - (int)psmf.presentationStartTime) break;
			lastOffset = e.EPOffset;
		}
		player->readSize = lastOffset * 2048;
		SeekPlayerFile(player, (s64)player->fileoffset + player->readSize);
		PsmfPlayerFillRingbuffer(player);
	}

	player->seekDestTimeStamp = initPts;
	PsmfPlayerContinueSeek(player);
	if (delayUs == 0) return 0;
	return (int)DelayWithStateChange(psmfPlayer, PSMF_PLAYER_STATUS_PLAYING, delayUs);
}

int scePsmfPlayerDelete(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	AbortFinish(player);
	g_psmfPlayerMap.erase(GRead32(psmfPlayer));
	delete player;
	GWrite32(psmfPlayer, 0);
	return (int)Delay(0, 20000);
}

int scePsmfPlayerUpdate(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_PLAYING) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->HasReachedEnd()) {
		if (g_videoLoopStatus == PSMF_PLAYER_CONFIG_NO_LOOP && player->videoStep >= 1) {
			if (player->status != PSMF_PLAYER_STATUS_PLAYING_FINISHED) ScheduleFinish(psmfPlayer, player);
		}
	}
	player->videoStep++;
	return 0;
}

int scePsmfPlayerReleasePsmf(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_STANDBY) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	player->status = PSMF_PLAYER_STATUS_INIT;
	return 0;
}

int scePsmfPlayerGetVideoData(u32 psmfPlayer, u32 videoDataAddr) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_PLAYING) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (!mem::IsValidRange(videoDataAddr, sizeof(PsmfVideoData))) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	PsmfVideoData *videoData = (PsmfVideoData *)GPtr(videoDataAddr);
	if (videoData->frameWidth < 0) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if (videoData->frameWidth != 0 && videoData->frameWidth < player->mediaengine->VideoWidth())
		return (int)SCE_KERNEL_ERROR_INVALID_VALUE;

	sched::ChargeCycles(20000);

	if (!PsmfPlayerContinueSeek(player)) return (int)SCE_PSMFPLAYER_ERROR_NO_MORE_DATA;

	// On a real PSP this takes a variable amount of time: normally at least 3 without audio, 5
	// with, but hundreds without enough delay between. Some games expect the first few to fail.
	if (player->warmUp < PSMF_PLAYER_WARMUP_FRAMES) {
		++player->warmUp;
		return (int)SCE_PSMFPLAYER_ERROR_NO_MORE_DATA;
	}
	player->warmUp = 10000;

	// An invalid value is fine while warming up, but not after.
	if (!mem::IsValidAddress(videoData->displaybuf)) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;

	MediaEngine *me = player->mediaengine.get();
	bool doVideoStep = true;
	if (player->playMode == PSMF_PLAYER_MODE_PAUSE) {
		doVideoStep = false;
	} else if (!me->IsNoAudioData() && me->IsActuallyPlayingAudio()) {
		s64 deltapts = me->getVideoTimeStamp() - me->getAudioTimeStamp();
		// Don't skip the very first frame, sometimes audio starts with an early timestamp.
		if (deltapts > 0 && me->getVideoTimeStamp() > 0) {
			// Don't advance, just return the same frame again.
			doVideoStep = false;
		} else {
			// An approximation: it should allow a certain amount ahead before skipping frames.
			while (!me->IsVideoEnd() && deltapts <= -(VIDEO_FRAME_DURATION_TS * 5)) {
				me->stepVideo(g_videoPixelMode, true);
				deltapts = me->getVideoTimeStamp() - me->getAudioTimeStamp();
			}
		}
	} else {
		// No audio: based on Update() calls. playSpeed doesn't seem to matter.
		if (player->videoStep <= 1 && me->getVideoTimeStamp() > 0) {
			doVideoStep = false;
		} else {
			player->videoStep = 0;
		}
	}

	if (doVideoStep) me->stepVideo(g_videoPixelMode);

	// The frameWidth is rounded down to even values, and defaults to 512.
	const int bufw = videoData->frameWidth == 0 ? 512 : videoData->frameWidth & ~1;
	// Always write the video frame, even after the video has ended.
	me->writeVideoImage(videoData->displaybuf, bufw, g_videoPixelMode);
	// getVideoTimeStamp() includes the frame duration; remove it for this frame's pts.
	player->psmfPlayerAvcAu.pts = me->getVideoTimeStamp() - VIDEO_FRAME_DURATION_TS;
	videoData = (PsmfVideoData *)GPtr(videoDataAddr);
	videoData->displaypts = (u32)player->psmfPlayerAvcAu.pts;

	PsmfPlayerFillRingbuffer(player);
	return (int)Delay(0, audiohle::MEScheduleJob(3000));
}

int scePsmfPlayerGetAudioData(u32 psmfPlayer, u32 audioDataAddr) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_PLAYING) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (!mem::IsValidAddress(audioDataAddr)) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	// Don't return audio frames before we would return video frames.
	if (player->warmUp < PSMF_PLAYER_WARMUP_FRAMES) return (int)SCE_PSMFPLAYER_ERROR_NO_MORE_DATA;
	if (player->playMode == PSMF_PLAYER_MODE_PAUSE) return (int)SCE_PSMFPLAYER_ERROR_NO_MORE_DATA;

	int ret = 0;
	if (player->mediaengine->getAudioSamples(audioDataAddr) == 0) {
		if (player->totalAudioStreams > 0 &&
		    player->psmfPlayerAvcAu.pts < (s64)player->totalDurationTimestamp - VIDEO_FRAME_DURATION_TS) {
			// Write zeros for any missing trailing frames so it syncs with the video.
			mem::Memset(audioDataAddr, 0, audiohle::ClampValidSize(audioDataAddr, audioSamplesBytes));
		} else {
			ret = (int)SCE_PSMFPLAYER_ERROR_NO_MORE_DATA;
		}
	}
	sched::ChargeCycles(ret != 0 ? 10000 : 30000);
	sched::Reschedule();
	return ret;
}

int scePsmfPlayerGetCurrentStatus(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	// Mana Khemia and other games call this even when not necessary.
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status == PSMF_PLAYER_STATUS_NONE) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	return player->status;
}

u32 scePsmfPlayerGetCurrentPts(u32 psmfPlayer, u32 currentPtsAddr) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_STANDBY) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->psmfPlayerAvcAu.pts < 0) return SCE_PSMFPLAYER_ERROR_NO_MORE_DATA;
	W32(currentPtsAddr, (u32)player->psmfPlayerAvcAu.pts);
	return 0;
}

u32 scePsmfPlayerGetPsmfInfo(u32 psmfPlayer, u32 psmfInfoAddr, u32 widthAddr, u32 heightAddr) {
	if (!mem::IsValidAddress(psmfPlayer) || !mem::IsValidRange(psmfInfoAddr, sizeof(PsmfInfo)))
		return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_STANDBY) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;

	PsmfInfo *info = (PsmfInfo *)GPtr(psmfInfoAddr);
	// The first frame is at 0, so subtract one frame's duration for the last frame's timestamp.
	info->lastFrameTS = (u32)(player->totalDurationTimestamp - VIDEO_FRAME_DURATION_TS);
	info->numVideoStreams = player->totalVideoStreams;
	info->numAudioStreams = player->totalAudioStreams;
	info->numPCMStreams = 0;
	info->playerVersion = player->playerVersion;

	if (g_psmfPlayerLibVersion == 0x03090510) {
		// LocoRoco 2 sizes its video output from these.
		if (player->videoWidth) W32(widthAddr, (u32)player->videoWidth);
		if (player->videoHeight) W32(heightAddr, (u32)player->videoHeight);
	}
	return 0;
}

u32 scePsmfPlayerGetCurrentPlayMode(u32 psmfPlayer, u32 playModeAddr, u32 playSpeedAddr) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	W32(playModeAddr, (u32)player->playMode);
	W32(playSpeedAddr, (u32)player->playSpeed);
	return 0;
}

u32 scePsmfPlayerGetCurrentVideoStream(u32 psmfPlayer, u32 videoCodecAddr, u32 videoStreamNumAddr) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status == PSMF_PLAYER_STATUS_INIT) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	W32(videoCodecAddr, (u32)(player->videoCodec == 0x0E ? 0 : player->videoCodec));
	W32(videoStreamNumAddr, (u32)player->videoStreamNum);
	return 0;
}

u32 scePsmfPlayerGetCurrentAudioStream(u32 psmfPlayer, u32 audioCodecAddr, u32 audioStreamNumAddr) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status == PSMF_PLAYER_STATUS_INIT) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	W32(audioCodecAddr, (u32)(player->audioCodec == 0x0F ? 1 : player->audioCodec));
	W32(audioStreamNumAddr, (u32)player->audioStreamNum);
	return 0;
}

int scePsmfPlayerSetTempBuf(u32 psmfPlayer, u32 tempBufAddr, u32 tempBufSize) {
	(void)tempBufAddr;
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status != PSMF_PLAYER_STATUS_INIT) return (int)SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (tempBufSize < 0x00010000) return (int)SCE_PSMFPLAYER_ERROR_INVALID_PARAM;
	// Faked: we read through our own buffer.
	return 0;
}

u32 scePsmfPlayerChangePlayMode(u32 psmfPlayer, int playMode, int playSpeed) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status < PSMF_PLAYER_STATUS_PLAYING) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (playMode < 0 || playMode > (int)PSMF_PLAYER_MODE_REWIND) return SCE_PSMFPLAYER_ERROR_INVALID_CONFIG;
	switch (playMode) {
	case PSMF_PLAYER_MODE_FORWARD:
	case PSMF_PLAYER_MODE_REWIND:
		if (player->playerVersion == PSMF_PLAYER_VERSION_BASIC) return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
		player->playSpeed = playSpeed;
		break;
	default: break;
	}
	player->playMode = playMode;
	return 0;
}

u32 scePsmfPlayerSelectAudio(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status != PSMF_PLAYER_STATUS_PLAYING) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	int next = player->audioStreamNum + 1;
	if (next >= player->totalAudioStreams) next = 0;
	// Only to a stream that actually has data (video/psmfplayer/selectstream).
	if (next == player->audioStreamNum || !player->mediaengine->HasAudioStreamData(next) ||
	    !player->mediaengine->setAudioStream(next))
		return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	player->audioStreamNum = next;
	return 0;
}

u32 scePsmfPlayerSelectVideo(u32 psmfPlayer) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status != PSMF_PLAYER_STATUS_PLAYING) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	int next = player->videoStreamNum + 1;
	if (next >= player->totalVideoStreams) next = 0;
	if (next == player->videoStreamNum || !player->mediaengine->HasVideoStreamData(next) ||
	    !player->mediaengine->setVideoStream(next))
		return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	player->videoStreamNum = next;
	return 0;
}

u32 scePsmfPlayerSelectSpecificVideo(u32 psmfPlayer, int videoCodec, int videoStreamNum) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status != PSMF_PLAYER_STATUS_PLAYING) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->totalVideoStreams < 2) return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	if (videoStreamNum < 0 || videoStreamNum >= player->totalVideoStreams) return SCE_PSMFPLAYER_ERROR_INVALID_CONFIG;
	if (videoCodec != 0x0E && videoCodec != 0x00) return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	if (!player->mediaengine->setVideoStream(videoStreamNum)) return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	const bool delay = player->videoStreamNum != videoStreamNum;
	player->videoCodec = videoCodec;
	player->videoStreamNum = videoStreamNum;
	return delay ? Delay(0, 100) : 0;
}

// This function appears to be buggy in most libraries.
u32 scePsmfPlayerSelectSpecificAudio(u32 psmfPlayer, int audioCodec, int audioStreamNum) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->status != PSMF_PLAYER_STATUS_PLAYING) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	if (player->totalAudioStreams < 2) return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	if (audioStreamNum < 0 || audioStreamNum >= player->totalAudioStreams) return SCE_PSMFPLAYER_ERROR_INVALID_CONFIG;
	if (audioCodec != 0x0F && audioCodec != 0x01) return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	if (!player->mediaengine->setAudioStream(audioStreamNum)) return SCE_PSMFPLAYER_ERROR_INVALID_STREAM;
	const bool delay = player->audioStreamNum != audioStreamNum;
	player->audioCodec = audioCodec;
	player->audioStreamNum = audioStreamNum;
	return delay ? Delay(0, 100) : 0;
}

u32 scePsmfPlayerConfigPlayer(u32 psmfPlayer, int configMode, int configAttr) {
	PsmfPlayer *player = getPsmfPlayer(psmfPlayer);
	if (!player) return SCE_PSMFPLAYER_ERROR_INVALID_STATUS;
	// This one works in any status as long as it's created.
	switch (configMode) {
	case PSMF_PLAYER_CONFIG_MODE_LOOP:
		if (configAttr != 0 && configAttr != 1) return SCE_PSMFPLAYER_ERROR_INVALID_PARAM;
		g_videoLoopStatus = configAttr;
		break;
	case PSMF_PLAYER_CONFIG_MODE_PIXEL_TYPE:
		if (configAttr < -1 || configAttr > 3) return SCE_PSMFPLAYER_ERROR_INVALID_PARAM;
		// -1 seems to mean the default (8888, at least for one video).
		g_videoPixelMode = configAttr != -1 ? configAttr : GE_CMODE_32BIT_ABGR8888;
		break;
	default: return SCE_PSMFPLAYER_ERROR_INVALID_CONFIG;
	}
	return 0;
}

}  // namespace

void PsmfPlayerNotifyLoadModule(int devkitVersion) { g_psmfPlayerLibVersion = devkitVersion; }

}  // namespace mediahle

void Media_NotifyNativeModuleLoad(const std::string &name, const std::vector<u8> &data) {
	int version = 0;
	u32 devkit = 0;
	loader::PspHeader h;
	loader::ElfFile elf;
	if (loader::ParsePspHeader(data.data(), data.size(), h)) {
		version = (h.moduleVerHi << 8) | h.moduleVerLo;
		devkit = h.devkitVersion;
	} else if (elf.Parse(data.data(), data.size(), nullptr) && elf.HasModuleInfo()) {
		version = elf.ModuleInfo().version[0] | (elf.ModuleInfo().version[1] << 8);
	} else {
		return;
	}
	if (name == "sceMpeg_library") mediahle::MpegNotifyLoadModule(version);
	// The library version sets how many atrac contexts exist (4 up to 0x101, else 6). A native
	// placeholder has no bss of its own, so the contexts stay in kernel memory.
	if (name == "sceATRAC3plus_Library") audiohle::AtracNotifyLoadModule(version, 0, 0, 0);
	if (name == "scePsmfP_library" || name == "scePsmfPlayer" || name == "libpsmfplayer" || name == "psmf_jk" ||
	    name == "jkPsmfP_library")
		mediahle::PsmfPlayerNotifyLoadModule((int)devkit);
}

namespace mediahle {

void Register_scePsmf() {
	static const hle::HleFunction funcs[] = {
	    HLEFN(0xC22C8327, scePsmfSetPsmf),
	    HLEFN(0xC7DB3A5B, scePsmfGetCurrentStreamType),
	    HLEFN(0x28240568, scePsmfGetCurrentStreamNumber),
	    HLEFN(0x1E6D9013, scePsmfSpecifyStreamWithStreamType),
	    HLEFN(0x0C120E1D, scePsmfSpecifyStreamWithStreamTypeNumber),
	    HLEFN(0x4BC9BDE0, scePsmfSpecifyStream),
	    HLEFN(0x76D3AEBA, scePsmfGetPresentationStartTime),
	    HLEFN(0xBD8AE0D8, scePsmfGetPresentationEndTime),
	    HLEFN(0xEAED89CD, scePsmfGetNumberOfStreams),
	    HLEFN(0x7491C438, scePsmfGetNumberOfEPentries),
	    HLEFN(0x0BA514E5, scePsmfGetVideoInfo),
	    HLEFN(0xA83F7113, scePsmfGetAudioInfo),
	    HLEFN(0x971A3A90, scePsmfCheckEPMap),
	    HLEFN(0x68D42328, scePsmfGetNumberOfSpecificStreams),
	    HLEFN(0x5B70FCC1, scePsmfQueryStreamOffset),
	    HLEFN(0x9553CC91, scePsmfQueryStreamSize),
	    HLEFN(0xB78EB9E9, scePsmfGetHeaderSize),
	    HLEFN(0xA5EBFE81, scePsmfGetStreamSize),
	    HLEFN(0xE1283895, scePsmfGetPsmfVersion),
	    HLERAW(0x2673646B, scePsmfVerifyPsmf),
	    HLEFN(0x4E624A34, scePsmfGetEPWithId),
	    HLEFN(0x7C0E7AC3, scePsmfGetEPWithTimestamp),
	    HLEFN(0x5F457515, scePsmfGetEPidWithTimestamp),
	    HLESTUB(0x43AC7DBB, "scePsmfGetPsmfMark"),
	    HLESTUB(0xDE78E9FC, "scePsmfGetNumberOfPsmfMarks"),
	};
	hle::RegisterModule("scePsmf", funcs);
}

void Register_scePsmfPlayer() {
	static const hle::HleFunction funcs[] = {
	    HLEFN(0x235D8787, scePsmfPlayerCreate),
	    HLEFN(0x1078C008, scePsmfPlayerStop),
	    HLEFN(0x1E57A8E7, scePsmfPlayerConfigPlayer),
	    HLEFN(0x2BEB1569, scePsmfPlayerBreak),
	    HLEFN(0x3D6D25A9, scePsmfPlayerSetPsmf),
	    HLEFN(0x58B83577, scePsmfPlayerSetPsmfCB),
	    HLEFN(0x3EA82A4B, scePsmfPlayerGetAudioOutSize),
	    HLEFN(0x3ED62233, scePsmfPlayerGetCurrentPts),
	    HLEFN(0x46F61F8B, scePsmfPlayerGetVideoData),
	    HLEFN(0x68F07175, scePsmfPlayerGetCurrentAudioStream),
	    HLEFN(0x75F03FA2, scePsmfPlayerSelectSpecificVideo),
	    HLEFN(0x85461EFF, scePsmfPlayerSelectSpecificAudio),
	    HLEFN(0x8A9EBDCD, scePsmfPlayerSelectVideo),
	    HLEFN(0x95A84EE5, scePsmfPlayerStart),
	    HLEFN(0x9B71A274, scePsmfPlayerDelete),
	    HLEFN(0x9FF2B2E7, scePsmfPlayerGetCurrentVideoStream),
	    HLEFN(0xA0B8CA55, scePsmfPlayerUpdate),
	    HLEFN(0xA3D81169, scePsmfPlayerChangePlayMode),
	    HLEFN(0xB8D10C56, scePsmfPlayerSelectAudio),
	    HLEFN(0xB9848A74, scePsmfPlayerGetAudioData),
	    HLEFN(0xDF089680, scePsmfPlayerGetPsmfInfo),
	    HLEFN(0xE792CD94, scePsmfPlayerReleasePsmf),
	    HLEFN(0xF3EFAA91, scePsmfPlayerGetCurrentPlayMode),
	    HLEFN(0xF8EF08A6, scePsmfPlayerGetCurrentStatus),
	    HLEFN(0x2D0E4E0A, scePsmfPlayerSetTempBuf),
	    HLEFN(0x76C0F4AE, scePsmfPlayerSetPsmfOffset),
	    HLEFN(0xA72DB4F9, scePsmfPlayerSetPsmfOffsetCB),
	    HLESTUB(0x340C12CB, "scePsmfPlayer_340C12CB"),
	};
	hle::RegisterModule("scePsmfPlayer", funcs);
}

}  // namespace mediahle
