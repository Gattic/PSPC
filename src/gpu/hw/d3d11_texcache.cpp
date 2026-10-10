// SPDX-License-Identifier: GPL-2.0-or-later
//
// The D3D11 backend's texture cache. Textures are decoded to RGBA8888 with the software sampler's own
// texel fetch (sampler.h FetchTexelT: every format, CLUT4/8/16/32 with shift/mask/offset and per-level
// palettes, DXT1/3/5, swizzling, reads past the end of memory), so they match the reference renderer
// texel for texel. An entry is keyed by address, format, size and CLUT contents, and validated by
// hashing its texel data once per GE run (the CPU may have rewritten it in between), or when the GE
// itself wrote the memory (block transfer, rendering written back).
#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "gpu/hw/d3d11_internal.h"
#include "gpu/sampler.h"

namespace gpu::hw {

namespace {

inline u64 Rotl(u64 x, int r) {
	return (x << r) | (x >> (64 - r));
}

constexpr u64 P1 = 0x9E3779B185EBCA87ULL;
constexpr u64 P2 = 0xC2B2AE3D27D4EB4FULL;
constexpr u64 P3 = 0x165667B19E3779F9ULL;

inline u64 Round(u64 acc, u64 v) {
	return Rotl(acc + v * P2, 31) * P1;
}

typedef void (*DecodeFn)(const SamplerState &ss, int level, int w, int h, bool clampS, bool clampT, u32 *out);

// One level, with the sampler's wrapping and clamping inside 512 texels (sampler.h ClampUV/WrapUV)
// applied to sizes above 512.
template <int fmt, bool swz>
void DecodeLevelT(const SamplerState &ss, int level, int w, int h, bool clampS, bool clampT, u32 *out) {
	for (int v = 0; v < h; ++v) {
		const int vv = v > 511 ? (clampT ? 511 : (v & 511)) : v;
		u32 *row = out + (size_t)v * w;
		for (int u = 0; u < w; ++u) {
			const int uu = u > 511 ? (clampS ? 511 : (u & 511)) : u;
			row[u] = FetchTexelT<fmt, swz>(ss, level, uu, vv);
		}
	}
}

template <int fmt>
DecodeFn PickDecode(bool swizzle) {
	return swizzle ? &DecodeLevelT<fmt, true> : &DecodeLevelT<fmt, false>;
}

DecodeFn GetDecodeFn(int texfmt, bool swizzle) {
	switch (texfmt) {
	case GE_TFMT_5650: return PickDecode<GE_TFMT_5650>(swizzle);
	case GE_TFMT_5551: return PickDecode<GE_TFMT_5551>(swizzle);
	case GE_TFMT_4444: return PickDecode<GE_TFMT_4444>(swizzle);
	case GE_TFMT_8888: return PickDecode<GE_TFMT_8888>(swizzle);
	case GE_TFMT_CLUT4: return PickDecode<GE_TFMT_CLUT4>(swizzle);
	case GE_TFMT_CLUT8: return PickDecode<GE_TFMT_CLUT8>(swizzle);
	case GE_TFMT_CLUT16: return PickDecode<GE_TFMT_CLUT16>(swizzle);
	case GE_TFMT_CLUT32: return PickDecode<GE_TFMT_CLUT32>(swizzle);
	// DXT isn't swizzled; invalid formats have no texel pointer and read 0 (sampler.cpp).
	case GE_TFMT_DXT1: return &DecodeLevelT<GE_TFMT_DXT1, false>;
	case GE_TFMT_DXT3: return &DecodeLevelT<GE_TFMT_DXT3, false>;
	default: return &DecodeLevelT<GE_TFMT_DXT5, false>;
	}
}

// The bytes a level's texel lookups can reach (binner.cpp AddTextureReads).
u32 LevelBytes(const SamplerState &ss, const TexLevel &lv) {
	if (!lv.ptr)
		return 0;
	const u32 h = std::min<u32>(lv.h, 512);
	const u32 rows = ss.swizzle ? ((h + 7) & ~7u) : h;
	return std::min<u32>((u32)((u64)rows * lv.bufw * kTexelBits[ss.texfmt] / 8), lv.validBytes);
}

}  // namespace

u64 HashBytes(const u8 *p, size_t n, u64 seed) {
	u64 a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
	size_t i = 0;
	for (; i + 32 <= n; i += 32) {
		u64 w[4];
		memcpy(w, p + i, 32);
		a = Round(a, w[0]);
		b = Round(b, w[1]);
		c = Round(c, w[2]);
		d = Round(d, w[3]);
	}
	u64 h = Rotl(a, 1) + Rotl(b, 7) + Rotl(c, 12) + Rotl(d, 18) + (u64)n;
	for (; i + 8 <= n; i += 8) {
		u64 w;
		memcpy(&w, p + i, 8);
		h ^= Round(0, w);
		h = Rotl(h, 27) * P1 + P3;
	}
	for (; i < n; ++i) {
		h ^= p[i] * P3;
		h = Rotl(h, 11) * P1;
	}
	h ^= h >> 33;
	h *= P2;
	h ^= h >> 29;
	h *= P3;
	h ^= h >> 32;
	return h;
}

size_t TextureCache::KeyHash::operator()(const Key &k) const {
	u64 h = (u64)k.addr * P1;
	h ^= ((u64)k.fmt | ((u64)k.levels << 8) | ((u64)k.flags << 16) | ((u64)k.bufw << 24) | ((u64)k.w << 40)) * P2;
	h ^= ((u64)k.h << 7) ^ k.clutHash;
	return (size_t)(h ^ (h >> 29));
}

ID3D11ShaderResourceView *TextureCache::Lookup(const SamplerState &ss, u64 runSeq, void (*syncRange)(void *, u32, u32), void *user, int *levelsOut) {
	// A mip chain the GPU can use: each level half the previous one. Otherwise level 0 only.
	int levels = 1;
	for (int i = 1; i <= ss.maxLevel; ++i) {
		const TexLevel &a = ss.levels[i - 1], &b = ss.levels[i];
		if (b.w != std::max(1, a.w / 2) || b.h != std::max(1, a.h / 2))
			break;
		levels = i + 1;
	}
	const TexLevel &l0 = ss.levels[0];
	const int w = std::min<int>(l0.w, 1024), h = std::min<int>(l0.h, 1024);
	Key k{};
	k.addr = l0.addr;
	k.fmt = ss.texfmt;
	k.levels = (u8)levels;
	k.flags = (u8)((ss.swizzle ? 1 : 0) | (w > 512 && ss.clampS ? 2 : 0) | (h > 512 && ss.clampT ? 4 : 0) | (ss.useSharedClut ? 8 : 0));
	k.bufw = l0.bufw;
	k.w = (u16)w;
	k.h = (u16)h;
	const bool indexed = ss.texfmt >= GE_TFMT_CLUT4 && ss.texfmt <= GE_TFMT_CLUT32;
	if (indexed) {
		const u64 params = (u64)ss.clutfmt | ((u64)ss.clutShift << 8) | ((u64)ss.clutMask << 16) | ((u64)ss.clutOffset << 24);
		k.clutHash = HashBytes(ss.clut, 1024, params) | 1;
	}
	for (int i = 1; i < levels; ++i) {
		// Other levels' addresses are part of what's decoded.
		k.clutHash ^= HashBytes((const u8 *)&ss.levels[i].addr, 4, i) * (2 * (u64)i + 1);
		k.clutHash ^= (u64)ss.levels[i].bufw << (i * 4);
	}
	*levelsOut = levels;

	auto ins = entries_.try_emplace(k);
	Entry &e = ins.first->second;
	e.lastUse = frame_;
	if (!ins.second && e.srv && e.checkedRun == runSeq && !e.stale)
		return e.srv.Get();

	// Guest memory first gets what was rendered there, then the data is hashed.
	u64 hash = 0;
	u32 memStart = ~0u, memEnd = 0;
	for (int i = 0; i < levels; ++i) {
		const TexLevel &lv = ss.levels[i];
		const u32 bytes = LevelBytes(ss, lv);
		if (bytes == 0)
			continue;
		const u32 start = FoldAddress(lv.addr);
		if (syncRange)
			syncRange(user, start, bytes);
		hash = HashBytes(lv.ptr, bytes, hash + i);
		memStart = std::min(memStart, start);
		memEnd = std::max(memEnd, start + bytes);
	}
	hashes++;
	e.checkedRun = runSeq;
	e.stale = false;
	e.memStart = memStart;
	e.memEnd = memEnd;
	if (!ins.second && e.srv && hash == e.dataHash)
		return e.srv.Get();
	e.dataHash = hash;
	decodes++;

	// Decode every level.
	const DecodeFn decode = GetDecodeFn(ss.texfmt, ss.swizzle);
	size_t total = 0;
	for (int i = 0; i < levels; ++i)
		total += (size_t)std::max(1, w >> i) * std::max(1, h >> i);
	decodeBuf_.resize(total);
	D3D11_SUBRESOURCE_DATA init[8];
	size_t at = 0;
	for (int i = 0; i < levels; ++i) {
		const int lw = std::max(1, w >> i), lh = std::max(1, h >> i);
		decode(ss, i, lw, lh, ss.clampS, ss.clampT, decodeBuf_.data() + at);
		init[i].pSysMem = decodeBuf_.data() + at;
		init[i].SysMemPitch = (UINT)lw * 4;
		init[i].SysMemSlicePitch = 0;
		at += (size_t)lw * lh;
	}
	if (e.tex && e.w == w && e.h == h && e.levels == levels) {
		for (int i = 0; i < levels; ++i)
			ctx_->UpdateSubresource(e.tex.Get(), (UINT)i, nullptr, init[i].pSysMem, init[i].SysMemPitch, 0);
		return e.srv.Get();
	}
	D3D11_TEXTURE2D_DESC td{};
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = (UINT)levels;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	e.tex.Reset();
	e.srv.Reset();
	if (FAILED(dev_->CreateTexture2D(&td, init, &e.tex)) || FAILED(dev_->CreateShaderResourceView(e.tex.Get(), nullptr, &e.srv))) {
		LOGW("d3d11", "texture %dx%d (format %d) could not be created", w, h, ss.texfmt);
		e.tex.Reset();
		e.srv.Reset();
		return nullptr;
	}
	e.w = w;
	e.h = h;
	e.levels = levels;
	return e.srv.Get();
}

void TextureCache::Invalidate(u32 start, u32 end) {
	for (auto &kv : entries_) {
		Entry &e = kv.second;
		if (e.memStart < end && start < e.memEnd)
			e.stale = true;
	}
}

void TextureCache::EndFrame(u64 frame) {
	frame_ = frame;
	if (frame % 64 != 0)
		return;
	// Textures unused for a while go.
	for (auto it = entries_.begin(); it != entries_.end();) {
		if (it->second.lastUse + 600 < frame)
			it = entries_.erase(it);
		else
			++it;
	}
}

}  // namespace gpu::hw
