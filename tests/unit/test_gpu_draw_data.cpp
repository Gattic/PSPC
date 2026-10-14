// SPDX-License-Identifier: GPL-2.0-or-later
// The native backends share these post-transform and texture-decoding contracts.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "gpu/hw/draw_data.h"
#include "gpu/sampler.h"
#include "testing.h"

using namespace gpu;
using namespace gpu::hw;

namespace {

struct PreparedDraw : DrawData {
	using DrawData::BuildVertices;
	using DrawData::FillStates;
	using DrawData::gpuStates_;
	using DrawData::segs_;
	using DrawData::verts_;
	void Options(int scale, int filter, int upscale) {
		scale_ = scale;
		texFilter_ = filter;
		texUpscale_ = upscale;
	}
};

RasterState State() {
	RasterState st{};
	st.throughMode = true;
	st.shadeGouraud = true;
	st.scissor = {0, 0, 479 * SUBPIX, 271 * SUBPIX};
	st.pix.fbFormat = GE_FORMAT_8888;
	st.samp.width0Shift = 5;
	st.samp.height0Shift = 4;
	st.samp.levels[0].w = 32;
	st.samp.levels[0].h = 16;
	st.samp.levels[0].bufw = 32;
	st.samp.lodSlope = 1.0f;
	return st;
}

BatchPrim Primitive(PrimType type, u32 state = 0) {
	BatchPrim p{};
	p.type = type;
	p.stateIndex = state;
	p.range = {0, 0, 479 * SUBPIX, 271 * SUBPIX};
	p.v[0] = {16, 32, 100, 0xFF102030, 0x00112233, 0, 0, 1, 1, 1};
	p.v[1] = {80, 96, 200, 0xFF405060, 0x00445566, 32, 16, 1, 1, 0.5f};
	p.v[2] = {16, 96, 300, 0xFF708090, 0x00778899, 0, 16, 1, 1, 0};
	return p;
}

}  // namespace

TEST(gpu_draw_data_triangle_color_fog_and_lod) {
	PreparedDraw draw;
	RasterState st = State();
	st.enableTextures = true;
	st.pix.applyFog = true;
	BatchPrim p = Primitive(PrimType::Triangle);
	draw.BuildVertices(&p, 1, &st);
	CHECK_EQ(draw.verts_.size(), 3u);
	CHECK_EQ(draw.segs_.size(), 1u);
	CHECK_EQ(draw.segs_[0].count, 3u);
	CHECK(!draw.segs_[0].lines);
	const float fog[] = {255, 128, 0};
	for (int i = 0; i < 3; ++i) {
		const auto &v = draw.verts_[i];
		CHECK(v.x == p.v[i].x / 16.0f);
		CHECK(v.y == p.v[i].y / 16.0f);
		CHECK_EQ(v.c0, p.v[i].color0);
		CHECK_EQ(v.c1, p.v[i].color1);
		CHECK(v.fog == fog[i]);
		CHECK(v.s == p.v[i].s / 32.0f);
		CHECK(v.t == p.v[i].t / 16.0f);
		CHECK(v.q == 1.0f);
		CHECK(v.lod == 8.0f);
	}
	st.shadeGouraud = false;
	draw.Options(4, 0, 1);
	draw.BuildVertices(&p, 1, &st);
	for (const auto &v : draw.verts_) {
		CHECK_EQ(v.c0, p.v[2].color0);
		CHECK_EQ(v.c1, p.v[2].color1);
		CHECK(v.lod == 2.0f);
	}
	st.throughMode = false;
	st.textureProj = true;
	for (auto &v : p.v) {
		v.clipw = 2;
		v.q = 3;
	}
	draw.BuildVertices(&p, 1, &st);
	for (int i = 0; i < 3; ++i) {
		CHECK(draw.verts_[i].s == p.v[i].s * 0.5f);
		CHECK(draw.verts_[i].t == p.v[i].t * 0.5f);
		CHECK(draw.verts_[i].q == 1.5f);
	}
}

TEST(gpu_draw_data_rect_clear_point_and_line) {
	PreparedDraw draw;
	RasterState st = State();
	for (PrimType type : {PrimType::Rect, PrimType::ClearRect}) {
		BatchPrim p = Primitive(type);
		// Clip the rectangle to x=2..3, y=3..4.
		p.range = {2 * SUBPIX, 3 * SUBPIX, 4 * SUBPIX - 1, 5 * SUBPIX - 1};
		draw.BuildVertices(&p, 1, &st);
		CHECK_EQ(draw.verts_.size(), 6u);
		const float xs[] = {2, 4, 2, 2, 4, 4};
		const float ys[] = {3, 3, 5, 5, 3, 5};
		for (int i = 0; i < 6; ++i) {
			CHECK(draw.verts_[i].x == xs[i]);
			CHECK(draw.verts_[i].y == ys[i]);
			CHECK_EQ(draw.verts_[i].c0, p.v[1].color0);
			CHECK_EQ(draw.verts_[i].c1, type == PrimType::ClearRect ? 0u : p.v[1].color1);
			CHECK(draw.verts_[i].z == 200);
		}
		p.range = {160, 160, 175, 175};
		draw.BuildVertices(&p, 1, &st);
		CHECK(draw.verts_.empty());
	}
	st.enableTextures = true;
	BatchPrim p = Primitive(PrimType::Point);
	// GE point UVs lose low mantissa bits, not their fractional texel coordinate.
	p.v[0].s = std::nextafter(3.75f, 4.0f);
	p.v[0].t = std::nextafter(2.25f, 3.0f);
	draw.BuildVertices(&p, 1, &st);
	CHECK_EQ(draw.verts_.size(), 6u);
	CHECK(draw.verts_[0].x == 1 && draw.verts_[0].y == 2);
	CHECK(draw.verts_[5].x == 2 && draw.verts_[5].y == 3);
	CHECK(draw.verts_[0].s == 3.75f / 32);
	CHECK(draw.verts_[0].t == 2.25f / 16);
	CHECK(draw.verts_[0].lod < 0);
	p = Primitive(PrimType::Line);
	draw.BuildVertices(&p, 1, &st);
	CHECK_EQ(draw.verts_.size(), 2u);
	CHECK(draw.segs_[0].lines);
	CHECK(draw.verts_[0].lod == -8);
	CHECK(draw.verts_[1].lod == -8);
	st.shadeGouraud = false;
	draw.BuildVertices(&p, 1, &st);
	CHECK_EQ(draw.verts_[0].c0, p.v[1].color0);
	CHECK_EQ(draw.verts_[0].c1, p.v[1].color1);
}

TEST(gpu_draw_data_batch_boundaries_and_reset) {
	PreparedDraw draw;
	std::array<RasterState, 4> states{State(), State(), State(), State()};
	states[1].pix.alphaBlend = true;  // Per-vertex state may change within one segment.
	states[2].scissor.x1 = 16;       // Scissor is per draw, so this must split.
	states[3].pix.clearMode = true;
	std::array<BatchPrim, 5> prims{Primitive(PrimType::Triangle), Primitive(PrimType::Triangle, 1),
		Primitive(PrimType::Triangle, 2), Primitive(PrimType::ClearRect, 3), Primitive(PrimType::Line, 3)};
	draw.BuildVertices(prims.data(), prims.size(), states.data());
	CHECK_EQ(draw.segs_.size(), 4u);
	CHECK_EQ(draw.segs_[0].count, 6u);
	CHECK_EQ(draw.segs_[0].lastState, 1u);
	CHECK_EQ(draw.segs_[1].first, 6u);
	CHECK_EQ(draw.segs_[2].first, 9u);
	CHECK_EQ(draw.segs_[3].first, 15u);
	CHECK(draw.segs_[3].lines);
	CHECK_EQ(draw.verts_[3].state, 1u);
	states[1] = states[0];
	states[0].enableTextures = states[1].enableTextures = true;
	states[1].samp.levels[0].addr = 0x08000000;
	draw.BuildVertices(prims.data(), 2, states.data());
	CHECK_EQ(draw.segs_.size(), 2u);
	states[1] = states[0];
	draw.BuildVertices(prims.data(), 2, states.data());
	CHECK_EQ(draw.segs_.size(), 1u);
	draw.BuildVertices(nullptr, 0, nullptr);
	CHECK(draw.verts_.empty());
	CHECK(draw.segs_.empty());
}

TEST(gpu_draw_data_state_packing_and_texture_options) {
	PreparedDraw draw;
	RasterState st = State();
	auto &p = st.pix;
	p.alphaBlend = p.applyLogicOp = p.applyFog = p.colorTest = p.stencilTest = true;
	p.applyDepthRange = p.dithering = p.applyColorWriteMask = p.hasAlphaTestMask = true;
	p.hasStencilTestMask = p.depthWrite = p.colorClear = p.stencilClear = p.depthClear = true;
	p.alphaTestFunc = 6; p.alphaTestRef = 37; p.alphaTestMask = 0x7F;
	p.stencilTestFunc = 4; p.stencilTestRef = 17; p.stencilRef = 29; p.stencilTestMask = 0xF0;
	p.sFail = 2; p.zFail = 3; p.zPass = 4;
	p.blendEq = 2; p.blendSrc = 3; p.blendDst = 4;
	p.minz = 13; p.maxz = 43210;
	p.colorWriteMask = 0xAABBCCDD;
	for (int i = 0; i < 16; ++i)
		p.ditherMatrix[i] = (s8)(i - 8);
	st.enableTextures = true;
	auto &s = st.samp;
	s.useColorDoubling = s.useTextureAlpha = true;
	s.texFunc = 3;
	s.clampS = s.minFilt = s.mipFilt = true;
	s.texLevelMode = 2;
	s.texLevelOffset = -19;
	s.maxLevel = 1;
	s.levels[1].w = 16; s.levels[1].h = 8;
	draw.Options(2, 0, 3);
	draw.FillStates(&st, 1);
	const auto d = draw.gpuStates_[0];
	CHECK_EQ(d.flags, 0x7FFFu);
	CHECK_EQ(d.alphaTest, 0x7F2506u);
	CHECK_EQ(d.stencilTest, 0xF01D1104u);
	CHECK_EQ(d.stencilOps, 0x432u);
	CHECK_EQ(d.blend, 0x432u);
	CHECK_EQ(d.minz, 13u);
	CHECK_EQ(d.maxz, 43210u);
	CHECK_EQ(d.writeMask, 0xAABBCCDDu);
	CHECK_EQ(d.dither0, 0xFEDCBA98u);
	CHECK_EQ(d.dither1, 0x76543210u);
	CHECK_EQ(d.texFunc, 0x33u);
	CHECK_EQ(d.texMode, 0x235u);
	CHECK_EQ(d.texLevelOffset, -19);
	CHECK_EQ(d.rtt, 3u << 12);
	CHECK_EQ(d.texLevels[0], 0x450000u);
	CHECK_EQ(d.texLevels[1], 0x340060u);
	CHECK_EQ(d.texLevels[7], d.texLevels[1]);
	for (int filter : {1, 2}) {
		draw.Options(1, filter, 1);
		draw.FillStates(&st, 1);
		CHECK_EQ(draw.gpuStates_[0].texMode & 12u, filter == 1 ? 0u : 12u);
		CHECK_EQ(draw.gpuStates_[0].rtt, 0u);
	}
	st = State();
	draw.FillStates(&st, 1);
	CHECK_EQ(draw.gpuStates_[0].flags, 0u);
	CHECK_EQ(draw.gpuStates_[0].dither0, 0u);
	CHECK_EQ(draw.gpuStates_[0].texMode, 0u);
	CHECK_EQ(draw.gpuStates_[0].texLevels[0], 0u);
}

TEST(gpu_draw_data_texture_atlas_limits) {
	SamplerState ss{};
	ss.maxLevel = 7;
	for (int i = 0; i < 8; ++i) {
		ss.levels[i].w = (u16)(1024 >> i);
		ss.levels[i].h = (u16)(512 >> i);
	}
	u32 levels[8];
	int w, h;
	TextureAtlasLayout(ss, levels, &w, &h);
	CHECK_EQ(w, 1528);
	CHECK_EQ(h, 512);
	CHECK_EQ(levels[0], 0x9A0000u); // Logical dimensions retained; decoded width capped at 512.
	CHECK_EQ(levels[1] & 0xFFFFu, 512u);
	CHECK_EQ(levels[2] & 0xFFFFu, 1024u);
	CHECK_EQ(TextureUpscale(4, w, h), 4);
	CHECK_EQ(TextureUpscale(4, 5000, 512), 3);
	CHECK_EQ(TextureUpscale(4, 512, 9000), 1);
}

TEST(gpu_draw_data_decode_matches_reference_sampler) {
	std::array<u8, 4096> data{};
	std::array<u8, 2048> clut{};
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = (u8)(i * 37 + (i >> 3) * 11);
	for (size_t i = 0; i < clut.size(); ++i)
		clut[i] = (u8)(i * 19 + (i >> 2));
	constexpr int width = 32, height = 16, pitch = 37;
	std::array<u32, pitch * height> decoded;
	for (int fmt = GE_TFMT_5650; fmt <= GE_TFMT_DXT5; ++fmt) {
		for (bool swizzle : {false, true}) {
			for (bool shared : {false, true}) {
				for (int clutfmt = 0; clutfmt < 4; ++clutfmt) {
					SamplerState ss{};
					ss.texfmt = (u8)fmt;
					ss.swizzle = swizzle;
					ss.clut = clut.data();
					ss.clutfmt = (u8)clutfmt;
					ss.clutMask = 0x3F; ss.clutShift = 1; ss.clutOffset = 0x20;
					ss.useSharedClut = shared;
					ss.sample = GetTexSampleFn(fmt, swizzle);
					for (int level : {0, 2}) {
						for (u32 bytes : {0u, 17u, 4096u}) {
							ss.levels[level] = {bytes ? data.data() : nullptr, 0, bytes, width, width, height, true};
							decoded.fill(0xDEADBEEF);
							DecodeTextureLevel(ss, level, width, height, decoded.data(), pitch);
							for (int y = 0; y < height; ++y) {
								for (int x = 0; x < width; ++x) {
									const u32 expected = SampleTexture(ss, (x + 0.5f) / width, (y + 0.5f) / height, level, 0, false);
									CHECK_EQ(decoded[y * pitch + x], expected);
								}
								for (int x = width; x < pitch; ++x)
									CHECK_EQ(decoded[y * pitch + x], 0xDEADBEEFu);
							}
						}
					}
				}
			}
		}
	}
}
