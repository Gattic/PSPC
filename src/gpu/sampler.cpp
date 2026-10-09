// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// The texture samplers, one per format and swizzling.
#include "gpu/sampler.h"

namespace gpu {

namespace {

template <int fmt>
TexSampleFn Pick(bool swizzle) {
	return swizzle ? &SampleTextureT<fmt, true> : &SampleTextureT<fmt, false>;
}

}  // namespace

TexSampleFn GetTexSampleFn(int texfmt, bool swizzle) {
	switch (texfmt) {
	case GE_TFMT_5650: return Pick<GE_TFMT_5650>(swizzle);
	case GE_TFMT_5551: return Pick<GE_TFMT_5551>(swizzle);
	case GE_TFMT_4444: return Pick<GE_TFMT_4444>(swizzle);
	case GE_TFMT_8888: return Pick<GE_TFMT_8888>(swizzle);
	case GE_TFMT_CLUT4: return Pick<GE_TFMT_CLUT4>(swizzle);
	case GE_TFMT_CLUT8: return Pick<GE_TFMT_CLUT8>(swizzle);
	case GE_TFMT_CLUT16: return Pick<GE_TFMT_CLUT16>(swizzle);
	case GE_TFMT_CLUT32: return Pick<GE_TFMT_CLUT32>(swizzle);
	// DXT isn't swizzled.
	case GE_TFMT_DXT1: return &SampleTextureT<GE_TFMT_DXT1, false>;
	case GE_TFMT_DXT3: return &SampleTextureT<GE_TFMT_DXT3, false>;
	// Invalid formats have no texel pointer and read 0.
	default: return &SampleTextureT<GE_TFMT_DXT5, false>;
	}
}

}  // namespace gpu
