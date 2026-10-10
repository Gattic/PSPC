// SPDX-License-Identifier: GPL-2.0-or-later
#include "media/jpeg_decoder.h"

#include <windows.h>
#include <wincodec.h>

#include "common/log.h"

namespace media {

namespace {

thread_local bool t_comInit = false;

IWICImagingFactory *Factory() {
	static IWICImagingFactory *factory = nullptr;
	static bool tried = false;
	if (!t_comInit) {
		// RPC_E_CHANGED_MODE (already in an STA) works just as well.
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		t_comInit = true;
	}
	if (!tried) {
		tried = true;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
			factory = nullptr;
			LOGW("MEDIA", "WIC is not available; JPEG decoding will fail");
		}
	}
	return factory;
}

}  // namespace

bool ParseJpegHeader(const u8 *data, size_t size, JpegInfo *info) {
	if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
	size_t i = 2;
	while (i + 4 <= size) {
		if (data[i] != 0xFF) return false;
		const u8 m = data[i + 1];
		if (m == 0xFF) {
			i++;
			continue;
		}
		if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
			i += 2;
			continue;
		}
		const size_t len = ((size_t)data[i + 2] << 8) | data[i + 3];
		if (len < 2) return false;
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			if (i + 2 + len > size || len < 8) return false;
			const u8 *p = data + i + 4;
			info->sofType = m - 0xC0;
			info->height = (p[1] << 8) | p[2];
			info->width = (p[3] << 8) | p[4];
			info->components = p[5];
			for (int c = 0; c < info->components && c < 4 && 6 + c * 3 + 2 < (int)len - 2; c++) {
				info->hSamp[c] = p[6 + c * 3 + 1] >> 4;
				info->vSamp[c] = p[6 + c * 3 + 1] & 15;
			}
			return true;
		}
		if (m == 0xDA || m == 0xD9) return false;
		i += 2 + len;
	}
	return false;
}

bool DecodeJpegRgb(const u8 *data, size_t size, int *width, int *height, std::vector<u8> *rgb) {
	IWICImagingFactory *factory = Factory();
	if (!factory || size == 0 || size > 0x7FFFFFFF) return false;
	IWICStream *stream = nullptr;
	IWICBitmapDecoder *decoder = nullptr;
	IWICBitmapFrameDecode *frame = nullptr;
	IWICBitmapSource *converted = nullptr;
	bool ok = false;
	if (SUCCEEDED(factory->CreateStream(&stream)) &&
	    SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(data), (DWORD)size)) &&
	    SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
	    SUCCEEDED(decoder->GetFrame(0, &frame)) &&
	    SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat24bppRGB, frame, &converted))) {
		UINT w = 0, h = 0;
		if (SUCCEEDED(converted->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 0x4000 && h <= 0x4000) {
			rgb->resize((size_t)w * h * 3);
			if (SUCCEEDED(converted->CopyPixels(nullptr, w * 3, (UINT)rgb->size(), rgb->data()))) {
				*width = (int)w;
				*height = (int)h;
				ok = true;
			}
		}
	}
	if (converted) converted->Release();
	if (frame) frame->Release();
	if (decoder) decoder->Release();
	if (stream) stream->Release();
	return ok;
}

bool DecodeImageRgba(const u8 *data, size_t size, int *width, int *height, std::vector<u32> *rgba) {
	IWICImagingFactory *factory = Factory();
	if (!factory || size == 0 || size > 0x7FFFFFFF) return false;
	IWICStream *stream = nullptr;
	IWICBitmapDecoder *decoder = nullptr;
	IWICBitmapFrameDecode *frame = nullptr;
	IWICBitmapSource *converted = nullptr;
	bool ok = false;
	if (SUCCEEDED(factory->CreateStream(&stream)) &&
	    SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(data), (DWORD)size)) &&
	    SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
	    SUCCEEDED(decoder->GetFrame(0, &frame)) &&
	    SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted))) {
		UINT w = 0, h = 0;
		if (SUCCEEDED(converted->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 0x1000 && h <= 0x1000) {
			rgba->resize((size_t)w * h);
			if (SUCCEEDED(converted->CopyPixels(nullptr, w * 4, (UINT)(rgba->size() * 4), (BYTE *)rgba->data()))) {
				*width = (int)w;
				*height = (int)h;
				ok = true;
			}
		}
	}
	if (converted) converted->Release();
	if (frame) frame->Release();
	if (decoder) decoder->Release();
	if (stream) stream->Release();
	return ok;
}

}  // namespace media
