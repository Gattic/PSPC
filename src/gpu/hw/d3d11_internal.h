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
#include "gpu/render.h"

namespace gpu::hw {

using Microsoft::WRL::ComPtr;

// Compute: surface uploads, write-backs and display images; Upscale: texture upscaling; Present: drawing
// an image into the host window.
enum class ShaderKind { Vertex, Pixel, Compute, Upscale, Present };
// The HLSL source for a kind of shader (d3d11_shaders.cpp).
std::string ShaderSource(ShaderKind kind);

// The largest texture side D3D11 allows.
constexpr int MAX_TEXTURE_SIDE = 16384;

// The factor textures of an atlas this size are scaled up by, at texture upscaling `setting` (1 = off):
// within the largest texture D3D11 allows.
inline int TextureUpscale(int setting, int atlasW, int atlasH) {
	int f = setting;
	while (f > 1 && (atlasW * f > MAX_TEXTURE_SIDE || atlasH * f > MAX_TEXTURE_SIDE))
		--f;
	return f > 1 ? f : 1;
}

// Per-draw state for the pixel shader; matches DrawState in the HLSL.
struct DrawStateGpu {
	u32 flags;
	u32 alphaTest;
	u32 colorTestFunc;
	u32 colorTestRef;
	u32 colorTestMask;
	u32 stencilTest;
	u32 stencilOps;
	u32 depthFunc;
	u32 minz;
	u32 maxz;
	u32 blend;
	u32 fixA;
	u32 fixB;
	u32 fogColor;
	u32 writeMask;
	u32 logicOp;
	u32 texFunc;
	u32 texEnv;
	u32 dither0;
	u32 dither1;
	u32 texMode;
	s32 texLevelOffset;
	s32 lodSlopeLog;
	u32 rtt;  // format | 0x100 (render to texture) | texture scale << 12
	u32 texLevels[8];
	u32 rttY;
	u32 pad2[3];
};
static_assert(sizeof(DrawStateGpu) == 144, "DrawStateGpu layout");

enum : u32 {
	F_ALPHA_BLEND = 0x0001,
	F_LOGIC_OP = 0x0002,
	F_FOG = 0x0004,
	F_COLOR_TEST = 0x0008,
	F_STENCIL_TEST = 0x0010,
	F_DEPTH_RANGE = 0x0020,
	F_DITHER = 0x0040,
	F_WRITE_MASK = 0x0080,
	F_ALPHA_MASK = 0x0100,
	F_STENCIL_MASK = 0x0200,
	F_DEPTH_WRITE = 0x0400,
	F_COLOR_CLEAR = 0x0800,
	F_STENCIL_CLEAR = 0x1000,
	F_DEPTH_CLEAR = 0x2000,
	F_DBL_SEC = 0x4000,
};

// A vertex as the GPU takes it: screen space, already divided by w; s, t and q interpolate linearly
// in screen space and the texel coordinate is (s/q, t/q), as on the GE.
struct GpuVertex {
	float x, y;      // PSP pixels
	float z, fog;    // 0-65535, 0-255
	u32 c0, c1;      // RGBA
	float s, t, q;
	u32 state;
	float lod;       // the primitive's mip level gradient (same at every vertex), see the pixel shader
};
static_assert(sizeof(GpuVertex) == 44, "GpuVertex layout");

// The texture atlas layout of a sampler state's levels (each level at most 512 x 512, side by side):
// DrawStateGpu::texLevels. Returns the atlas size.
void TextureAtlasLayout(const SamplerState &ss, u32 levels[8], int *atlasW, int *atlasH);

// A 64-bit hash of guest memory (texture validation).
u64 HashBytes(const u8 *p, size_t n, u64 seed);

class Device;

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
