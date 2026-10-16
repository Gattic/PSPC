// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internals of the D3D11 backend (d3d11_backend.cpp, d3d11_shaders.cpp, d3d11_texcache.cpp).
#pragma once

#include <d3d11_3.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common.h"
#include "gpu/hw/draw_data.h"

namespace gpu::hw {

using Microsoft::WRL::ComPtr;

// Compute: surface uploads, write-backs and display images; Upscale: texture upscaling; Present: drawing
// an image into the host window; Geometry: vertex transform and triangle assembly (gpu/geometry.h).
enum class ShaderKind { Vertex, Pixel, Compute, Upscale, Present, Geometry };
// The HLSL source for a kind of shader (d3d11_shaders.cpp).
std::string ShaderSource(ShaderKind kind);
// ShaderKind::Geometry's (d3d11_geometry.cpp).
std::string GeometryShaderSource();

// Decoded textures, keyed by address, format, size and CLUT, validated by hashing the texel data
// (d3d11_texcache.cpp).
class TextureCache {
public:
	explicit TextureCache(ID3D11Device *dev, ID3D11DeviceContext *ctx) : dev_(dev), ctx_(ctx) {}
	// The texture for a sampler state; nullptr if it can't be made. syncRange is called with the guest
	// memory the texture reads before it's hashed or decoded (so rendering there reaches memory first).
	ID3D11ShaderResourceView *Lookup(const SamplerState &ss, u64 runSeq, void (*syncRange)(void *, u32, u32), void *user);
	// Guest memory [start, end) (folded addresses) changed: entries reading it rehash on next use.
	void Invalidate(u32 start, u32 end);
	void EndFrame(u64 frame);
	size_t Size() const { return entries_.size(); }
	// Texture upscaling: decoded textures are scaled up by TextureUpscale(factor, ...) with `cs`
	// (CSUpscale, xBR) before they're sampled. The pixel shader samples them at that scale.
	bool SetUpscale(int factor, ID3D11ComputeShader *cs);
	u64 decodes = 0, hashes = 0;
	double hashSeconds = 0, decodeSeconds = 0, pureHashSeconds = 0;
	u64 hashBytes = 0, decodeTexels = 0;

private:
	struct Key {
		u32 addr;
		u8 fmt, levels, flags;
		u16 bufw, w, h;
		u64 clutHash;
		bool operator==(const Key &o) const {
			return addr == o.addr && fmt == o.fmt && levels == o.levels && flags == o.flags && bufw == o.bufw && w == o.w &&
				h == o.h && clutHash == o.clutHash;
		}
	};
	struct KeyHash {
		size_t operator()(const Key &k) const;
	};
	struct Entry {
		ComPtr<ID3D11Texture2D> tex;
		ComPtr<ID3D11ShaderResourceView> srv;
		ComPtr<ID3D11UnorderedAccessView> uav;  // upscaled textures: the upscaler's target
		u64 dataHash = 0;
		u64 checkedRun = 0;
		bool stale = false;
		u32 memStart = 0, memEnd = 0;
		u64 lastUse = 0;
		int w = 0, h = 0;
	};
	ID3D11Device *dev_;
	ID3D11DeviceContext *ctx_;
	bool Upscale(Entry &e, const SamplerState &ss, const u32 layout[8], int w, int h, int factor);

	std::unordered_map<Key, Entry, KeyHash> entries_;
	std::vector<u32> decodeBuf_;
	u64 frame_ = 0;
	int upscale_ = 1;
	ComPtr<ID3D11ComputeShader> upscaleCs_;
	ComPtr<ID3D11Buffer> upscaleCb_;
	// The 1x atlas the upscaler reads.
	ComPtr<ID3D11Texture2D> upSrc_;
	ComPtr<ID3D11ShaderResourceView> upSrcSrv_;
	int upSrcW_ = 0, upSrcH_ = 0;
};

}  // namespace gpu::hw
