// SPDX-License-Identifier: GPL-2.0-or-later
// Linux implementation of the frame-decoder interface. ATRAC remains the standalone decoder.
#include "audio/mf_decoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
namespace audio {
namespace {
struct Decoder {
	AVCodecContext *codec = nullptr;
	AVFrame *frame = av_frame_alloc();
	SwrContext *swr = nullptr;
	~Decoder() { swr_free(&swr); av_frame_free(&frame); avcodec_free_context(&codec); }
};
}
bool MFAvailable() { return avcodec_find_decoder(AV_CODEC_ID_MP3) && avcodec_find_decoder(AV_CODEC_ID_AAC); }
MFDecoder::~MFDecoder() { Close(); }
bool MFDecoder::Open(Format fmt, int rate, int channels, const u8 *asc, int ascSize) {
	Close();
	const AVCodec *codec = avcodec_find_decoder(fmt == Format::MP3 ? AV_CODEC_ID_MP3 : AV_CODEC_ID_AAC);
	if (!codec || rate <= 0 || channels < 1 || channels > 8 || ascSize < 0) return false;
	auto *d = new Decoder;
	d->codec = avcodec_alloc_context3(codec);
	if (!d->codec || !d->frame) { delete d; return false; }
	d->codec->sample_rate = rate;
	av_channel_layout_default(&d->codec->ch_layout, channels);
	if (fmt == Format::AAC_RAW && asc && ascSize) {
		d->codec->extradata = (u8 *)av_mallocz((size_t)ascSize + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!d->codec->extradata) { delete d; return false; }
		memcpy(d->codec->extradata, asc, ascSize); d->codec->extradata_size = ascSize;
	}
	if (avcodec_open2(d->codec, codec, nullptr) < 0) { delete d; return false; }
	transform_ = d; sampleRate_ = rate; channels_ = outChannels_ = channels;
	return true;
}
void MFDecoder::Close() { delete (Decoder *)transform_; transform_ = nullptr; }
void MFDecoder::Flush() {
	if (auto *d = (Decoder *)transform_) { avcodec_flush_buffers(d->codec); swr_free(&d->swr); }
}
bool MFDecoder::Drain(std::vector<s16> &out) {
	auto &d = *(Decoder *)transform_;
	for (;;) {
		int rc = avcodec_receive_frame(d.codec, d.frame);
		if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
		if (rc < 0) return false;
		AVFrame &f = *d.frame;
		outChannels_ = f.ch_layout.nb_channels;
		sampleRate_ = f.sample_rate;
		if (!d.swr) {
			if (swr_alloc_set_opts2(&d.swr, &f.ch_layout, AV_SAMPLE_FMT_S16, f.sample_rate,
			                       &f.ch_layout, (AVSampleFormat)f.format, f.sample_rate, 0, nullptr) < 0 ||
			    swr_init(d.swr) < 0) return false;
		}
		size_t old = out.size();
		int capacity = swr_get_out_samples(d.swr, f.nb_samples);
		if (capacity < 0) return false;
		out.resize(old + (size_t)capacity * outChannels_);
		u8 *dest = (u8 *)(out.data() + old);
		int count = swr_convert(d.swr, &dest, capacity, (const u8 **)f.extended_data, f.nb_samples);
		if (count < 0) { out.resize(old); return false; }
		out.resize(old + (size_t)count * outChannels_);
		av_frame_unref(d.frame);
	}
}
bool MFDecoder::Decode(const u8 *data, int size, std::vector<s16> &out) {
	if (!transform_ || !data || size <= 0) return false;
	auto &d = *(Decoder *)transform_;
	AVPacket *packet = av_packet_alloc();
	if (!packet) return false;
	bool ok = av_new_packet(packet, size) >= 0;
	if (ok) {
		memcpy(packet->data, data, size);
		int rc = avcodec_send_packet(d.codec, packet);
		if (rc == AVERROR(EAGAIN)) {
			ok = Drain(out);
			if (ok) rc = avcodec_send_packet(d.codec, packet);
		}
		ok = ok && rc >= 0 && Drain(out);
	}
	av_packet_free(&packet);
	return ok;
}
}
