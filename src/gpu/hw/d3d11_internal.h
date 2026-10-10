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

enum class ShaderKind { Vertex, Pixel, Compute };
// The HLSL source for a kind of shader (d3d11_shaders.cpp).
std::string ShaderSource(ShaderKind kind);

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
	u32 texLevelMode;
	float texLevelOffset;
	float lodSlope;
	float pad;
};
static_assert(sizeof(DrawStateGpu) == 96, "DrawStateGpu layout");

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
};
static_assert(sizeof(GpuVertex) == 40, "GpuVertex layout");

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
	ID3D11ShaderResourceView *Lookup(const SamplerState &ss, u64 runSeq, void (*syncRange)(void *, u32, u32), void *user, int *levelsOut);
	// Guest memory [start, end) (folded addresses) changed: entries reading it rehash on next use.
	void Invalidate(u32 start, u32 end);
	void EndFrame(u64 frame);
	size_t Size() const { return entries_.size(); }
	u64 decodes = 0, hashes = 0;

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
		u64 dataHash = 0;
		u64 checkedRun = 0;
		bool stale = false;
		u32 memStart = 0, memEnd = 0;
		u64 lastUse = 0;
		int w = 0, h = 0, levels = 0;
	};
	ID3D11Device *dev_;
	ID3D11DeviceContext *ctx_;
	std::unordered_map<Key, Entry, KeyHash> entries_;
	std::vector<u32> decodeBuf_;
	u64 frame_ = 0;
};

}  // namespace gpu::hw
