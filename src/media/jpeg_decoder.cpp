// SPDX-License-Identifier: GPL-2.0-or-later
#include "media/jpeg_decoder.h"

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#endif

#include "common/log.h"

namespace media {

namespace {

#ifdef _WIN32
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

#else
bool DecodePixels(const u8 *data, size_t size, int limit, AVPixelFormat format, int *width, int *height,
                  std::vector<u8> &pixels) {
	if (!data || size < 8 || size > INT32_MAX) return false;
	AVCodecID id;
	if (data[0] == 0xFF && data[1] == 0xD8) id = AV_CODEC_ID_MJPEG;
	else if (!memcmp(data, "\x89PNG\r\n\x1a\n", 8)) id = AV_CODEC_ID_PNG;
	else if (data[0] == 'B' && data[1] == 'M') id = AV_CODEC_ID_BMP;
	else if (!memcmp(data, "GIF8", 4)) id = AV_CODEC_ID_GIF;
	else return false;
	const AVCodec *codec = avcodec_find_decoder(id);
	if (!codec) return false;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	AVFrame *frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	SwsContext *scale = nullptr;
	bool ok = false;
	if (ctx && frame && packet) {
		ctx->max_pixels = (int64_t)limit * limit;
		if (avcodec_open2(ctx, codec, nullptr) >= 0 && av_new_packet(packet, (int)size) >= 0) {
			memcpy(packet->data, data, size);
			if (avcodec_send_packet(ctx, packet) >= 0 && avcodec_receive_frame(ctx, frame) >= 0 &&
			    frame->width > 0 && frame->width <= limit && frame->height > 0 && frame->height <= limit) {
				int channels = format == AV_PIX_FMT_RGBA ? 4 : 3;
				pixels.resize((size_t)frame->width * frame->height * channels);
				u8 *dest[] = {pixels.data(), nullptr, nullptr, nullptr};
				int stride[] = {frame->width * channels, 0, 0, 0};
				scale = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
				                       frame->width, frame->height, format, SWS_POINT, nullptr, nullptr, nullptr);
				if (scale && sws_scale(scale, frame->data, frame->linesize, 0, frame->height, dest, stride) == frame->height) {
					*width = frame->width; *height = frame->height; ok = true;
				}
			}
		}
	}
	sws_freeContext(scale); av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&ctx);
	return ok;
}
#endif
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
#ifndef _WIN32
	return DecodePixels(data, size, 0x4000, AV_PIX_FMT_RGB24, width, height, *rgb);
#else
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
#endif
}

bool DecodeImageRgba(const u8 *data, size_t size, int *width, int *height, std::vector<u32> *rgba) {
#ifndef _WIN32
	std::vector<u8> bytes;
	if (!DecodePixels(data, size, 0x1000, AV_PIX_FMT_RGBA, width, height, bytes)) return false;
	rgba->resize(bytes.size() / 4);
	memcpy(rgba->data(), bytes.data(), bytes.size());
	return true;
#else
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
#endif
}

}  // namespace media
