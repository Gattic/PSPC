// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/sha1.h"

static inline u32 Rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

Sha1::Sha1() {
	h_[0] = 0x67452301;
	h_[1] = 0xEFCDAB89;
	h_[2] = 0x98BADCFE;
	h_[3] = 0x10325476;
	h_[4] = 0xC3D2E1F0;
}

void Sha1::Block(const u8 *p) {
	u32 w[80];
	for (int i = 0; i < 16; i++) {
		w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	}
	for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
	for (int i = 0; i < 80; i++) {
		u32 f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		u32 t = Rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = Rol(b, 30);
		b = a;
		a = t;
	}
	h_[0] += a;
	h_[1] += b;
	h_[2] += c;
	h_[3] += d;
	h_[4] += e;
}

void Sha1::Update(const void *data, size_t size) {
	const u8 *p = (const u8 *)data;
	total_ += size;
	while (size > 0) {
		size_t take = 64 - used_;
		if (take > size) take = size;
		memcpy(buf_ + used_, p, take);
		used_ += take;
		p += take;
		size -= take;
		if (used_ == 64) {
			Block(buf_);
			used_ = 0;
		}
	}
}

std::array<u8, 20> Sha1::Final() {
	u64 bits = total_ * 8;
	u8 pad = 0x80;
	Update(&pad, 1);
	u8 zero = 0;
	while (used_ != 56) Update(&zero, 1);
	u8 len[8];
	for (int i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - i * 8));
	Update(len, 8);
	std::array<u8, 20> out;
	for (int i = 0; i < 5; i++) {
		out[i * 4] = (u8)(h_[i] >> 24);
		out[i * 4 + 1] = (u8)(h_[i] >> 16);
		out[i * 4 + 2] = (u8)(h_[i] >> 8);
		out[i * 4 + 3] = (u8)h_[i];
	}
	return out;
}

std::array<u8, 20> Sha1::Hash(const void *data, size_t size) {
	Sha1 s;
	s.Update(data, size);
	return s.Final();
}

u32 ComputeNid(const std::string &name) {
	auto d = Sha1::Hash(name.data(), name.size());
	return (u32)d[0] | ((u32)d[1] << 8) | ((u32)d[2] << 16) | ((u32)d[3] << 24);
}
