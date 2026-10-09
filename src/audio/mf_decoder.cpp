// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/mf_decoder.h"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <mutex>

#include "common/log.h"

namespace audio {

namespace {

template <typename T>
void SafeRelease(T *&p) {
	if (p) {
		p->Release();
		p = nullptr;
	}
}

std::once_flag g_mfOnce;
bool g_mfOk = false;
thread_local bool t_comInit = false;

bool EnsureMF() {
	if (!t_comInit) {
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		// RPC_E_CHANGED_MODE: the thread is already in an STA, which works just as well.
		(void)hr;
		t_comInit = true;
	}
	std::call_once(g_mfOnce, []() {
		HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
		g_mfOk = SUCCEEDED(hr);
		if (!g_mfOk) LOGW("AUDIO", "Media Foundation is not available (%08lx); MP3/AAC will decode to silence", hr);
	});
	return g_mfOk;
}

IMFTransform *CreateTransform(const GUID &subtype) {
	MFT_REGISTER_TYPE_INFO in{MFMediaType_Audio, subtype};
	IMFActivate **acts = nullptr;
	UINT32 count = 0;
	IMFTransform *t = nullptr;
	HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
	                       &in, nullptr, &acts, &count);
	if (SUCCEEDED(hr)) {
		for (UINT32 i = 0; i < count && !t; i++) {
			if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) t = nullptr;
		}
		for (UINT32 i = 0; i < count; i++) acts[i]->Release();
		CoTaskMemFree(acts);
	}
	if (!t) {
		const CLSID &clsid = subtype == MFAudioFormat_MP3 ? CLSID_CMP3DecMediaObject : CLSID_CMSAACDecMFT;
		if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) t = nullptr;
	}
	return t;
}

bool SetPcmOutput(IMFTransform *t, int *outChannels, int *outRate) {
	for (DWORD i = 0;; i++) {
		IMFMediaType *type = nullptr;
		if (FAILED(t->GetOutputAvailableType(0, i, &type))) break;
		GUID sub{};
		UINT32 bits = 0;
		type->GetGUID(MF_MT_SUBTYPE, &sub);
		type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
		if (sub == MFAudioFormat_PCM && bits == 16 && SUCCEEDED(t->SetOutputType(0, type, 0))) {
			UINT32 ch = 0, rate = 0;
			type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
			type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
			*outChannels = (int)ch;
			*outRate = (int)rate;
			type->Release();
			return true;
		}
		type->Release();
	}
	return false;
}

}  // namespace

bool MFAvailable() { return EnsureMF(); }

MFDecoder::~MFDecoder() { Close(); }

void MFDecoder::Close() {
	IMFTransform *t = (IMFTransform *)transform_;
	if (t) {
		t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
		t->Release();
	}
	transform_ = nullptr;
}

bool MFDecoder::Open(Format fmt, int sampleRate, int channels, const u8 *asc, int ascSize) {
	Close();
	if (!EnsureMF()) return false;
	const GUID &subtype = fmt == Format::MP3 ? MFAudioFormat_MP3 : MFAudioFormat_AAC;
	IMFTransform *t = CreateTransform(subtype);
	if (!t) {
		LOGW("AUDIO", "No Media Foundation decoder for %s", fmt == Format::MP3 ? "MP3" : "AAC");
		return false;
	}

	IMFMediaType *in = nullptr;
	MFCreateMediaType(&in);
	in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	in->SetGUID(MF_MT_SUBTYPE, subtype);
	in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
	in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
	HRESULT hr;
	if (fmt == Format::MP3) {
		hr = t->SetInputType(0, in, 0);
	} else {
		in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		const UINT32 payload = fmt == Format::AAC_ADTS ? 1 : 0;
		in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, payload);
		in->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
		// The part of HEAACWAVEINFO after WAVEFORMATEX, then the AudioSpecificConfig.
		u8 user[12 + 16] = {};
		user[0] = (u8)payload;
		user[2] = 0x29;
		int userSize = 12;
		if (asc && ascSize > 0 && ascSize <= 16) {
			memcpy(user + 12, asc, ascSize);
			userSize += ascSize;
		}
		in->SetBlob(MF_MT_USER_DATA, user, userSize);
		hr = t->SetInputType(0, in, 0);
		if (FAILED(hr) && userSize > 12) {
			in->SetBlob(MF_MT_USER_DATA, user, 12);
			hr = t->SetInputType(0, in, 0);
		}
	}
	in->Release();
	if (FAILED(hr)) {
		LOGW("AUDIO", "MF decoder rejected input type (%d Hz, %d ch): %08lx", sampleRate, channels, hr);
		t->Release();
		return false;
	}
	int oc = 0, orate = 0;
	if (!SetPcmOutput(t, &oc, &orate)) {
		LOGW("AUDIO", "MF decoder has no 16-bit PCM output");
		t->Release();
		return false;
	}
	MFT_OUTPUT_STREAM_INFO info{};
	t->GetOutputStreamInfo(0, &info);
	providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
	outBufferSize_ = info.cbSize > 0x10000 ? info.cbSize : 0x10000;
	t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	transform_ = t;
	sampleRate_ = orate ? orate : sampleRate;
	channels_ = channels;
	outChannels_ = oc ? oc : channels;
	time_ = 0;
	return true;
}

void MFDecoder::Flush() {
	IMFTransform *t = (IMFTransform *)transform_;
	if (t) t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

bool MFDecoder::Drain(std::vector<s16> &out) {
	IMFTransform *t = (IMFTransform *)transform_;
	for (int guard = 0; guard < 64; guard++) {
		MFT_OUTPUT_DATA_BUFFER odb{};
		odb.dwStreamID = 0;
		IMFSample *own = nullptr;
		if (!providesSamples_) {
			IMFMediaBuffer *ob = nullptr;
			if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(outBufferSize_, &ob))) {
				SafeRelease(own);
				return false;
			}
			own->AddBuffer(ob);
			ob->Release();
			odb.pSample = own;
		}
		DWORD status = 0;
		HRESULT hr = t->ProcessOutput(0, 1, &odb, &status);
		if (odb.pEvents) odb.pEvents->Release();
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
			SafeRelease(own);
			return true;
		}
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			SafeRelease(own);
			int oc = 0, orate = 0;
			if (!SetPcmOutput(t, &oc, &orate)) return false;
			outChannels_ = oc;
			sampleRate_ = orate;
			continue;
		}
		if (FAILED(hr)) {
			SafeRelease(own);
			return false;
		}
		IMFSample *s = odb.pSample;
		IMFMediaBuffer *buf = nullptr;
		if (s && SUCCEEDED(s->ConvertToContiguousBuffer(&buf))) {
			BYTE *p = nullptr;
			DWORD len = 0;
			if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
				size_t n = len / 2;
				size_t old = out.size();
				out.resize(old + n);
				memcpy(out.data() + old, p, n * 2);
				buf->Unlock();
			}
			buf->Release();
		}
		if (providesSamples_ && s) s->Release();
		SafeRelease(own);
	}
	return true;
}

bool MFDecoder::Decode(const u8 *data, int size, std::vector<s16> &out) {
	IMFTransform *t = (IMFTransform *)transform_;
	if (!t || size <= 0) return false;
	IMFSample *sample = nullptr;
	IMFMediaBuffer *buf = nullptr;
	if (FAILED(MFCreateSample(&sample))) return false;
	if (FAILED(MFCreateMemoryBuffer((DWORD)size, &buf))) {
		sample->Release();
		return false;
	}
	BYTE *p = nullptr;
	buf->Lock(&p, nullptr, nullptr);
	memcpy(p, data, size);
	buf->Unlock();
	buf->SetCurrentLength((DWORD)size);
	sample->AddBuffer(buf);
	buf->Release();
	sample->SetSampleTime(time_);
	time_ += 10000000LL * 1152 / (sampleRate_ ? sampleRate_ : 44100);
	HRESULT hr = t->ProcessInput(0, sample, 0);
	if (hr == MF_E_NOTACCEPTING) {
		Drain(out);
		hr = t->ProcessInput(0, sample, 0);
	}
	sample->Release();
	if (FAILED(hr)) return false;
	return Drain(out);
}

}  // namespace audio
