// SPDX-License-Identifier: GPL-2.0-or-later
//
// The D3D11 backend's texture cache. Textures are decoded to RGBA8888 with the software sampler's own
// texel fetch (sampler.h FetchTexelT: every format, CLUT4/8/16/32 with shift/mask/offset and per-level
// palettes, DXT1/3/5, swizzling, reads past the end of memory), so they match the reference renderer
// texel for texel. An entry is keyed by address, format, size and CLUT contents, and validated by
// hashing its texel data once per GE run (the CPU may have rewritten it in between), or when the GE
// itself wrote the memory (block transfer, rendering written back). With texture upscaling, the decoded
// atlas is then scaled up on the GPU (CSUpscale, xBR) into the texture that's sampled.
#include <algorithm>
#include <chrono>
#include <cstring>

#include "common/log.h"
#include "gpu/hw/d3d11_internal.h"
#include "gpu/sampler.h"

namespace gpu::hw {

namespace {

// The bytes a level's texel lookups can reach (binner.cpp AddTextureReads).
u32 LevelBytes(const SamplerState &ss, const TexLevel &lv) {
	if (!lv.ptr)
		return 0;
	const u32 h = std::min<u32>(lv.h, 512);
	const u32 rows = ss.swizzle ? ((h + 7) & ~7u) : h;
	return std::min<u32>((u32)((u64)rows * lv.bufw * kTexelBits[ss.texfmt] / 8), lv.validBytes);
}

}  // namespace

size_t TextureCache::KeyHash::operator()(const Key &k) const {
	u64 h = (u64)k.addr * P1;
	h ^= ((u64)k.fmt | ((u64)k.levels << 8) | ((u64)k.flags << 16) | ((u64)k.bufw << 24) | ((u64)k.w << 40)) * P2;
	h ^= ((u64)k.h << 7) ^ k.clutHash;
	return (size_t)(h ^ (h >> 29));
}

ID3D11ShaderResourceView *TextureCache::Lookup(const SamplerState &ss, u64 runSeq, void (*syncRange)(void *, u32, u32), void *user) {
	const int levels = ss.maxLevel + 1;
	const TexLevel &l0 = ss.levels[0];
	Key k{};
	k.addr = l0.addr;
	k.fmt = ss.texfmt;
	k.levels = (u8)levels;
	k.flags = (u8)((ss.swizzle ? 1 : 0) | (ss.useSharedClut ? 8 : 0));
	k.bufw = l0.bufw;
	k.w = l0.w;
	k.h = l0.h;
	u32 layout[8];
	int w, h;
	TextureAtlasLayout(ss, layout, &w, &h);
	const int up = upscaleCs_ ? TextureUpscale(upscale_, w, h) : 1;
	// The upscaler wraps or clamps at a texture's edges as it's sampled.
	if (up > 1)
		k.flags |= (u8)((ss.clampS ? 0x10 : 0) | (ss.clampT ? 0x20 : 0));
	const bool indexed = ss.texfmt >= GE_TFMT_CLUT4 && ss.texfmt <= GE_TFMT_CLUT32;
	if (indexed) {
		const u64 params = (u64)ss.clutfmt | ((u64)ss.clutShift << 8) | ((u64)ss.clutMask << 16) | ((u64)ss.clutOffset << 24);
		k.clutHash = HashBytes(ss.clut, 1024, params) | 1;
	}
	for (int i = 1; i < levels; ++i) {
		// The other levels' addresses and sizes are part of what's decoded.
		const TexLevel &lv = ss.levels[i];
		const u64 v = (u64)lv.addr | ((u64)lv.bufw << 32) | ((u64)Log2Pow2(lv.w) << 48) | ((u64)Log2Pow2(lv.h) << 56);
		k.clutHash ^= HashBytes((const u8 *)&v, 8, (u64)i) * (2 * (u64)i + 1);
	}

	auto ins = entries_.try_emplace(k);
	Entry &e = ins.first->second;
	e.lastUse = frame_;
	if (!ins.second && e.srv && e.checkedRun == runSeq && !e.stale)
		return e.srv.Get();

	// Guest memory first gets what was rendered there, then the data is hashed.
	auto t0 = std::chrono::steady_clock::now();
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
		const auto th = std::chrono::steady_clock::now();
		hash = HashBytes(lv.ptr, bytes, hash + (u64)i);
		pureHashSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - th).count();
		hashBytes += bytes;
		memStart = std::min(memStart, start);
		memEnd = std::max(memEnd, start + bytes);
	}
	hashes++;
	auto t1 = std::chrono::steady_clock::now();
	hashSeconds += std::chrono::duration<double>(t1 - t0).count();
	e.checkedRun = runSeq;
	e.stale = false;
	e.memStart = memStart;
	e.memEnd = memEnd;
	if (!ins.second && e.srv && hash == e.dataHash)
		return e.srv.Get();
	e.dataHash = hash;
	decodes++;

	// Decode every level into the atlas.
	decodeBuf_.assign((size_t)w * h, 0);
	for (int i = 0; i < levels; ++i) {
		const TexLevel &lv = ss.levels[i];
		DecodeTextureLevel(ss, i, std::min<int>(lv.w, 512), std::min<int>(lv.h, 512), decodeBuf_.data() + (layout[i] & 0xFFFF), (size_t)w);
	}
	decodeTexels += (u64)w * h;
	decodeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
	if (up > 1)
		return Upscale(e, ss, layout, w, h, up) ? e.srv.Get() : nullptr;
	if (e.tex && e.w == w && e.h == h) {
		ctx_->UpdateSubresource(e.tex.Get(), 0, nullptr, decodeBuf_.data(), (UINT)w * 4, 0);
		return e.srv.Get();
	}
	D3D11_TEXTURE2D_DESC td{};
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init{ decodeBuf_.data(), (UINT)w * 4, 0 };
	e.tex.Reset();
	e.srv.Reset();
	if (FAILED(dev_->CreateTexture2D(&td, &init, &e.tex)) || FAILED(dev_->CreateShaderResourceView(e.tex.Get(), nullptr, &e.srv))) {
		LOGW("d3d11", "texture %dx%d (format %d) could not be created", w, h, ss.texfmt);
		e.tex.Reset();
		e.srv.Reset();
		return nullptr;
	}
	e.w = w;
	e.h = h;
	return e.srv.Get();
}

bool TextureCache::SetUpscale(int factor, ID3D11ComputeShader *cs) {
	upscale_ = std::clamp(factor, 1, 4);
	upscaleCs_ = cs;
	entries_.clear();
	if (!upscaleCb_) {
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 32;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(dev_->CreateBuffer(&bd, nullptr, &upscaleCb_)))
			upscaleCs_.Reset();
	}
	return upscaleCs_ != nullptr;
}

// The decoded atlas (decodeBuf_, w x h) scaled up into the entry's texture, level by level.
bool TextureCache::Upscale(Entry &e, const SamplerState &ss, const u32 layout[8], int w, int h, int factor) {
	if (!upSrc_ || upSrcW_ < w || upSrcH_ < h) {
		D3D11_TEXTURE2D_DESC td{};
		td.Width = (UINT)std::max(w, upSrcW_);
		td.Height = (UINT)std::max(h, upSrcH_);
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		upSrc_.Reset();
		upSrcSrv_.Reset();
		if (FAILED(dev_->CreateTexture2D(&td, nullptr, &upSrc_)) || FAILED(dev_->CreateShaderResourceView(upSrc_.Get(), nullptr, &upSrcSrv_))) {
			upSrc_.Reset();
			upSrcW_ = upSrcH_ = 0;
			return false;
		}
		upSrcW_ = (int)td.Width;
		upSrcH_ = (int)td.Height;
	}
	const D3D11_BOX box{ 0, 0, 0, (UINT)w, (UINT)h, 1 };
	ctx_->UpdateSubresource(upSrc_.Get(), 0, &box, decodeBuf_.data(), (UINT)w * 4, 0);
	const int sw = w * factor, sh = h * factor;
	if (!e.tex || !e.uav || e.w != sw || e.h != sh) {
		D3D11_TEXTURE2D_DESC td{};
		td.Width = (UINT)sw;
		td.Height = (UINT)sh;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		e.tex.Reset();
		e.srv.Reset();
		e.uav.Reset();
		if (FAILED(dev_->CreateTexture2D(&td, nullptr, &e.tex)) || FAILED(dev_->CreateShaderResourceView(e.tex.Get(), nullptr, &e.srv)) ||
			FAILED(dev_->CreateUnorderedAccessView(e.tex.Get(), nullptr, &e.uav))) {
			LOGW("d3d11", "upscaled texture %dx%d could not be created", sw, sh);
			e.tex.Reset();
			e.srv.Reset();
			e.uav.Reset();
			return false;
		}
		e.w = sw;
		e.h = sh;
	}
	// (The texture may still be bound for drawing.)
	ID3D11ShaderResourceView *nullSrv = nullptr;
	ctx_->PSSetShaderResources(0, 1, &nullSrv);
	ctx_->CSSetShader(upscaleCs_.Get(), nullptr, 0);
	ctx_->CSSetConstantBuffers(0, 1, upscaleCb_.GetAddressOf());
	ctx_->CSSetShaderResources(0, 1, upSrcSrv_.GetAddressOf());
	ctx_->CSSetUnorderedAccessViews(0, 1, e.uav.GetAddressOf(), nullptr);
	for (int i = 0; i <= ss.maxLevel; ++i) {
		const int lw = std::min<int>(ss.levels[i].w, 512), lh = std::min<int>(ss.levels[i].h, 512);
		if (lw <= 0 || lh <= 0)
			continue;
		const s32 cb[8] = { (s32)(layout[i] & 0xFFFF), 0, lw, lh, factor, (ss.clampS ? 0 : 1) | (ss.clampT ? 0 : 2), 0, 0 };
		ctx_->UpdateSubresource(upscaleCb_.Get(), 0, nullptr, cb, 0, 0);
		ctx_->Dispatch((UINT)(lw * factor + 7) / 8, (UINT)(lh * factor + 7) / 8, 1);
	}
	ID3D11UnorderedAccessView *nullUav = nullptr;
	ctx_->CSSetShaderResources(0, 1, &nullSrv);
	ctx_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	return true;
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
