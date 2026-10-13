// SPDX-License-Identifier: GPL-2.0-or-later
#include "media/h264_decoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
namespace media {
namespace {
struct Decoder {
	AVCodecContext *codec = nullptr;
	AVFrame *frame = av_frame_alloc();
	SwsContext *scale = nullptr;
	~Decoder() { sws_freeContext(scale); av_frame_free(&frame); avcodec_free_context(&codec); }
};
}
H264Decoder::~H264Decoder() { Close(); }
bool H264Decoder::Available() { return avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr; }
bool H264Decoder::Open() {
	Close();
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) return false;
	auto *d = new Decoder;
	d->codec = avcodec_alloc_context3(codec);
	if (!d->codec || !d->frame) { delete d; return false; }
	d->codec->pkt_timebase = AVRational{1, 90000};
	d->codec->thread_count = 1;
	if (avcodec_open2(d->codec, codec, nullptr) < 0) { delete d; return false; }
	transform_ = d; decoded_ = 0; displayW_ = displayH_ = 0;
	return true;
}
void H264Decoder::Close() { delete (Decoder *)transform_; transform_ = nullptr; frames_.clear(); }
void H264Decoder::Flush() {
	if (auto *d = (Decoder *)transform_) avcodec_flush_buffers(d->codec);
	frames_.clear();
}
void H264Decoder::CollectOutput() {
	auto &d = *(Decoder *)transform_;
	while (avcodec_receive_frame(d.codec, d.frame) == 0) {
		AVFrame &f = *d.frame;
		if (f.width <= 0 || f.height <= 0 || f.width > 4096 || f.height > 4096 || (f.width & 1) || (f.height & 1)) {
			av_frame_unref(d.frame); continue;
		}
		YuvFrame out;
		out.width = displayW_ = f.width; out.height = displayH_ = f.height;
		out.pts = f.pts == AV_NOPTS_VALUE ? -1 : f.pts;
		out.y.resize((size_t)f.width * f.height);
		out.cb.resize(out.y.size() / 4); out.cr.resize(out.cb.size());
		d.scale = sws_getCachedContext(d.scale, f.width, f.height, (AVPixelFormat)f.format,
		                              f.width, f.height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
		u8 *dest[] = {out.y.data(), out.cb.data(), out.cr.data(), nullptr};
		int stride[] = {f.width, f.width / 2, f.width / 2, 0};
		if (d.scale && sws_scale(d.scale, f.data, f.linesize, 0, f.height, dest, stride) == f.height) {
			frames_.push_back(std::move(out)); ++decoded_;
		}
		av_frame_unref(d.frame);
	}
}
bool H264Decoder::Decode(const u8 *data, int size, s64 pts) {
	if (!transform_ || !data || size <= 0) return false;
	auto &d = *(Decoder *)transform_;
	AVPacket *packet = av_packet_alloc();
	if (!packet) return false;
	bool ok = av_new_packet(packet, size) >= 0;
	if (ok) {
		memcpy(packet->data, data, size); packet->pts = pts < 0 ? AV_NOPTS_VALUE : pts;
		int rc = avcodec_send_packet(d.codec, packet);
		if (rc == AVERROR(EAGAIN)) { CollectOutput(); rc = avcodec_send_packet(d.codec, packet); }
		ok = rc >= 0;
		if (ok) CollectOutput();
	}
	av_packet_free(&packet);
	return ok;
}
void H264Decoder::Drain() {
	if (!transform_) return;
	auto &d = *(Decoder *)transform_;
	int rc = avcodec_send_packet(d.codec, nullptr);
	if (rc == AVERROR(EAGAIN)) { CollectOutput(); avcodec_send_packet(d.codec, nullptr); }
	CollectOutput();
}
bool H264Decoder::PopFrame(YuvFrame *out) {
	if (frames_.empty()) return false;
	*out = std::move(frames_.front()); frames_.pop_front(); return true;
}
}
