// SPDX-License-Identifier: GPL-2.0-or-later
#include "media/h264_decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "audio/mf_decoder.h"
#include "common/log.h"

namespace media {

namespace {

// CODECAPI_AVLowLatencyMode (codecapi.h), defined here so no extra import library is needed.
const GUID kCodecApiLowLatency = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

template <typename T>
void SafeRelease(T *&p) {
	if (p) {
		p->Release();
		p = nullptr;
	}
}

IMFTransform *CreateH264Transform() {
	IMFTransform *t = nullptr;
	if (FAILED(CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) t = nullptr;
	if (t) return t;
	MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_H264};
	IMFActivate **acts = nullptr;
	UINT32 count = 0;
	if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
	                        &in, nullptr, &acts, &count))) {
		for (UINT32 i = 0; i < count && !t; i++) {
			if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) t = nullptr;
		}
		for (UINT32 i = 0; i < count; i++) acts[i]->Release();
		CoTaskMemFree(acts);
	}
	return t;
}

inline s64 PtsToHns(s64 pts) { return (pts * 1000 + 4) / 9; }
inline s64 HnsToPts(s64 t) { return (t * 9 + 500) / 1000; }

std::once_flag g_probeOnce;
bool g_available = false;

}  // namespace

bool H264Decoder::Available() {
	std::call_once(g_probeOnce, []() {
		if (!audio::MFAvailable()) return;
		IMFTransform *t = CreateH264Transform();
		g_available = t != nullptr;
		if (t) t->Release();
		if (!g_available) LOGW("MEDIA", "No Media Foundation H.264 decoder; videos will not play");
	});
	return g_available;
}

H264Decoder::~H264Decoder() { Close(); }

void H264Decoder::Close() {
	IMFTransform *t = (IMFTransform *)transform_;
	if (t) {
		t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
		t->Release();
	}
	transform_ = nullptr;
	frames_.clear();
	frameW_ = frameH_ = displayW_ = displayH_ = stride_ = 0;
	decoded_ = 0;
}

bool H264Decoder::Open() {
	Close();
	if (!Available()) return false;
	IMFTransform *t = CreateH264Transform();
	if (!t) return false;

	// Hand pictures over as soon as they are decodable, as the Media Engine does.
	IMFAttributes *attrs = nullptr;
	if (SUCCEEDED(t->GetAttributes(&attrs)) && attrs) {
		attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
		attrs->Release();
	}
	ICodecAPI *codecApi = nullptr;
	if (SUCCEEDED(t->QueryInterface(IID_PPV_ARGS(&codecApi))) && codecApi) {
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_UI4;
		v.ulVal = 1;
		codecApi->SetValue(&kCodecApiLowLatency, &v);
		codecApi->Release();
	}

	IMFMediaType *in = nullptr;
	MFCreateMediaType(&in);
	in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	HRESULT hr = t->SetInputType(0, in, 0);
	in->Release();
	if (FAILED(hr)) {
		LOGW("MEDIA", "H.264 decoder rejected its input type: %08lx", hr);
		t->Release();
		return false;
	}
	transform_ = t;
	if (!SetOutputType()) {
		LOGW("MEDIA", "H.264 decoder has no YUV 4:2:0 output");
		Close();
		return false;
	}
	t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	lastTime_ = 0;
	return true;
}

bool H264Decoder::SetOutputType() {
	IMFTransform *t = (IMFTransform *)transform_;
	static const GUID *const kWanted[3] = {&MFVideoFormat_NV12, &MFVideoFormat_I420, &MFVideoFormat_YV12};
	for (int want = 0; want < 3; want++) {
		for (DWORD i = 0;; i++) {
			IMFMediaType *type = nullptr;
			if (FAILED(t->GetOutputAvailableType(0, i, &type))) break;
			GUID sub{};
			type->GetGUID(MF_MT_SUBTYPE, &sub);
			bool ok = false;
			if (sub == *kWanted[want] || (want == 1 && sub == MFVideoFormat_IYUV)) {
				ok = SUCCEEDED(t->SetOutputType(0, type, 0));
			}
			if (ok) {
				UINT32 w = 0, h = 0;
				MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
				frameW_ = (int)w;
				frameH_ = (int)h;
				displayW_ = frameW_;
				displayH_ = frameH_;
				MFVideoArea area{};
				UINT32 got = 0;
				if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), &got)) &&
				    got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
					displayW_ = std::min<int>(area.Area.cx, frameW_);
					displayH_ = std::min<int>(area.Area.cy, frameH_);
				}
				UINT32 stride = 0;
				if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && (int)stride > 0)
					stride_ = (int)stride;
				else
					stride_ = frameW_;
				format_ = want;
				type->Release();
				MFT_OUTPUT_STREAM_INFO info{};
				t->GetOutputStreamInfo(0, &info);
				providesSamples_ =
				    (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
				return true;
			}
			type->Release();
		}
	}
	return false;
}

void H264Decoder::CollectOutput() {
	IMFTransform *t = (IMFTransform *)transform_;
	if (!t) return;
	for (int guard = 0; guard < 64; guard++) {
		MFT_OUTPUT_STREAM_INFO info{};
		t->GetOutputStreamInfo(0, &info);
		MFT_OUTPUT_DATA_BUFFER odb{};
		IMFSample *own = nullptr;
		if (!providesSamples_) {
			IMFMediaBuffer *ob = nullptr;
			const DWORD size = std::max<DWORD>(info.cbSize, (DWORD)(std::max(frameW_, 16) * std::max(frameH_, 16) * 3 / 2));
			if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(size, &ob))) {
				SafeRelease(own);
				SafeRelease(ob);
				return;
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
			return;
		}
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			SafeRelease(own);
			if (!SetOutputType()) return;
			continue;
		}
		if (FAILED(hr)) {
			SafeRelease(own);
			return;
		}
		IMFSample *s = odb.pSample;
		if (s && frameW_ > 0 && frameH_ > 0) {
			YuvFrame f;
			f.width = displayW_ & ~1;
			f.height = displayH_ & ~1;
			LONGLONG time = 0;
			f.pts = SUCCEEDED(s->GetSampleTime(&time)) ? HnsToPts(time) : -1;
			IMFMediaBuffer *buf = nullptr;
			if (SUCCEEDED(s->GetBufferByIndex(0, &buf)) && buf) {
				BYTE *base = nullptr;
				LONG pitch = 0;
				IMF2DBuffer *b2d = nullptr;
				bool locked2d = false;
				DWORD len = 0;
				if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2d))) && b2d) {
					if (SUCCEEDED(b2d->Lock2D(&base, &pitch)) && pitch > 0) {
						locked2d = true;
					} else {
						base = nullptr;
					}
				}
				if (!locked2d) {
					if (FAILED(buf->Lock(&base, nullptr, &len))) base = nullptr;
					pitch = stride_;
				}
				if (base) {
					const int w = f.width, h = f.height;
					const int w2 = w / 2, h2 = h / 2;
					f.y.resize((size_t)w * h);
					f.cb.resize((size_t)w2 * h2);
					f.cr.resize((size_t)w2 * h2);
					for (int row = 0; row < h; row++) memcpy(&f.y[(size_t)row * w], base + (size_t)row * pitch, w);
					const u8 *planes = base + (size_t)pitch * frameH_;
					if (format_ == 0) {
						for (int row = 0; row < h2; row++) {
							const u8 *uv = planes + (size_t)row * pitch;
							u8 *cb = &f.cb[(size_t)row * w2];
							u8 *cr = &f.cr[(size_t)row * w2];
							for (int x = 0; x < w2; x++) {
								cb[x] = uv[x * 2];
								cr[x] = uv[x * 2 + 1];
							}
						}
					} else {
						const int cpitch = pitch / 2;
						const u8 *p1 = planes;
						const u8 *p2 = planes + (size_t)cpitch * (frameH_ / 2);
						const u8 *cbp = format_ == 1 ? p1 : p2;
						const u8 *crp = format_ == 1 ? p2 : p1;
						for (int row = 0; row < h2; row++) {
							memcpy(&f.cb[(size_t)row * w2], cbp + (size_t)row * cpitch, w2);
							memcpy(&f.cr[(size_t)row * w2], crp + (size_t)row * cpitch, w2);
						}
					}
					if (locked2d)
						b2d->Unlock2D();
					else
						buf->Unlock();
					decoded_++;
					frames_.push_back(std::move(f));
				}
				SafeRelease(b2d);
				buf->Release();
			}
		}
		if (providesSamples_ && s) s->Release();
		SafeRelease(own);
	}
}

bool H264Decoder::Decode(const u8 *data, int size, s64 pts) {
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
	const s64 time = pts >= 0 ? PtsToHns(pts) : lastTime_ + 333667;
	lastTime_ = time;
	sample->SetSampleTime(time);
	sample->SetSampleDuration(333667);
	HRESULT hr = t->ProcessInput(0, sample, 0);
	if (hr == MF_E_NOTACCEPTING) {
		CollectOutput();
		hr = t->ProcessInput(0, sample, 0);
	}
	sample->Release();
	CollectOutput();
	return SUCCEEDED(hr);
}

bool H264Decoder::PopFrame(YuvFrame *out) {
	if (frames_.empty()) return false;
	*out = std::move(frames_.front());
	frames_.pop_front();
	return true;
}

void H264Decoder::Drain() {
	IMFTransform *t = (IMFTransform *)transform_;
	if (!t) return;
	t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
	CollectOutput();
}

void H264Decoder::Flush() {
	IMFTransform *t = (IMFTransform *)transform_;
	frames_.clear();
	if (t) t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

}  // namespace media
