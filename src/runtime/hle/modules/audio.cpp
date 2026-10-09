// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceAudio, sceAudio_driver and sceVaudio: the eight mixer channels, the Output2/SRC channel
// (which sceVaudio shares), and the 44.1kHz stereo mixer that feeds the host.
//
// Ported from PPSSPP's Core/HLE/sceAudio.cpp, __sceAudio.cpp and sceVaudio.cpp
// (https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later);
// see PPSSPP's docs/sceAudio.md for the hardware behaviour this models. In short: every mixer
// channel holds exactly one buffer, played straight out of guest memory 64 samples at a time,
// and has room for one parked thread; the SRC channel holds two buffers. A caller that finds no
// room is told SCE_ERROR_AUDIO_CHANNEL_BUSY.
//
// Register_sceAudio() also registers every other audio library (see audio_hle.h).
#include <algorithm>
#include <chrono>
#include <vector>

#include "audio/guest_memory.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/host/host.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"
#include "runtime/kernel/waitqueue.h"



using namespace kernel;

namespace audiohle {

namespace {

constexpr u32 PSP_AUDIO_CHANNEL_MAX = 8;
constexpr u32 PSP_AUDIO_SAMPLE_MAX = 65536 - 64;
constexpr u32 PSP_AUDIO_FORMAT_STEREO = 0;
constexpr u32 PSP_AUDIO_FORMAT_MONO = 0x10;
constexpr u32 PSP_AUDIO_ERROR_SRC_FORMAT_4 = 0x80000003;
// Mixer channels wait on index + 1; the SRC channel on the id after them.
constexpr SceUID PSP_AUDIO_SRC_WAIT_ID = PSP_AUDIO_CHANNEL_MAX + 1;
constexpr int kHwBlock = 64;
constexpr int kHwRate = 44100;

struct AudioChannel {
	int index = 0;
	bool reserved = false;
	u32 sampleCount = 0;
	u32 leftVolume = 0;
	u32 rightVolume = 0;
	u32 format = 0;
	// sampleAddress walks forward as the mixer consumes the buffer and drops to zero when it runs
	// out. An output with a null pointer sets remainingSamples but leaves sampleAddress at zero.
	u32 sampleAddress = 0;
	u32 remainingSamples = 0;
	// The one parked thread, and what it wanted to hand over.
	SceUID waitingThread = 0;
	u32 waitingAddress = 0;
	int waitingLeftVolume = 0;
	int waitingRightVolume = 0;

	void clear() {
		reserved = false;
		leftVolume = rightVolume = 0;
		format = 0;
		sampleCount = 0;
		sampleAddress = 0;
		remainingSamples = 0;
		waitingThread = 0;
		waitingAddress = 0;
		waitingLeftVolume = waitingRightVolume = 0;
	}
};

struct PendingBuffer {
	u32 address = 0;
	u32 samples = 0;
};

struct SrcWaiter {
	SceUID thread;
	u32 result;
};

// Channel 8: Output2, SRC and Vaudio. Two DMA descriptors, no mixer.
struct AudioSRCChannel {
	bool reserved = false;
	u32 sampleCount = 0;
	u32 leftVolume = 0;
	u32 rightVolume = 0;
	u32 format = 0;
	PendingBuffer buffers[2];
	int bufferCount = 0;
	u32 playedSamples = 0;
	u32 frac = 0;
	// One completion can sit unclaimed, so the first output after an idle stretch doesn't block.
	bool completion = false;
	std::vector<SrcWaiter> waitingThreads;

	bool Full() const { return bufferCount >= 2; }
	void clear() {
		reserved = false;
		leftVolume = rightVolume = 0;
		format = 0;
		sampleCount = 0;
		bufferCount = 0;
		playedSamples = 0;
		frac = 0;
		completion = false;
		waitingThreads.clear();
		for (PendingBuffer &b : buffers) b = PendingBuffer{};
	}
};

AudioChannel g_chans[PSP_AUDIO_CHANNEL_MAX];
AudioSRCChannel g_src;
int g_mixFrequency = 44100;
int g_srcFrequency = 0;
int g_routingMode = 1;
int g_routingVolMode = 1;
bool g_vaudioReserved = false;

bool g_inited = false;
u64 g_mixBaseUs = 0;
u64 g_mixIndex = 0;
u64 g_mixEventId = 0;
bool g_mixerDMABusy = false;
bool g_audioMixing = false;
bool g_mixingInSyscall = false;
bool g_rescheduleAfterCall = false;
s64 g_meBusyUntilUs = 0;

s32 g_mixBuffer[kHwBlock * 2];
std::vector<s16> g_hostBuffer;
u64 g_mixedBlocks = 0;

// ---- Timing ----

u64 MixTime(u64 index) { return g_mixBaseUs + (index * 64000000ull + kHwRate / 2) / kHwRate; }

void AudioUpdate();

void OnMixEvent() {
	g_mixEventId = 0;
	g_mixIndex++;
	g_mixEventId = sched::ScheduleEvent(MixTime(g_mixIndex + 1), OnMixEvent);
	AudioUpdate();
}

// Re-phases the periodic mix to start now. The interval, so the sample rate, is unchanged.
void RephaseMixer() {
	if (g_mixEventId) sched::CancelEvent(g_mixEventId);
	g_mixBaseUs = sched::NowUs();
	g_mixIndex = 0;
	g_mixEventId = sched::ScheduleEvent(MixTime(1), OnMixEvent);
}

PspThread *WaitingThread(SceUID uid, SceUID waitId) {
	PspThread *t = Get<PspThread>(uid);
	if (t && WaitQueue::IsWaitingOn(t, WAITTYPE_AUDIOCHANNEL, waitId)) return t;
	return nullptr;
}

// Wakes from inside a syscall must not switch threads until the call has returned.
void RescheduleAfterWake() {
	if (g_mixingInSyscall) g_rescheduleAfterCall = true;
}

void FinishCall() {
	if (g_rescheduleAfterCall) {
		g_rescheduleAfterCall = false;
		sched::Reschedule();
	}
}

inline int ApplyChannelVolume(int sample, int vol) { return (int)(((s64)sample * vol) >> 15); }

bool AnyChannelPlaying() {
	for (const AudioChannel &c : g_chans) {
		if (c.sampleAddress != 0) return true;
	}
	return false;
}

// The driver starts the mixer DMA when the first buffer arrives and its mixer thread, which
// outranks the caller, fills a block right away: the first 64 samples are gone before the call
// returns.
void StartMixerDMA() {
	if (g_audioMixing) return;
	EatUs(112);
	if (g_mixEventId) sched::CancelEvent(g_mixEventId);
	g_mixingInSyscall = true;
	AudioUpdate();
	g_mixingInSyscall = false;
	g_mixBaseUs = sched::NowUs();
	g_mixIndex = 0;
	g_mixEventId = sched::ScheduleEvent(MixTime(1), OnMixEvent);
}

// The SRC DMA feeds the codec directly; nothing is read early, only the phase moves.
void StartSRCDMA() {
	if (g_audioMixing || AnyChannelPlaying()) return;
	RephaseMixer();
}

u32 Enqueue(AudioChannel &chan, u32 samplePtr, int leftVol, int rightVol) {
	if (!chan.reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_INIT;
	if (chan.sampleAddress != 0) return SCE_ERROR_AUDIO_CHANNEL_BUSY;
	chan.remainingSamples = chan.sampleCount;
	if (leftVol >= 0) chan.leftVolume = (u32)leftVol;
	if (rightVol >= 0) chan.rightVolume = (u32)rightVol;
	const bool startsDMA = samplePtr != 0 && !AnyChannelPlaying() && !g_mixerDMABusy;
	chan.sampleAddress = samplePtr;
	if (startsDMA) StartMixerDMA();
	return chan.sampleCount;
}

void EnqueueOneshot(AudioChannel &chan, u32 samplePtr, u32 sampleCount, u32 format, int leftVol, int rightVol) {
	chan.format = format;
	chan.leftVolume = (u32)leftVol;
	chan.rightVolume = (u32)rightVol;
	chan.remainingSamples = sampleCount;
	const bool startsDMA = samplePtr != 0 && !AnyChannelPlaying() && !g_mixerDMABusy;
	chan.sampleAddress = samplePtr;
	if (startsDMA) StartMixerDMA();
}

u32 EnqueueBlocking(AudioChannel &chan, u32 samplePtr, int leftVol, int rightVol) {
	u32 result = Enqueue(chan, samplePtr, leftVol, rightVol);
	if (result != SCE_ERROR_AUDIO_CHANNEL_BUSY) return result;
	// One "a thread is waiting" flag per channel: the second thread is turned away.
	if (chan.waitingThread != 0) return SCE_ERROR_AUDIO_CHANNEL_BUSY;
	if (sched::InInterrupt()) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!sched::DispatchEnabled()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	PspThread *t = sched::Current();
	if (!t) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	chan.waitingThread = t->uid;
	chan.waitingAddress = samplePtr;
	chan.waitingLeftVolume = leftVol;
	chan.waitingRightVolume = rightVol;
	// ChannelFinished retries the enqueue and supplies the real return value.
	return (u32)sched::Wait(WAITTYPE_AUDIOCHANNEL, (SceUID)chan.index + 1, 0, false);
}

// The buffer ran out.
bool ChannelFinished(AudioChannel &chan) {
	chan.sampleAddress = 0;
	chan.remainingSamples = 0;
	if (chan.waitingThread == 0) return false;
	const SceUID threadID = chan.waitingThread;
	chan.waitingThread = 0;
	PspThread *t = WaitingThread(threadID, (SceUID)chan.index + 1);
	if (!t) return false;
	sched::Wake(t, (s32)Enqueue(chan, chan.waitingAddress, chan.waitingLeftVolume, chan.waitingRightVolume));
	return true;
}

int SRCCallUs(const AudioSRCChannel &chan) { return chan.Full() ? 45 : 112; }

// The non-waiting half of an SRC output. Sets *mustWait when the caller has to park.
u32 SRCEnqueue(AudioSRCChannel &chan, u32 samplePtr, int vol, bool *mustWait) {
	*mustWait = false;
	if (!chan.reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	if (chan.Full()) return SCE_ERROR_AUDIO_CHANNEL_BUSY;
	u32 result = 0;
	if (samplePtr != 0) {
		if (vol >= 0) {
			chan.leftVolume = (u32)vol;
			chan.rightVolume = (u32)vol;
		}
		const bool wasIdle = chan.bufferCount == 0;
		if (wasIdle) {
			// Starting the DMA signals a completion by itself.
			chan.completion = true;
			chan.playedSamples = 0;
			chan.frac = 0;
		}
		chan.buffers[chan.bufferCount].address = samplePtr;
		chan.buffers[chan.bufferCount].samples = chan.sampleCount;
		chan.bufferCount++;
		result = chan.sampleCount;
		if (wasIdle) StartSRCDMA();
	} else if (chan.bufferCount == 0) {
		return 0;
	}
	// The event flag wait refuses before looking at the flag.
	if (sched::InInterrupt()) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!sched::DispatchEnabled()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (chan.completion) {
		chan.completion = false;
		return result;
	}
	*mustWait = true;
	return result;
}

u32 SRCWait(AudioSRCChannel &chan, u32 result) {
	PspThread *t = sched::Current();
	if (!t) return result;
	chan.waitingThreads.push_back(SrcWaiter{t->uid, result});
	return (u32)sched::Wait(WAITTYPE_AUDIOCHANNEL, PSP_AUDIO_SRC_WAIT_ID, 0, false);
}

u32 SRCEnqueueBlocking(AudioSRCChannel &chan, u32 samplePtr, int vol) {
	bool mustWait;
	u32 result = SRCEnqueue(chan, samplePtr, vol, &mustWait);
	if (!mustWait) return result;
	return SRCWait(chan, result);
}

void SRCSignal(AudioSRCChannel &chan) { chan.completion = true; }

bool SRCCompleted(AudioSRCChannel &chan) {
	while (!chan.waitingThreads.empty()) {
		SrcWaiter w = chan.waitingThreads.front();
		chan.waitingThreads.erase(chan.waitingThreads.begin());
		if (PspThread *t = WaitingThread(w.thread, PSP_AUDIO_SRC_WAIT_ID)) {
			sched::Wake(t, (s32)w.result);
			return true;
		}
	}
	chan.completion = true;
	return false;
}

void WakeSRCThreads(AudioSRCChannel &chan, int result) {
	bool woke = false;
	for (const SrcWaiter &w : chan.waitingThreads) {
		if (PspThread *t = WaitingThread(w.thread, PSP_AUDIO_SRC_WAIT_ID)) {
			sched::Wake(t, result);
			woke = true;
		}
	}
	chan.waitingThreads.clear();
	if (woke) g_rescheduleAfterCall = true;
}

void SRCReset(AudioSRCChannel &chan) {
	WakeSRCThreads(chan, (int)SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED);
	chan.clear();
}

bool MixChannel(AudioChannel &chan) {
	if (chan.sampleAddress == 0) return false;
	if (chan.remainingSamples == 0) return ChannelFinished(chan);
	const u32 count = std::min(chan.remainingSamples, (u32)kHwBlock);
	const bool mono = chan.format == PSP_AUDIO_FORMAT_MONO;
	const u32 stride = mono ? 2 : 4;
	if (const s16 *src = (const s16 *)mem::GetPointer(chan.sampleAddress, count * stride)) {
		const int lv = (int)chan.leftVolume, rv = (int)chan.rightVolume;
		if (mono) {
			for (u32 s = 0; s < count; s++) {
				const int sample = src[s];
				g_mixBuffer[s * 2] += ApplyChannelVolume(sample, lv);
				g_mixBuffer[s * 2 + 1] += ApplyChannelVolume(sample, rv);
			}
		} else {
			for (u32 s = 0; s < count; s++) {
				g_mixBuffer[s * 2] += ApplyChannelVolume(src[s * 2], lv);
				g_mixBuffer[s * 2 + 1] += ApplyChannelVolume(src[s * 2 + 1], rv);
			}
		}
	}
	chan.sampleAddress += count * stride;
	chan.remainingSamples -= count;
	if (chan.remainingSamples == 0) return ChannelFinished(chan);
	return false;
}

bool MixSRC(AudioSRCChannel &chan) {
	if (chan.bufferCount == 0) return false;
	const int inRate = g_srcFrequency != 0 ? g_srcFrequency : g_mixFrequency;
	const u32 ratio = (u32)(((u64)(u32)inRate << 16) / (u32)g_mixFrequency);
	const bool resampling = ratio != 0x10000;
	const bool mono = chan.format == PSP_AUDIO_FORMAT_MONO;
	const u32 stride = mono ? 2 : 4;
	const int lv = (int)chan.leftVolume, rv = (int)chan.rightVolume;
	bool woke = false;
	for (int out = 0; out < kHwBlock; out++) {
		if (chan.bufferCount == 0) break;
		const PendingBuffer &buf = chan.buffers[0];
		const u32 addr = buf.address + chan.playedSamples * stride;
		if (const s16 *src = (const s16 *)mem::GetPointer(addr, stride)) {
			const int l0 = src[0];
			const int r0 = mono ? l0 : src[1];
			int l1 = l0, r1 = r0;
			if (resampling) {
				// The codec reads the two descriptors as one stream.
				u32 nextAddr = addr + stride;
				if (chan.playedSamples + 1 >= buf.samples) nextAddr = chan.bufferCount > 1 ? chan.buffers[1].address : 0;
				if (nextAddr != 0) {
					if (const s16 *next = (const s16 *)mem::GetPointer(nextAddr, stride)) {
						l1 = next[0];
						r1 = mono ? l1 : next[1];
					}
				}
			}
			const int fr = (int)(chan.frac >> 1);
			g_mixBuffer[out * 2] += ApplyChannelVolume(l0 + (((l1 - l0) * fr) >> 15), lv);
			g_mixBuffer[out * 2 + 1] += ApplyChannelVolume(r0 + (((r1 - r0) * fr) >> 15), rv);
		}
		chan.frac += ratio;
		u32 step = chan.frac >> 16;
		chan.frac &= 0xFFFF;
		while (step > 0 && chan.bufferCount > 0) {
			const u32 take = std::min(step, chan.buffers[0].samples - chan.playedSamples);
			chan.playedSamples += take;
			step -= take;
			if (chan.playedSamples >= chan.buffers[0].samples) {
				chan.buffers[0] = chan.buffers[1];
				chan.buffers[1] = PendingBuffer{};
				chan.bufferCount--;
				chan.playedSamples = 0;
				woke |= SRCCompleted(chan);
			}
		}
	}
	return woke;
}

void SubmitToHost() {
	if (host::IsHeadless()) return;
	const size_t frames = g_hostBuffer.size() / 2;
	if (frames < 512) return;
	// A dry device queue means we underran (or are just starting): put a little slack in front
	// so scheduling jitter doesn't turn into clicks.
	if (host::AudioQueuedFrames() == 0) {
		std::vector<s16> silence(1024 * 2, 0);
		host::AudioSubmit(silence.data(), 1024);
	}
	host::AudioSubmit(g_hostBuffer.data(), (int)frames);
	g_hostBuffer.clear();

	// Rate check for debugging: what actually reached the device, against the wall clock.
	static u64 statFrames = 0;
	static auto statStart = std::chrono::steady_clock::now();
	statFrames += frames;
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - statStart).count();
	if (secs >= 5.0) {
		LOGD("AUDIO", "host output: %llu frames in %.2fs = %.0f Hz, %d frames queued", (unsigned long long)statFrames, secs,
		     statFrames / secs, host::AudioQueuedFrames());
		statFrames = 0;
		statStart = std::chrono::steady_clock::now();
	}
}

void AudioUpdate() {
	memset(g_mixBuffer, 0, sizeof(g_mixBuffer));
	g_audioMixing = true;
	g_mixerDMABusy = AnyChannelPlaying();
	bool woke = false;
	// Not gated on `reserved`: a released channel's buffer keeps playing out.
	for (AudioChannel &chan : g_chans) woke |= MixChannel(chan);
	woke |= MixSRC(g_src);
	g_audioMixing = false;
	if (woke) RescheduleAfterWake();
	g_mixedBlocks++;

	if (!host::IsHeadless()) {
		const size_t old = g_hostBuffer.size();
		g_hostBuffer.resize(old + kHwBlock * 2);
		for (int i = 0; i < kHwBlock * 2; i++) g_hostBuffer[old + i] = audio::ClampS16(g_mixBuffer[i]);
		SubmitToHost();
	}
}

u8 *GuestPtrAccessor(u32 addr, u32 size) { return mem::GetPointer(addr, size); }

bool SRCFrequencyAllowed(int freq) {
	switch (freq) {
	case 44100: case 22050: case 11025: case 48000: case 32000: case 24000: case 16000: case 12000: case 8000: return true;
	default: return false;
	}
}

int GetFreeChannel() {
	// Downwards from 7; free means released and finished playing.
	for (int i = (int)PSP_AUDIO_CHANNEL_MAX - 1; i >= 0; --i) {
		if (g_chans[i].sampleCount == 0 && g_chans[i].sampleAddress == 0) return i;
	}
	return -1;
}

// ---- sceAudio ----

u32 sceAudioOutputBlocking(u32 chan, int vol, u32 samplePtr) {
	EnsureInit();
	if (vol > 0xFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	u32 r = EnqueueBlocking(g_chans[chan], samplePtr, vol, vol);
	FinishCall();
	return r;
}

u32 sceAudioOutputPannedBlocking(u32 chan, int leftvol, int rightvol, u32 samplePtr) {
	EnsureInit();
	// The two volumes are ORed before comparing, so a negative one fails here.
	if ((u32)(leftvol | rightvol) > 0xFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	u32 r = EnqueueBlocking(g_chans[chan], samplePtr, leftvol, rightvol);
	FinishCall();
	return r;
}

u32 sceAudioOutput(u32 chan, int vol, u32 samplePtr) {
	EnsureInit();
	if (vol > 0xFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	u32 r = Enqueue(g_chans[chan], samplePtr, vol, vol);
	FinishCall();
	return r;
}

u32 sceAudioOutputPanned(u32 chan, int leftvol, int rightvol, u32 samplePtr) {
	EnsureInit();
	if (leftvol > 0xFFFF || rightvol > 0xFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	u32 r = Enqueue(g_chans[chan], samplePtr, leftvol, rightvol);
	FinishCall();
	return r;
}

int sceAudioGetChannelRestLen(u32 chan) {
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return (int)SCE_ERROR_AUDIO_INVALID_CHANNEL;
	const AudioChannel &c = g_chans[chan];
	int rest = (int)c.remainingSamples;
	if (c.waitingThread != 0) rest += (int)c.sampleCount;
	return rest;
}

int sceAudioGetChannelRestLength(u32 chan) {
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return (int)SCE_ERROR_AUDIO_INVALID_CHANNEL;
	const AudioChannel &c = g_chans[chan];
	int rest = c.sampleAddress != 0 ? (int)c.remainingSamples : 0;
	if (c.waitingThread != 0) rest += (int)c.sampleCount;
	return rest;
}

u32 sceAudioChReserve(int chan, u32 sampleCount, u32 format) {
	EnsureInit();
	if (chan < 0) {
		chan = GetFreeChannel();
		if (chan < 0) return SCE_ERROR_AUDIO_NO_CHANNELS_AVAILABLE;
	}
	if ((u32)chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	if (g_chans[chan].reserved) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	if ((sampleCount & 63) != 0 || sampleCount == 0 || sampleCount > PSP_AUDIO_SAMPLE_MAX)
		return SCE_ERROR_AUDIO_OUTPUT_SAMPLE_DATA_SIZE_NOT_ALIGNED;
	if (format != PSP_AUDIO_FORMAT_MONO && format != PSP_AUDIO_FORMAT_STEREO) return SCE_ERROR_AUDIO_INVALID_FORMAT;
	AudioChannel &c = g_chans[chan];
	c.sampleCount = sampleCount;
	c.format = format;
	c.reserved = true;
	c.leftVolume = 0;
	c.rightVolume = 0;
	return (u32)chan;
}

u32 sceAudioChRelease(u32 chan) {
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	if (!g_chans[chan].reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	if (g_chans[chan].waitingThread != 0) return SCE_ERROR_AUDIO_CHANNEL_BUSY;
	// Only the reservation goes; a buffer in flight plays out.
	g_chans[chan].reserved = false;
	g_chans[chan].sampleCount = 0;
	return 0;
}

u32 sceAudioSetChannelDataLen(u32 chan, u32 len) {
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	if ((len & 63) != 0 || len == 0 || len > PSP_AUDIO_SAMPLE_MAX) return SCE_ERROR_AUDIO_OUTPUT_SAMPLE_DATA_SIZE_NOT_ALIGNED;
	if (g_chans[chan].waitingThread != 0) return SCE_ERROR_AUDIO_CHANNEL_BUSY;
	if (!g_chans[chan].reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_INIT;
	g_chans[chan].sampleCount = len;
	return 0;
}

u32 sceAudioChangeChannelConfig(u32 chan, u32 format) {
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	if (g_chans[chan].waitingThread != 0 || g_chans[chan].sampleAddress != 0) return SCE_ERROR_AUDIO_CHANNEL_BUSY;
	if (!g_chans[chan].reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	if (format != PSP_AUDIO_FORMAT_MONO && format != PSP_AUDIO_FORMAT_STEREO) return SCE_ERROR_AUDIO_INVALID_FORMAT;
	g_chans[chan].format = format;
	return 0;
}

u32 sceAudioOneshotOutput(int chan, int sampleCount, int format, int leftvol, int rightvol, u32 samplePtr) {
	EnsureInit();
	if ((u32)leftvol > 0xFFFF || (u32)rightvol > 0xFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	if (chan < 0) {
		chan = GetFreeChannel();
		if (chan < 0) return SCE_ERROR_AUDIO_NO_CHANNELS_AVAILABLE;
	} else if (chan >= (int)PSP_AUDIO_CHANNEL_MAX || g_chans[chan].reserved) {
		return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	}
	if (sampleCount <= 0) return SCE_ERROR_AUDIO_OUTPUT_SAMPLE_DATA_SIZE_NOT_ALIGNED;
	if (format != (int)PSP_AUDIO_FORMAT_MONO && format != (int)PSP_AUDIO_FORMAT_STEREO) return SCE_ERROR_AUDIO_INVALID_FORMAT;
	EnqueueOneshot(g_chans[chan], samplePtr, (u32)sampleCount, (u32)format, leftvol, rightvol);
	FinishCall();
	return (u32)chan;
}

u32 sceAudioChangeChannelVolume(u32 chan, int leftvol, int rightvol) {
	if (leftvol > 0xFFFF || rightvol > 0xFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	if (chan >= PSP_AUDIO_CHANNEL_MAX) return SCE_ERROR_AUDIO_INVALID_CHANNEL;
	// No reservation check; a negative volume leaves that side alone.
	if (leftvol >= 0) g_chans[chan].leftVolume = (u32)leftvol;
	if (rightvol >= 0) g_chans[chan].rightVolume = (u32)rightvol;
	return 0;
}

u32 sceAudioInit() { return 0; }
u32 sceAudioEnd() { return 0; }

u32 sceAudioOutput2Reserve(u32 sampleCount) {
	EnsureInit();
	sampleCount &= 0x7FFFFFFF;
	if (sampleCount < 17 || sampleCount > 4111) return SCE_KERNEL_ERROR_INVALID_SIZE;
	if (g_src.reserved) return SCE_ERROR_AUDIO_CHANNEL_ALREADY_RESERVED;
	g_src.clear();
	g_src.sampleCount = sampleCount;
	g_src.format = PSP_AUDIO_FORMAT_STEREO;
	g_src.reserved = true;
	g_srcFrequency = 0;
	return 0;
}

u32 sceAudioOutput2OutputBlocking(u32 vol, u32 dataPtr) {
	EnsureInit();
	// 0xFFFFF, not 0xFFFF.
	if (vol > 0xFFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	EatUs(SRCCallUs(g_src));
	u32 r = SRCEnqueueBlocking(g_src, dataPtr, (int)vol);
	FinishCall();
	return r;
}

u32 sceAudioOutput2ChangeLength(u32 sampleCount) {
	if (sampleCount - 17 >= 0xFFF) return SCE_ERROR_AUDIO_OUTPUT_SAMPLE_DATA_SIZE_NOT_ALIGNED;
	if (!g_src.reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	g_src.sampleCount = sampleCount;
	return 0;
}

u32 sceAudioOutput2GetRestSample() {
	if (!g_src.reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	return (u32)g_src.bufferCount * g_src.sampleCount;
}

u32 sceAudioOutput2Release() {
	if (!g_src.reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	if (g_src.bufferCount != 0) return SCE_ERROR_AUDIO_CHANNEL_ALREADY_RESERVED;
	SRCReset(g_src);
	SRCSignal(g_src);
	FinishCall();
	return 0;
}

u32 sceAudioSetFrequency(u32 freq) {
	if (freq == 44100 || freq == 48000) {
		g_mixFrequency = (int)freq;
		return 0;
	}
	return SCE_ERROR_AUDIO_INVALID_FREQUENCY;
}

u32 sceAudioSetVolumeOffset() { return 0; }

u32 sceAudioSRCChReserve(u32 sampleCount, u32 freq, u32 format) {
	EnsureInit();
	sampleCount &= 0x7FFFFFFF;
	if (format == 4) return PSP_AUDIO_ERROR_SRC_FORMAT_4;
	if (format != 2) return SCE_KERNEL_ERROR_INVALID_SIZE;
	if (sampleCount < 17 || sampleCount > 4111) return SCE_KERNEL_ERROR_INVALID_SIZE;
	if (freq != 0 && !SRCFrequencyAllowed((int)freq)) return SCE_ERROR_AUDIO_INVALID_FREQUENCY;
	if (g_src.reserved) return SCE_ERROR_AUDIO_CHANNEL_ALREADY_RESERVED;
	g_src.clear();
	g_src.reserved = true;
	g_src.sampleCount = sampleCount;
	g_src.format = PSP_AUDIO_FORMAT_STEREO;
	g_srcFrequency = (int)freq;
	return 0;
}

u32 sceAudioSRCChRelease() {
	if (!g_src.reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	if (g_src.bufferCount != 0) return SCE_ERROR_AUDIO_CHANNEL_ALREADY_RESERVED;
	SRCReset(g_src);
	SRCSignal(g_src);
	FinishCall();
	return 0;
}

u32 sceAudioSRCOutputBlocking(u32 vol, u32 buf) {
	EnsureInit();
	if (vol > 0xFFFFF) return SCE_ERROR_AUDIO_INVALID_VOLUME;
	EatUs(SRCCallUs(g_src));
	u32 r = SRCEnqueueBlocking(g_src, buf, (int)vol);
	FinishCall();
	return r;
}

// Microphone input: there is no microphone. Report silence of the asked length.
int sceAudioInputBlocking(u32 maxSamples, u32 sampleRate, u32 bufAddr) {
	if (!mem::IsValidAddress(bufAddr)) return -1;
	if (u8 *p = mem::GetPointer(bufAddr, maxSamples * 2)) memset(p, 0, maxSamples * 2);
	const int rate = sampleRate ? (int)sampleRate : 44100;
	DelayResult(0, (int)((u64)maxSamples * 1000000 / rate));
	return (int)maxSamples;
}

int sceAudioInput(u32 maxSamples, u32 sampleRate, u32 bufAddr) {
	if (!mem::IsValidAddress(bufAddr)) return -1;
	if (u8 *p = mem::GetPointer(bufAddr, maxSamples * 2)) memset(p, 0, maxSamples * 2);
	return 0;
}

int sceAudioInputInit(int, int, int) { return 0; }
int sceAudioInputInitEx(u32) { return 0; }
int sceAudioPollInputEnd() { return 0; }
int sceAudioWaitInputEnd() { return 0; }
int sceAudioGetInputLength() { return 0; }

u32 sceAudioRoutingSetMode(u32 mode) {
	int prev = g_routingMode;
	g_routingMode = (int)mode;
	return (u32)prev;
}
u32 sceAudioRoutingGetMode() { return (u32)g_routingMode; }
u32 sceAudioRoutingSetVolumeMode(u32 mode) {
	int prev = g_routingVolMode;
	g_routingVolMode = (int)mode;
	return (u32)prev;
}
u32 sceAudioRoutingGetVolumeMode() { return (u32)g_routingVolMode; }

// ---- sceVaudio ----

u32 sceVaudioChReserve(int sampleCount, int freq, int format) {
	EnsureInit();
	if (sampleCount != 256 && sampleCount != 576 && sampleCount != 1024 && sampleCount != 1152 && sampleCount != 2048)
		return SCE_KERNEL_ERROR_INVALID_SIZE;
	if (format != 2) return SCE_KERNEL_ERROR_INVALID_FORMAT;
	if (g_vaudioReserved) return SCE_KERNEL_ERROR_BUSY;
	// Marked reserved before delegating, and not undone when the delegate fails.
	g_vaudioReserved = true;
	const int reserveUs = 250;
	if (freq != 0 && !SRCFrequencyAllowed(freq)) return DelayResult(SCE_ERROR_AUDIO_INVALID_FREQUENCY, reserveUs);
	if (g_src.reserved) return DelayResult(SCE_ERROR_AUDIO_CHANNEL_ALREADY_RESERVED, reserveUs);
	g_src.clear();
	g_src.reserved = true;
	g_src.sampleCount = (u32)sampleCount;
	g_src.format = PSP_AUDIO_FORMAT_STEREO;
	g_srcFrequency = freq;
	return DelayResult(0, reserveUs);
}

u32 sceVaudioChRelease() {
	// Not gated on vaudio's own flag: it releases whatever holds the SRC channel.
	g_vaudioReserved = false;
	if (!g_src.reserved) return SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
	// Hands the channel a null pointer first, which parks the caller until a buffer finishes.
	bool mustWait = false;
	u32 drain = SRCEnqueue(g_src, 0, -1, &mustWait);
	u32 ret;
	if (g_src.Full()) {
		ret = SCE_ERROR_AUDIO_CHANNEL_ALREADY_RESERVED;
	} else {
		// The reservation goes now rather than when the drain finishes.
		g_src.reserved = false;
		g_src.sampleCount = 0;
		SRCSignal(g_src);
		ret = 0;
	}
	if (mustWait) ret = SRCWait(g_src, drain);
	FinishCall();
	return ret;
}

u32 sceVaudioOutputBlocking(int vol, u32 buffer) {
	EnsureInit();
	u32 r = SRCEnqueueBlocking(g_src, buffer, vol);
	FinishCall();
	return r;
}

u32 sceVaudioSetEffectType(int effectType, int vol) { return 0; }
u32 sceVaudioSetAlcMode(int alcMode) { return 0; }

}  // namespace

// ---- Shared helpers ----

void EnsureInit() {
	if (g_inited) return;
	g_inited = true;
	audio::SetGuestMemory(&GuestPtrAccessor);
	for (u32 i = 0; i < PSP_AUDIO_CHANNEL_MAX; i++) {
		g_chans[i].index = (int)i;
		g_chans[i].clear();
	}
	g_src.clear();
	g_mixFrequency = 44100;
	g_srcFrequency = 0;
	g_mixerDMABusy = false;
	RephaseMixer();
}

u32 DelayResult(u32 result, int usec) {
	if (usec <= 0 || sched::InInterrupt() || !sched::DispatchEnabled() || !sched::Current()) return result;
	sched::WaitUntil(WAITTYPE_HLEDELAY, 0, sched::NowUs() + (u64)usec, false);
	return result;
}

void EatUs(int usec) {
	// Only the virtual clock (--fast-time) needs telling; in real time the call simply took as
	// long as it took.
	if (usec > 0) sched::ChargeUs(usec);
}

int MEScheduleJob(int us) {
	const s64 now = (s64)sched::NowUs();
	const s64 start = std::max(now, g_meBusyUntilUs);
	g_meBusyUntilUs = start + us;
	return (int)(g_meBusyUntilUs - now);
}

u32 ClampValidSize(u32 addr, u32 size) {
	if (mem::IsValidRange(addr, size)) return size;
	if (!mem::IsValidAddress(addr)) return 0;
	u32 lo = 1, hi = size;
	while (lo < hi) {
		const u32 mid = lo + (hi - lo + 1) / 2;
		if (mem::IsValidRange(addr, mid))
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

u32 CompiledSdkVersion() {
	// Owned by SysMemUserForUser; ask it through its HLE entry point.
	const hle::HleFunction *f = hle::FindFunction("SysMemUserForUser", 0xFC114573);
	if (!f || !f->fn) return 0;
	alignas(64) static Ctx tmp;
	memset(&tmp, 0, sizeof(tmp));
	f->fn(&tmp);
	return tmp.r[2];
}

}  // namespace audiohle

using namespace audiohle;

void Register_sceAudio() {
	static const hle::HleFunction audioFuncs[] = {
		HLEFN(0x01562BA3, sceAudioOutput2Reserve),
		HLEFN(0x2D53F36E, sceAudioOutput2OutputBlocking),
		HLEFN(0x63F2889C, sceAudioOutput2ChangeLength),
		HLEFN(0x647CEF33, sceAudioOutput2GetRestSample),
		HLEFN(0x43196845, sceAudioOutput2Release),
		HLEFN(0x80F1F7E0, sceAudioInit),
		HLEFN(0x210567F7, sceAudioEnd),
		HLEFN(0xA2BEAA6C, sceAudioSetFrequency),
		HLEFN(0x927AC32B, sceAudioSetVolumeOffset),
		HLEFN(0x8C1009B2, sceAudioOutput),
		HLEFN(0x136CAF51, sceAudioOutputBlocking),
		HLEFN(0xE2D56B2D, sceAudioOutputPanned),
		HLEFN(0x13F592BC, sceAudioOutputPannedBlocking),
		HLEFN(0x5EC81C55, sceAudioChReserve),
		HLEFN(0x6FC46853, sceAudioChRelease),
		HLEFN(0xE9D97901, sceAudioGetChannelRestLen),
		HLEFN(0xB011922F, sceAudioGetChannelRestLength),
		HLEFN(0xCB2E439E, sceAudioSetChannelDataLen),
		HLEFN(0x95FD0C2D, sceAudioChangeChannelConfig),
		HLEFN(0xB7E1D8E7, sceAudioChangeChannelVolume),
		HLEFN(0x38553111, sceAudioSRCChReserve),
		HLEFN(0x5C37C0AE, sceAudioSRCChRelease),
		HLEFN(0xE0727056, sceAudioSRCOutputBlocking),
		HLEFN(0x41EFADE7, sceAudioOneshotOutput),
		HLESTUB(0xB61595C0, "sceAudioLoopbackTest"),
		HLEFN(0x7DE61688, sceAudioInputInit),
		HLEFN(0xE926D3FB, sceAudioInputInitEx),
		HLEFN(0x6D4BEC68, sceAudioInput),
		HLEFN(0x086E5895, sceAudioInputBlocking),
		HLEFN(0xA708C6A6, sceAudioGetInputLength),
		HLEFN(0xA633048E, sceAudioPollInputEnd),
		HLEFN(0x87B2E651, sceAudioWaitInputEnd),
		HLEFN(0x36FD8AA9, sceAudioRoutingSetMode),
		HLEFN(0x39240E7D, sceAudioRoutingGetMode),
		HLEFN(0xBB548475, sceAudioRoutingSetVolumeMode),
		HLEFN(0x28235C56, sceAudioRoutingGetVolumeMode),
	};
	hle::RegisterModule("sceAudio", audioFuncs);
	hle::RegisterModule("sceAudio_driver", audioFuncs);

	Register_sceVaudio();
	Register_sceSasCore();
	Register_sceAtrac3plus();
	Register_sceMp3();
	Register_sceAudiocodec();
	Register_sceAac();
}

void audiohle::Register_sceVaudio() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x8986295E, sceVaudioOutputBlocking),
		HLEFN(0x03B6807D, sceVaudioChReserve),
		HLEFN(0x67585DFD, sceVaudioChRelease),
		HLEFN(0x346FBE94, sceVaudioSetEffectType),
		HLEFN(0xCBD4AC51, sceVaudioSetAlcMode),
		HLESTUB(0x504E4745, "sceVaudio_504E4745"),
		HLESTUB(0x27ACC20B, "sceVaudioChReserveBuffering"),
		HLESTUB(0xE8E78DC8, "sceVaudio_E8E78DC8"),
	};
	hle::RegisterModule("sceVaudio", funcs);
	hle::RegisterModule("sceVaudio_driver", funcs);
}
