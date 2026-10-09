// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceSasCore: the HLE face of the sceSas mixer (audio/sas.h does the mixing).
// Ported from PPSSPP's Core/HLE/sceSas.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include <cstdlib>

#include "audio/sas.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

using namespace audio;

namespace audiohle {

namespace {

SasInstance *g_sas = nullptr;

SasInstance *Sas() {
	if (!g_sas) {
		EnsureInit();
		g_sas = new SasInstance();
	}
	return g_sas;
}

// The mix runs on the Media Engine, after whatever else it is busy with; the caller waits.
u32 DelaySasResult(u32 result) {
	const int usec = MEScheduleJob(Sas()->EstimateMixUs());
	if (sched::InInterrupt()) return result;
	return DelayResult(result, usec);
}

u32 sceSasInit(u32 core, u32 grainSize, u32 maxVoices, u32 outputMode, u32 sampleRate) {
	SasInstance *sas = Sas();
	if (!mem::IsValidAddress(core) || (core & 0x3F) != 0) return SCE_SAS_ERROR_BAD_ADDRESS;
	if (maxVoices == 0 || maxVoices > PSP_SAS_VOICES_MAX) return SCE_SAS_ERROR_INVALID_MAX_VOICES;
	if (grainSize < 0x40 || grainSize > 0x800 || (grainSize & 0x1F) != 0) return SCE_SAS_ERROR_INVALID_GRAIN;
	if (outputMode != 0 && outputMode != 1) return SCE_SAS_ERROR_INVALID_OUTPUT_MODE;
	if (sampleRate != 44100) return SCE_SAS_ERROR_INVALID_SAMPLE_RATE;
	sas->SetGrainSize((int)grainSize);
	// maxVoices is effectively ignored.
	sas->maxVoices = PSP_SAS_VOICES_MAX;
	sas->outputMode = (int)outputMode;
	for (int i = 0; i < sas->maxVoices; i++) {
		sas->voices[i].sampleRate = (int)sampleRate;
		sas->voices[i].playing = false;
		sas->voices[i].loop = false;
	}
	return 0;
}

u32 sceSasGetEndFlag(u32 core) {
	SasInstance *sas = Sas();
	u32 endFlag = 0;
	for (int i = 0; i < sas->maxVoices; i++) {
		if (!sas->voices[i].playing) endFlag |= (1u << i);
	}
	return endFlag;
}

u32 _sceSasCore(u32 core, u32 outAddr) {
	SasInstance *sas = Sas();
	if (!mem::IsValidAddress(outAddr)) return SCE_SAS_ERROR_INVALID_PARAMETER;
	if (!sched::DispatchEnabled()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	sas->Mix(outAddr, 0, 0, 0, false);
	return DelaySasResult(0);
}

u32 _sceSasCoreWithMix(u32 core, u32 inoutAddr, int leftVolume, int rightVolume) {
	SasInstance *sas = Sas();
	if (!mem::IsValidAddress(inoutAddr)) return SCE_SAS_ERROR_INVALID_PARAMETER;
	if (sas->outputMode == PSP_SAS_OUTPUTMODE_RAW) return SCE_KERNEL_ERROR_BAD_ARGUMENT;
	if (!sched::DispatchEnabled()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	sas->Mix(inoutAddr, inoutAddr, leftVolume, rightVolume, false);
	return DelaySasResult(0);
}

u32 sceSasSetVoice(u32 core, int voiceNum, u32 vagAddr, int size, int loop) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	if (size == 0 || ((u32)size & 0xF) != 0) return SCE_SAS_ERROR_INVALID_PARAMETER;
	if (loop != 0 && loop != 1) return SCE_SAS_ERROR_INVALID_LOOP_POS;
	if (!mem::IsValidAddress(vagAddr)) return 0;  // ignored
	SasVoice &v = sas->voices[voiceNum];
	if (v.type == VOICETYPE_ATRAC3) return SCE_SAS_ERROR_ATRAC3_ALREADY_SET;
	// A negative size returns OK but plays nothing.
	if (size < 0) size = 0;
	bool reset = false;
	if (v.type != VOICETYPE_VAG || v.vagAddr != vagAddr || v.vagSize != (u32)size || v.loop != (loop != 0)) {
		v.type = VOICETYPE_VAG;
		reset = true;
	}
	v.vagAddr = vagAddr;
	v.vagSize = (u32)size;
	v.loop = loop != 0;
	if (v.on) v.playing = true;
	if (reset) v.vag.Start(vagAddr, (u32)size, loop != 0);
	return 0;
}

u32 sceSasSetVoicePCM(u32 core, int voiceNum, u32 pcmAddr, int size, int loopPos) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	if (size <= 0 || size > 0x10000) return SCE_SAS_ERROR_INVALID_PCM_SIZE;
	if (loopPos >= size) return SCE_SAS_ERROR_INVALID_LOOP_POS;
	if (!mem::IsValidAddress(pcmAddr)) return 0;  // ignored
	SasVoice &v = sas->voices[voiceNum];
	if (v.type == VOICETYPE_ATRAC3) return SCE_SAS_ERROR_ATRAC3_ALREADY_SET;
	v.type = VOICETYPE_PCM;
	v.pcmAddr = pcmAddr;
	v.pcmSize = size;
	v.pcmIndex = 0;
	v.pcmLoopPos = loopPos >= 0 ? loopPos : 0;
	v.loop = loopPos >= 0;
	v.playing = true;
	return 0;
}

u32 sceSasGetPauseFlag(u32 core) {
	SasInstance *sas = Sas();
	u32 pauseFlag = 0;
	for (int i = 0; i < sas->maxVoices; i++) {
		if (sas->voices[i].paused) pauseFlag |= (1u << i);
	}
	return pauseFlag;
}

u32 sceSasSetPause(u32 core, u32 voicebit, int pause) {
	SasInstance *sas = Sas();
	for (int i = 0; voicebit != 0; i++, voicebit >>= 1) {
		if (i < PSP_SAS_VOICES_MAX && (voicebit & 1) != 0) sas->voices[i].paused = pause != 0;
	}
	return 0;
}

u32 sceSasSetVolume(u32 core, int voiceNum, int leftVol, int rightVol, int effectLeftVol, int effectRightVol) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	bool overVolume = abs(leftVol) > PSP_SAS_VOL_MAX || abs(rightVol) > PSP_SAS_VOL_MAX;
	overVolume = overVolume || abs(effectLeftVol) > PSP_SAS_VOL_MAX || abs(effectRightVol) > PSP_SAS_VOL_MAX;
	if (overVolume) return SCE_SAS_ERROR_INVALID_VOLUME;
	SasVoice &v = sas->voices[voiceNum];
	v.volumeLeft = leftVol;
	v.volumeRight = rightVol;
	v.effectLeft = effectLeftVol;
	v.effectRight = effectRightVol;
	return 0;
}

u32 sceSasSetPitch(u32 core, int voiceNum, int pitch) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	if (pitch < PSP_SAS_PITCH_MIN || pitch > PSP_SAS_PITCH_MAX) return SCE_SAS_ERROR_INVALID_PITCH;
	sas->voices[voiceNum].pitch = pitch;
	return 0;
}

u32 sceSasSetKeyOn(u32 core, int voiceNum) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	if (sas->voices[voiceNum].paused || sas->voices[voiceNum].on) return SCE_SAS_ERROR_VOICE_PAUSED;
	sas->voices[voiceNum].KeyOn();
	return 0;
}

// Can also start sounds that only play their release phase.
u32 sceSasSetKeyOff(u32 core, int voiceNum) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	if (sas->voices[voiceNum].paused || !sas->voices[voiceNum].on) return SCE_SAS_ERROR_VOICE_PAUSED;
	sas->voices[voiceNum].KeyOff();
	return 0;
}

u32 sceSasSetNoise(u32 core, int voiceNum, int freq) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	if (freq < 0 || freq >= 64) return SCE_SAS_ERROR_INVALID_NOISE_FREQ;
	SasVoice &v = sas->voices[voiceNum];
	v.type = VOICETYPE_NOISE;
	v.noiseFreq = freq;
	return 0;
}

u32 sceSasSetSL(u32 core, int voiceNum, int level) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	sas->voices[voiceNum].envelope.SetSustainLevel(level);
	return 0;
}

u32 sceSasSetADSR(u32 core, int voiceNum, int flag, int a, int d, int s, int r) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	int invalid = (a < 0 ? 0x1 : 0) | (d < 0 ? 0x2 : 0) | (s < 0 ? 0x4 : 0) | (r < 0 ? 0x8 : 0);
	if (invalid & flag) return SCE_SAS_ERROR_INVALID_ADSR_RATE;
	sas->voices[voiceNum].envelope.SetRate(flag, a, d, s, r);
	return 0;
}

u32 sceSasSetADSRMode(u32 core, int voiceNum, int flag, int a, int d, int s, int r) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	// The PSP ignores the top bit of these.
	a &= ~0x80000000;
	d &= ~0x80000000;
	s &= ~0x80000000;
	r &= ~0x80000000;
	int invalid = 0;
	if (a > 5 || (a & 1) != 0) invalid |= 0x1;
	if (d > 5 || (d & 1) != 1) invalid |= 0x2;
	if (s > 5) invalid |= 0x4;
	if (r > 5 || (r & 1) != 1) invalid |= 0x8;
	if (invalid & flag) return SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE;
	sas->voices[voiceNum].envelope.SetEnvelope(flag, a, d, s, r);
	return 0;
}

u32 sceSasSetSimpleADSR(u32 core, int voiceNum, u32 ADSREnv1, u32 ADSREnv2) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	// Setting this bit is an error.
	if ((ADSREnv2 >> 13) & 1) return SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE;
	sas->voices[voiceNum].envelope.SetSimpleEnvelope(ADSREnv1 & 0xFFFF, ADSREnv2 & 0xFFFF);
	return 0;
}

u32 sceSasGetEnvelopeHeight(u32 core, int voiceNum) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	return (u32)sas->voices[voiceNum].envelope.GetHeight();
}

u32 sceSasRevType(u32 core, int type) {
	SasInstance *sas = Sas();
	if (type < PSP_SAS_EFFECT_TYPE_OFF || type > PSP_SAS_EFFECT_TYPE_MAX) return SCE_SAS_ERROR_REV_INVALID_TYPE;
	sas->SetWaveformEffectType(type);
	return 0;
}

u32 sceSasRevParam(u32 core, int delay, int feedback) {
	SasInstance *sas = Sas();
	if (delay < 0 || delay >= 128) return SCE_SAS_ERROR_REV_INVALID_DELAY;
	if (feedback < 0 || feedback >= 128) return SCE_SAS_ERROR_REV_INVALID_FEEDBACK;
	sas->SetWaveformEffectParams(delay, feedback);
	return 0;
}

u32 sceSasRevEVOL(u32 core, u32 lv, u32 rv) {
	SasInstance *sas = Sas();
	if (lv > 0x1000 || rv > 0x1000) return SCE_SAS_ERROR_REV_INVALID_VOLUME;
	sas->waveformEffect.leftVol = (int)lv;
	sas->waveformEffect.rightVol = (int)rv;
	return 0;
}

u32 sceSasRevVON(u32 core, int dry, int wet) {
	SasInstance *sas = Sas();
	sas->waveformEffect.isDryOn = dry != 0;
	sas->waveformEffect.isWetOn = wet != 0;
	return 0;
}

u32 sceSasGetGrain(u32 core) { return (u32)Sas()->GetGrainSize(); }

u32 sceSasSetGrain(u32 core, int grain) {
	if (grain < 0x40 || grain > 0x800 || (grain & 0x1F) != 0) return SCE_SAS_ERROR_INVALID_GRAIN;
	Sas()->SetGrainSize(grain);
	return 0;
}

u32 sceSasGetOutputMode(u32 core) { return (u32)Sas()->outputMode; }

u32 sceSasSetOutputMode(u32 core, u32 outputMode) {
	if (outputMode != 0 && outputMode != 1) return SCE_SAS_ERROR_INVALID_OUTPUT_MODE;
	Sas()->outputMode = (int)outputMode;
	return 0;
}

u32 sceSasGetAllEnvelopeHeights(u32 core, u32 heightsAddr) {
	SasInstance *sas = Sas();
	if (!mem::IsValidRange(heightsAddr, 4 * PSP_SAS_VOICES_MAX)) return SCE_SAS_ERROR_INVALID_PARAMETER;
	for (int i = 0; i < PSP_SAS_VOICES_MAX; i++) GWrite32(heightsAddr + i * 4, (u32)sas->voices[i].envelope.GetHeight());
	return 0;
}

u32 sceSasSetTriangularWave(u32 sasCore, int voice, int unknown) {
	LOGW("SAS", "UNIMPL sceSasSetTriangularWave(%08x, %i, %i)", sasCore, voice, unknown);
	return 0;
}

u32 sceSasSetSteepWave(u32 sasCore, int voice, int unknown) {
	LOGW("SAS", "UNIMPL sceSasSetSteepWave(%08x, %i, %i)", sasCore, voice, unknown);
	return 0;
}

u32 __sceSasSetVoiceATRAC3(u32 core, int voiceNum, u32 atrac3Context) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	if ((atrac3Context & 3) != 0 || !mem::IsValidRange(atrac3Context, 256)) return SCE_SAS_ERROR_INVALID_PARAMETER;
	SasVoice &v = sas->voices[voiceNum];
	if (v.type == VOICETYPE_ATRAC3) return SCE_SAS_ERROR_ATRAC3_ALREADY_SET;
	v.type = VOICETYPE_ATRAC3;
	v.loop = false;
	v.playing = true;
	v.atrac3.SetContext(atrac3Context);
	if (mem::IsValidRange(core + 56 * voiceNum + 20, 4)) GWrite32(core + 56 * voiceNum + 20, atrac3Context);
	return 0;
}

u32 __sceSasConcatenateATRAC3(u32 core, int voiceNum, u32 atrac3DataAddr, int atrac3DataLength) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	sas->voices[voiceNum].atrac3.Concatenate(atrac3DataAddr, (u32)atrac3DataLength);
	return 0;
}

u32 __sceSasUnsetATRAC3(u32 core, int voiceNum) {
	SasInstance *sas = Sas();
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) return SCE_SAS_ERROR_INVALID_VOICE;
	SasVoice &v = sas->voices[voiceNum];
	if (v.type != VOICETYPE_ATRAC3) return SCE_SAS_ERROR_ATRAC3_NOT_SET;
	v.type = VOICETYPE_OFF;
	v.playing = false;
	v.on = false;
	// This unpauses (Sol Trigger depends on it).
	v.paused = false;
	if (mem::IsValidRange(core + 56 * voiceNum + 20, 4)) GWrite32(core + 56 * voiceNum + 20, 0);
	return 0;
}

}  // namespace

void Register_sceSasCore() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x42778A9F, sceSasInit),
		HLEFN_F(0xA3589D81, _sceSasCore, hle::HLE_NOT_IN_INTERRUPT),
		HLEFN(0x50A14DFC, _sceSasCoreWithMix),
		HLEFN(0x68A46B95, sceSasGetEndFlag),
		HLEFN(0x440CA7D8, sceSasSetVolume),
		HLEFN(0xAD84D37F, sceSasSetPitch),
		HLEFN(0x99944089, sceSasSetVoice),
		HLEFN(0xB7660A23, sceSasSetNoise),
		HLEFN(0x019B25EB, sceSasSetADSR),
		HLEFN(0x9EC3676A, sceSasSetADSRMode),
		HLEFN(0x5F9529F6, sceSasSetSL),
		HLEFN(0x74AE582A, sceSasGetEnvelopeHeight),
		HLEFN(0xCBCD4F79, sceSasSetSimpleADSR),
		HLEFN(0xA0CF2FA4, sceSasSetKeyOff),
		HLEFN(0x76F01ACA, sceSasSetKeyOn),
		HLEFN(0xF983B186, sceSasRevVON),
		HLEFN(0xD5A229C9, sceSasRevEVOL),
		HLEFN(0x33D4AB37, sceSasRevType),
		HLEFN(0x267A6DD2, sceSasRevParam),
		HLEFN(0x2C8E6AB3, sceSasGetPauseFlag),
		HLEFN(0x787D04D5, sceSasSetPause),
		HLEFN(0xA232CBE6, sceSasSetTriangularWave),
		HLEFN(0xD5EBBBCD, sceSasSetSteepWave),
		HLEFN(0xBD11B7C2, sceSasGetGrain),
		HLEFN(0xD1E0A01E, sceSasSetGrain),
		HLEFN(0xE175EF66, sceSasGetOutputMode),
		HLEFN(0xE855BF76, sceSasSetOutputMode),
		HLEFN(0x07F58C24, sceSasGetAllEnvelopeHeights),
		HLEFN(0xE1CD9561, sceSasSetVoicePCM),
		HLEFN(0x4AA9EAD6, __sceSasSetVoiceATRAC3),
		HLEFN(0x7497EA85, __sceSasConcatenateATRAC3),
		HLEFN(0xF6107F00, __sceSasUnsetATRAC3),
	};
	hle::RegisterModule("sceSasCore", funcs);
}

}  // namespace audiohle
