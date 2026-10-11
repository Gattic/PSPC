// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceJpeg: the Media Engine's motion JPEG decoder. Ported from PPSSPP's Core/HLE/sceJpeg.cpp
// (https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
// Decoding goes through WIC (media/jpeg_decoder.h) instead of jpgd.
//
// The hardware only decodes three-component 4:2:0 images: 4:2:2 and greyscale fail with
// 0x80650016 (pspautotests jpeg/getoutputinfo, jpeg/decode).
#include <algorithm>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "media/jpeg_decoder.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/audio_hle.h"
#include "runtime/hle/modules/media_hle.h"
#include "runtime/kernel/errors.h"



namespace mediahle {

namespace {

// The names aren't known. An unsupported chroma subsampling (or greyscale) returns the first; the
// second is sceJpegDecodeMJpegYCbCrSuccessively's answer to an unusable output buffer.
constexpr u32 SCE_JPEG_ERROR_UNSUPPORTED_SAMPLING = 0x80650016;
constexpr u32 SCE_JPEG_ERROR_INVALID_OUTPUT = 0x80650010;

int g_mjpegInited = 0;
int g_mjpegWidth = 0;
int g_mjpegHeight = 0;
// Init and Finish wait for the Media Engine, except straight after a colour conversion
// (jpeg/csc, jpeg/mjpegcsc, jpeg/init, jpeg/finish all fit this).
bool g_cscSinceInitFinish = false;

u32 Delay(u32 result, int us) { return audiohle::DelayResult(result, us); }

int getWidthHeight(int width, int height) { return (width << 16) | height; }

// sceJpegMJpegCsc: chroma below 1 counts as 1, then BT.601 in 8-bit fixed point, rounded
// (jpeg/mjpegcsc "Bits", and 00008600 for an all-zero input).
u32 MJpegYCbCrToABGR(int y, int cb, int cr) {
	const int d = std::max(cb, 1) - 128, e = std::max(cr, 1) - 128;
	const int r = std::clamp(y + ((359 * e + 128) >> 8), 0, 255);
	const int g = std::clamp(y + ((-88 * d - 182 * e + 128) >> 8), 0, 255);
	const int b = std::clamp(y + ((454 * d + 128) >> 8), 0, 255);
	return (u32)((b << 16) | (g << 8) | r);
}

// The converter reads its planes 8 bytes at a time: from a plane that starts 4 bytes into an
// 8-byte block, the second half of each block comes from the block before (jpeg/mjpegcsc's
// patterns).
u8 MJpegPlaneByte(u32 base, u32 i) {
	const u32 addr = base + i - ((base & 7) == 4 && (i & 4) ? 8 : 0);
	return GRead8(addr);
}

// sceJpegCsc: JFIF (full range BT.601), rounded (jpeg/csc "Bits").
u32 JfifYCbCrToABGR(int y, int cb, int cr) {
	const int d = cb - 128, e = cr - 128;
	int r = y + ((91881 * e + 32768) >> 16);
	int g = y + ((-22554 * d - 46802 * e + 32768) >> 16);
	int b = y + ((116130 * d + 32768) >> 16);
	r = std::min(255, std::max(0, r));
	g = std::min(255, std::max(0, g));
	b = std::min(255, std::max(0, b));
	return (u32)((b << 16) | (g << 8) | r);
}

int JpegCsc(u32 imageAddr, u32 yCbCrAddr, int widthHeight, int bufferWidth, u32 chroma, int &usec) {
	if ((chroma & 0x000FFFFF) != 0x00020202 && (chroma & 0x000FFFFF) != 0x00020201 && (chroma & 0x000FFFFF) != 0x00020101)
		return (int)SCE_JPEG_ERROR_INVALID_COLORSPACE;
	if (bufferWidth < 0) bufferWidth = 0;

	int height = widthHeight & 0xFFFF;
	const int width = (widthHeight >> 16) & 0xFFFF;
	if (height == 0) height = 1;

	const u8 widthShift = (u8)(((chroma >> 8) & 0x03) - 1);
	const u8 heightShift = (u8)((chroma & 0x03) - 1);
	const int sizeY = width * height;
	const int sizeCb = sizeY >> (widthShift + heightShift);

	const u64 destSize = ((u64)bufferWidth * (height - 1) + width) * 4;
	if (destSize > 0x3FFFFFFF || !mem::IsValidRange(imageAddr, (u32)destSize)) return (int)SCE_JPEG_ERROR_INVALID_VALUE;
	if (sizeY > 0x3FFFFFFF || !mem::IsValidRange(yCbCrAddr, (u32)(sizeY + sizeCb + sizeCb)))
		return (int)SCE_JPEG_ERROR_INVALID_VALUE;

	u32 *imageBuffer = (u32 *)GPtr(imageAddr);
	const u8 *Y = GPtr(yCbCrAddr);
	const u8 *Cb = Y + sizeY;
	const u8 *Cr = Cb + sizeCb;

	// Very approximate, from tests on a PSP. Usually under.
	usec += 60 + 6 * height + width / 2 + width / 4;

	// 2x2 blocks share one chroma sample only in 4:2:0 (0x20202); 0x20201 and 0x20101 have a
	// chroma row per pixel row (jpeg/csc "short cb/cr").
	if ((chroma & 0x000FFFFF) == 0x00020202 && (widthHeight & 0x00010001) == 0 && height > 1) {
		for (int y = 0; y < height; y += 2) {
			for (int x = 0; x < width; x += 2) {
				const u8 y0 = Y[width * y + x];
				const u8 y1 = Y[width * y + x + 1];
				const u8 y2 = Y[width * (y + 1) + x];
				const u8 y3 = Y[width * (y + 1) + x + 1];
				const u8 cb = Cb[(width >> widthShift) * (y >> heightShift) + (x >> widthShift)];
				const u8 cr = Cr[(width >> widthShift) * (y >> heightShift) + (x >> widthShift)];
				imageBuffer[bufferWidth * y + x] = JfifYCbCrToABGR(y0, cb, cr);
				imageBuffer[bufferWidth * y + x + 1] = JfifYCbCrToABGR(y1, cb, cr);
				imageBuffer[bufferWidth * (y + 1) + x] = JfifYCbCrToABGR(y2, cb, cr);
				imageBuffer[bufferWidth * (y + 1) + x + 1] = JfifYCbCrToABGR(y3, cb, cr);
			}
		}
	} else {
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const u8 yy = Y[width * y + x];
				const u8 cb = Cb[(width >> widthShift) * (y >> heightShift) + (x >> widthShift)];
				const u8 cr = Cr[(width >> widthShift) * (y >> heightShift) + (x >> widthShift)];
				imageBuffer[bufferWidth * y + x] = JfifYCbCrToABGR(yy, cb, cr);
			}
		}
	}
	if ((widthHeight & 0xFFFF) == 0) return -1;
	return 0;
}

int JpegMJpegCsc(u32 imageAddr, u32 yCbCrAddr, int widthHeight, int bufferWidth, int &usec) {
	int height = widthHeight & 0xFFFF;
	const int width = (widthHeight >> 16) & 0xFFFF;
	if (bufferWidth < 0) bufferWidth = bufferWidth > -901 ? 901 + bufferWidth : 0;
	if (height == 0) height = 1;

	const int sizeY = width * height;
	const int sizeCb = sizeY >> 2;
	if (width > 720 || height > 480) return (int)SCE_JPEG_ERROR_INVALID_SIZE;
	if (bufferWidth > 1024) return (int)SCE_JPEG_ERROR_INVALID_SIZE;
	const u32 destSize = (u32)((bufferWidth * (height - 1) + width) * 4);
	if (!mem::IsValidRange(imageAddr, destSize)) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;

	u32 *imageBuffer = (u32 *)GPtr(imageAddr);
	const u32 yAddr = yCbCrAddr, cbAddr = yAddr + (u32)sizeY, crAddr = cbAddr + (u32)sizeCb;

	// The PSP behaves strangely for heights below 16, not rescheduling and writing fewer bytes.
	if (height >= 16) usec += 9 * height;

	if (!mem::IsValidRange(yCbCrAddr, (u32)(sizeY + sizeCb + sizeCb))) {
		// Seems to write based on zeros.
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) imageBuffer[bufferWidth * y + x] = MJpegYCbCrToABGR(0, 0, 0);
		}
	} else {
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const u32 c = (u32)((width >> 1) * (y >> 1) + (x >> 1));
				imageBuffer[bufferWidth * y + x] = MJpegYCbCrToABGR(MJpegPlaneByte(yAddr, (u32)(width * y + x)),
				                                                    MJpegPlaneByte(cbAddr, c), MJpegPlaneByte(crAddr, c));
			}
		}
	}
	return 0;
}

int sceJpegMJpegCsc(u32 imageAddr, u32 yCbCrAddr, int widthHeight, int bufferWidth) {
	if (g_mjpegInited == 0) return (int)0x80000001;
	int usec = 0;
	const int result = JpegMJpegCsc(imageAddr, yCbCrAddr, widthHeight, bufferWidth, usec);
	if (result >= 0) g_cscSinceInitFinish = true;
	if (usec != 0) return (int)Delay((u32)result, usec);
	return result;
}

// Decodes and checks what the hardware can decode. Returns 0 with the RGB pixels, or an error.
int DecodeToRgb(u32 jpegAddr, int jpegSize, int *width, int *height, std::vector<u8> *rgb) {
	const u8 *buf = GPtr(jpegAddr);
	if (jpegSize < 2 || buf[0] != 0xFF || buf[1] != 0xD8) return (int)SCE_JPEG_ERROR_NO_SOI;
	media::JpegInfo info;
	if (!media::ParseJpegHeader(buf, (size_t)jpegSize, &info)) return (int)SCE_JPEG_ERROR_INVALID_DATA;
	if (info.components != 3 || info.hSamp[0] != 2 || info.vSamp[0] != 2 || info.hSamp[1] != 1 || info.vSamp[1] != 1 ||
	    info.hSamp[2] != 1 || info.vSamp[2] != 1)
		return (int)SCE_JPEG_ERROR_UNSUPPORTED_SAMPLING;
	if (!media::DecodeJpegRgb(buf, (size_t)jpegSize, width, height, rgb)) return (int)SCE_JPEG_ERROR_INVALID_DATA;
	return 0;
}

int DecodeJpeg(u32 jpegAddr, int jpegSize, u32 imageAddr, int &usec) {
	if (!mem::IsValidRange(jpegAddr, (u32)jpegSize)) return (int)SCE_JPEG_ERROR_NO_SOI;
	if (jpegSize == 0) return (int)SCE_JPEG_ERROR_INVALID_DATA;

	int width = 0, height = 0;
	std::vector<u8> rgb;
	const int err = DecodeToRgb(jpegAddr, jpegSize, &width, &height, &rgb);
	if (err != 0) return err;

	usec += (width * height) / 14;

	if (!mem::IsValidRange(imageAddr, (u32)(g_mjpegWidth * g_mjpegHeight * 4))) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	// Even after a Delete, the size is still allowed.
	if (width > g_mjpegWidth || height > g_mjpegHeight) return (int)SCE_JPEG_ERROR_INVALID_SIZE;
	// Finish after setting the size, then this: an interesting error.
	if (g_mjpegInited == 0) return (int)0x80000001;

	usec += (width * height) / 110;

	const u32 rowsSpan = (u32)(((u64)g_mjpegWidth * (height - 1) + width) * 4);
	if (u32 *abgr = (u32 *)mem::GetPointer(imageAddr, rowsSpan)) {
		const u8 *src = rgb.data();
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; x++) {
				const u8 *p = src + (size_t)x * 3;
				abgr[x] = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
			}
			src += (size_t)width * 3;
			abgr += g_mjpegWidth;
		}
	}
	return getWidthHeight(width, height);
}

int sceJpegDecodeMJpeg(u32 jpegAddr, int jpegSize, u32 imageAddr, int dhtMode) {
	(void)dhtMode;
	if ((jpegAddr | (u32)jpegSize | (jpegAddr + (u32)jpegSize)) & 0x80000000) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if (imageAddr & 0x80000000) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	int usec = 300;
	const int result = DecodeJpeg(jpegAddr, jpegSize, imageAddr, usec);
	return (int)Delay((u32)result, usec);
}

int sceJpegDecodeMJpegSuccessively(u32 jpegAddr, int jpegSize, u32 imageAddr, int dhtMode) {
	return sceJpegDecodeMJpeg(jpegAddr, jpegSize, imageAddr, dhtMode);
}

int sceJpegCsc(u32 imageAddr, u32 yCbCrAddr, int widthHeight, int bufferWidth, int colourInfo) {
	int usec = 0;
	const int result = JpegCsc(imageAddr, yCbCrAddr, widthHeight, bufferWidth, (u32)colourInfo, usec);
	if (result >= 0) g_cscSinceInitFinish = true;
	if (usec != 0) return (int)Delay((u32)result, usec);
	return result;
}

int getYCbCrBufferSize(int w, int h) {
	// 12 bits per pixel.
	return ((w * h) >> 1) * 3;
}

int JpegGetOutputInfo(u32 jpegAddr, int jpegSize, u32 colourInfoAddr) {
	if (!mem::IsValidRange(jpegAddr, (u32)jpegSize)) return (int)SCE_JPEG_ERROR_NO_SOI;
	if (jpegSize == 0) return (int)SCE_JPEG_ERROR_INVALID_DATA;
	int width = 0, height = 0;
	std::vector<u8> rgb;
	const int err = DecodeToRgb(jpegAddr, jpegSize, &width, &height, &rgb);
	if (err != 0) return err;

	// Bits 16-23: colour mode (1 greyscale, 2 YCbCr); 8-15 and 0-7 the chroma subsampling.
	// Nothing else seems to work on a PSP.
	if ((colourInfoAddr & 3) == 0 && mem::IsValidRange(colourInfoAddr, 4)) GWrite32(colourInfoAddr, 0x00020202);
	return getYCbCrBufferSize(width, height);
}

int sceJpegGetOutputInfo(u32 jpegAddr, int jpegSize, u32 colourInfoAddr, int dhtMode) {
	(void)dhtMode;
	if ((jpegAddr | (u32)jpegSize | (jpegAddr + (u32)jpegSize)) & 0x80000000) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	const int result = JpegGetOutputInfo(jpegAddr, jpegSize, colourInfoAddr);
	// Time taken varies a bit with the data; this is the low end. Errors delay as well.
	return (int)Delay((u32)result, 250);
}

u32 convertRGBToYCbCr(const u8 *p) {
	const u8 r = p[0], g = p[1], b = p[2];
	int y = (int)(0.299f * r + 0.587f * g + 0.114f * b + 0);
	int cb = (int)(-0.169f * r - 0.331f * g + 0.499f * b + 128.0f);
	int cr = (int)(0.499f * r - 0.418f * g - 0.0813f * b + 128.0f);
	y = std::min(255, std::max(0, y));
	cb = std::min(255, std::max(0, cb));
	cr = std::min(255, std::max(0, cr));
	return (u32)((y << 16) | (cb << 8) | cr);
}

void JpegConvertRGBToYCbCr(const u8 *rgb, u8 *output, int width, int height) {
	const int sizeY = width * height;
	const int sizeCb = sizeY >> 2;
	u8 *Y = output;
	u8 *Cb = Y + sizeY;
	u8 *Cr = Cb + sizeCb;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const u32 yCbCr = convertRGBToYCbCr(rgb + ((size_t)width * y + x) * 3);
			Y[width * y + x] = (u8)(yCbCr >> 16);
			if ((y & 1) == 0 && (x & 1) == 0) {
				// These came from a JPEG, so the 2x2 block's chroma ought to match.
				Cb[(width >> 1) * (y >> 1) + (x >> 1)] = (u8)(yCbCr >> 8);
				Cr[(width >> 1) * (y >> 1) + (x >> 1)] = (u8)yCbCr;
			}
		}
	}
}

int JpegDecodeMJpegYCbCr(u32 jpegAddr, int jpegSize, u32 yCbCrAddr, int yCbCrSize, int &usec) {
	if (!mem::IsValidRange(jpegAddr, (u32)jpegSize)) return (int)SCE_JPEG_ERROR_NO_SOI;
	if (jpegSize == 0) return (int)SCE_JPEG_ERROR_INVALID_DATA;
	int width = 0, height = 0;
	std::vector<u8> rgb;
	const int err = DecodeToRgb(jpegAddr, jpegSize, &width, &height, &rgb);
	if (err != 0) return err;
	if (yCbCrSize < getYCbCrBufferSize(width, height)) return (int)SCE_JPEG_ERROR_OUT_OF_MEMORY;

	if (u8 *out = mem::GetPointer(yCbCrAddr, (u32)getYCbCrBufferSize(width, height))) {
		JpegConvertRGBToYCbCr(rgb.data(), out, width, height);
	} else {
		// The PSP writes data around the last passed address.
		LOGW("JPEG", "JpegDecodeMJpegYCbCr: invalid output address (%08x / %08x) for %dx%d", yCbCrAddr, yCbCrSize, width,
		     height);
	}
	// Rough estimate based on observed timing.
	usec += (width * height) / 14;
	return getWidthHeight(width, height);
}

int sceJpegDecodeMJpegYCbCr(u32 jpegAddr, int jpegSize, u32 yCbCrAddr, int yCbCrSize, int dhtMode) {
	(void)dhtMode;
	if ((jpegAddr | (u32)jpegSize | (jpegAddr + (u32)jpegSize)) & 0x80000000) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if ((yCbCrAddr | (u32)yCbCrSize | (yCbCrAddr + (u32)yCbCrSize)) & 0x80000000) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if (!mem::IsValidRange(jpegAddr, (u32)jpegSize)) return (int)SCE_JPEG_ERROR_INVALID_VALUE;
	int usec = 300;
	const int result = JpegDecodeMJpegYCbCr(jpegAddr, jpegSize, yCbCrAddr, yCbCrSize, usec);
	return (int)Delay((u32)result, usec);
}

int sceJpegDecodeMJpegYCbCrSuccessively(u32 jpegAddr, int jpegSize, u32 yCbCrAddr, int yCbCrSize, int dhtMode) {
	(void)dhtMode;
	if ((jpegAddr | (u32)jpegSize | (jpegAddr + (u32)jpegSize)) & 0x80000000) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if ((yCbCrAddr | (u32)yCbCrSize | (yCbCrAddr + (u32)yCbCrSize)) & 0x80000000) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	// The same as sceJpegDecodeMJpegYCbCr() but in smaller blocks, and an output it can't write
	// to is an error (jpeg/decodeycbcrs).
	int usec = 300;
	if (mem::IsValidRange(jpegAddr, (u32)jpegSize) && jpegSize >= 2 && !mem::IsValidAddress(yCbCrAddr)) {
		int width = 0, height = 0;
		std::vector<u8> rgb;
		const int err = DecodeToRgb(jpegAddr, jpegSize, &width, &height, &rgb);
		if (err != 0) return (int)Delay((u32)err, usec);
		if (yCbCrSize < getYCbCrBufferSize(width, height)) return (int)Delay(SCE_JPEG_ERROR_OUT_OF_MEMORY, usec);
		return (int)Delay(SCE_JPEG_ERROR_INVALID_OUTPUT, usec);
	}
	const int result = JpegDecodeMJpegYCbCr(jpegAddr, jpegSize, yCbCrAddr, yCbCrSize, usec);
	return (int)Delay((u32)result, usec);
}

int sceJpegCreateMJpeg(int width, int height) {
	if (g_mjpegInited == 0) return (int)SCE_JPEG_ERROR_INVALID_STATE;
	if (g_mjpegInited == 2) return (int)SCE_JPEG_ERROR_INVALID_STATE;
	if (width > 1024) return (int)SCE_JPEG_ERROR_INVALID_SIZE;
	g_mjpegInited = 2;
	g_mjpegWidth = width;
	g_mjpegHeight = height;
	return 0;
}

int sceJpegDeleteMJpeg() {
	if (g_mjpegInited == 0) return (int)SCE_JPEG_ERROR_INVALID_STATE;
	if (g_mjpegInited == 1) return (int)SCE_JPEG_ERROR_INVALID_STATE;
	g_mjpegInited = 1;
	return 0;
}

int sceJpegInitMJpeg() {
	if (g_mjpegInited == 1 || g_mjpegInited == 2) return (int)SCE_JPEG_ERROR_ALREADY_INIT;
	g_mjpegInited = 1;
	const bool wait = !g_cscSinceInitFinish;
	g_cscSinceInitFinish = false;
	return wait ? (int)Delay(0, 130) : 0;
}

int sceJpegFinishMJpeg() {
	if (g_mjpegInited == 0) return (int)SCE_JPEG_ERROR_INVALID_STATE;
	if (g_mjpegInited == 2) return (int)SCE_JPEG_ERROR_INVALID_STATE;
	g_mjpegInited = 0;
	const bool wait = !g_cscSinceInitFinish;
	g_cscSinceInitFinish = false;
	return wait ? (int)Delay(0, 120) : 0;
}

int sceJpegUnimplemented() {
	LOGW("JPEG", "UNIMPL %s()", hle::CurrentFunctionName());
	return 0;
}

}  // namespace

void Register_sceJpeg() {
	static const hle::HleFunction funcs[] = {
	    hle::HleFunction{0x0425B986, &hle::Wrap<&sceJpegUnimplemented>, "sceJpegDecompressAllImage", 0},
	    HLEFN(0x04B5AE02, sceJpegMJpegCsc),
	    HLEFN(0x04B93CEF, sceJpegDecodeMJpeg),
	    HLEFN(0x227662D7, sceJpegDecodeMJpegYCbCrSuccessively),
	    HLEFN(0x48B602B7, sceJpegDeleteMJpeg),
	    HLEFN(0x64B6F978, sceJpegDecodeMJpegSuccessively),
	    HLEFN(0x67F0ED84, sceJpegCsc),
	    HLEFN(0x7D2F3D7F, sceJpegFinishMJpeg),
	    HLEFN(0x8F2BB012, sceJpegGetOutputInfo),
	    HLEFN(0x91EED83C, sceJpegDecodeMJpegYCbCr),
	    hle::HleFunction{0x9B36444C, &hle::Wrap<&sceJpegUnimplemented>, "sceJpeg_9B36444C", 0},
	    HLEFN(0x9D47469C, sceJpegCreateMJpeg),
	    HLEFN(0xAC9E70E6, sceJpegInitMJpeg),
	    hle::HleFunction{0xA06A75C4, &hle::Wrap<&sceJpegUnimplemented>, "sceJpegMJpegCscWithColorOption", 0},
	};
	hle::RegisterModule("sceJpeg", funcs);
}

}  // namespace mediahle
