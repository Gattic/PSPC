// SPDX-License-Identifier: GPL-2.0-or-later
//
// H.264 decoding through the Media Foundation H.264 decoder transform that ships with Windows
// (CLSID_MSH264DecoderMFT). Input is one Annex B access unit at a time, as the PSP's Media
// Engine is fed; output is planar YCbCr 4:2:0 pictures in display order.
#pragma once

#include <deque>
#include <vector>

#include "common/common.h"

namespace media {

struct YuvFrame {
	int width = 0;   // display size
	int height = 0;
	std::vector<u8> y;   // width * height
	std::vector<u8> cb;  // (width / 2) * (height / 2)
	std::vector<u8> cr;
	s64 pts = -1;        // 90kHz units, as given to Decode

	bool Valid() const { return width > 0 && height > 0 && !y.empty(); }
	const u8 *Plane(int i) const { return i == 0 ? y.data() : (i == 1 ? cb.data() : cr.data()); }
	int Stride(int i) const { return i == 0 ? width : width / 2; }
};

class H264Decoder {
public:
	H264Decoder() = default;
	~H264Decoder();
	H264Decoder(const H264Decoder &) = delete;
	H264Decoder &operator=(const H264Decoder &) = delete;

	bool Open();
	void Close();
	bool IsOpen() const { return transform_ != nullptr; }

	// Feeds one access unit. pts is in 90kHz units (-1 if unknown); it comes back on the picture.
	bool Decode(const u8 *data, int size, s64 pts);
	// The next decoded picture, if any.
	bool PopFrame(YuvFrame *out);
	size_t PendingFrames() const { return frames_.size(); }
	// End of stream: pictures held back for reordering come out.
	void Drain();
	// Drops everything held (seeking, or a new stream).
	void Flush();

	int Width() const { return displayW_; }
	int Height() const { return displayH_; }
	int DecodedCount() const { return decoded_; }

	// Whether Media Foundation and its H.264 decoder can be used on this machine.
	static bool Available();

private:
	bool SetOutputType();
	void CollectOutput();

	void *transform_ = nullptr;  // IMFTransform*
	bool providesSamples_ = false;
	int frameW_ = 0, frameH_ = 0;      // buffer layout
	int displayW_ = 0, displayH_ = 0;  // visible picture
	int stride_ = 0;
	int format_ = 0;  // 0 = NV12, 1 = I420, 2 = YV12
	s64 lastTime_ = 0;
	int decoded_ = 0;
	std::deque<YuvFrame> frames_;
};

}  // namespace media
