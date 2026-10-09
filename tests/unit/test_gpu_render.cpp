// SPDX-License-Identifier: GPL-2.0-or-later
// GE rendering: synthetic display lists drawn into emulated VRAM, checked pixel by pixel.
#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "allegrex/guestmem.h"
#include "gpu/ge.h"
#include "gpu/ge_constants.h"
#include "testing.h"

using namespace gpu;

namespace {

constexpr u32 VRAM = 0x04000000;
constexpr u32 RAM = 0x08000000;
constexpr u32 RAM_SIZE = 0x00800000;
constexpr u32 ZBUF = 0x00088000;  // VRAM offset of the depth buffer
constexpr u32 LIST = RAM + 0x100000;
constexpr u32 VERTS = RAM + 0x200000;
constexpr u32 TEX = RAM + 0x300000;
constexpr u32 CLUTA = RAM + 0x380000;

// A 160 MB guest address space with VRAM and the first MBs of RAM committed.
struct GeEnv {
	u8 *mem = nullptr;
	u8 *saved = nullptr;
	explicit GeEnv(int threads = 1) {
		mem = (u8 *)VirtualAlloc(nullptr, 0x0A000000, MEM_RESERVE, PAGE_NOACCESS);
		VirtualAlloc(mem + VRAM, 0x200000, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(mem + RAM, RAM_SIZE, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(mem + 0x10000, 0x4000, MEM_COMMIT, PAGE_READWRITE);
		saved = g_membase;
		g_membase = mem;
		SetRamSize(RAM_SIZE);
		SetSdkVersion(0x06060010);
		Init();
		SetRenderThreads(threads);
	}
	~GeEnv() {
		Shutdown();
		g_membase = saved;
		VirtualFree(mem, 0, MEM_RELEASE);
	}
};

struct DL {
	u32 start, pc;
	explicit DL(u32 addr = LIST) : start(addr), pc(addr) {}
	DL &C(int cmd, u32 data = 0) {
		GWrite32(pc, ((u32)cmd << 24) | (data & 0xFFFFFF));
		pc += 4;
		return *this;
	}
	DL &F(int cmd, float f) {
		u32 b;
		memcpy(&b, &f, 4);
		return C(cmd, b >> 8);
	}
	DL &Addr(int cmd, u32 addr) { return C(GE_CMD_BASE, (addr >> 8) & 0x0F0000).C(cmd, addr & 0xFFFFFF); }
	DL &Finish() { return C(GE_CMD_FINISH).C(GE_CMD_END); }
	// 8888 (or other) framebuffer at VRAM 0, stride 512, depth at ZBUF, full 480x272 scissor and region.
	DL &Target(int fmt = GE_FORMAT_8888) {
		C(GE_CMD_FRAMEBUFPTR, 0).C(GE_CMD_FRAMEBUFWIDTH, 512).C(GE_CMD_FRAMEBUFPIXFORMAT, fmt);
		C(GE_CMD_ZBUFPTR, ZBUF).C(GE_CMD_ZBUFWIDTH, 512);
		C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 479 | (271 << 10));
		C(GE_CMD_REGION1, 0).C(GE_CMD_REGION2, 479 | (271 << 10));
		return *this;
	}
	DL &Prim(int prim, int count, u32 verts) { return Addr(GE_CMD_VADDR, verts).C(GE_CMD_PRIM, ((u32)prim << 16) | count); }
};

int RunList(const DL &dl) {
	const int id = EnqueueList(dl.start, 0, -1, 0, false);
	Run();
	return id;
}

u32 Pixel32(int x, int y) {
	return GRead32(VRAM + (y * 512 + x) * 4);
}

u16 Pixel16(int x, int y) {
	return GRead16(VRAM + (y * 512 + x) * 2);
}

u16 Depth(int x, int y) {
	return GRead16(VRAM + ZBUF + (y * 512 + x) * 2);
}

// Through mode vertex: 8888 color, 16-bit position (12 bytes).
struct TVert {
	u32 color;
	s16 x, y, z;
	u16 pad;
};
static_assert(sizeof(TVert) == 12, "layout");
constexpr u32 VT_C8888_P16 = GE_VTYPE_THROUGH | GE_VTYPE_COL_8888 | GE_VTYPE_POS_16BIT;

// Through mode textured vertex: 16-bit UV, 8888 color, 16-bit position (16 bytes).
struct TTVert {
	u16 u, v;
	u32 color;
	s16 x, y, z;
	u16 pad;
};
static_assert(sizeof(TTVert) == 16, "layout");
constexpr u32 VT_T16_C8888_P16 = GE_VTYPE_THROUGH | GE_VTYPE_TC_16BIT | GE_VTYPE_COL_8888 | GE_VTYPE_POS_16BIT;

// Transform mode vertex: 8888 color, float position (16 bytes).
struct FVert {
	u32 color;
	float x, y, z;
};
constexpr u32 VT_C8888_PF = GE_VTYPE_COL_8888 | GE_VTYPE_POS_FLOAT;

template <typename T>
void PutVerts(u32 addr, const T *v, int n) {
	memcpy(GPtr(addr), v, sizeof(T) * n);
}

void ClearScreen(DL &dl, u32 color, u16 z = 0) {
	TVert v[2] = { { color, 0, 0, (s16)z, 0 }, { color, 480, 272, (s16)z, 0 } };
	const u32 at = VERTS + 0x7F000;
	PutVerts(at, v, 2);
	dl.C(GE_CMD_CLEARMODE, 1 | (7 << 8)).C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, at).C(GE_CMD_CLEARMODE, 0);
}

}  // namespace

TEST(gpu_clear_rect) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF336699, 0x1234);
	dl.Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(0, 0), 0xFF336699u);
	CHECK_EQ(Pixel32(479, 271), 0xFF336699u);
	CHECK_EQ(Pixel32(480, 0), 0u);
	CHECK_EQ(Pixel32(0, 272), 0u);
	CHECK_EQ(Depth(100, 100), 0x1234u);
	CHECK_EQ(Depth(480, 100), 0u);
}

TEST(gpu_sprite_fill_edges) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	// A sprite from (10, 20) to (30, 25): columns 10-29, rows 20-24.
	TVert v[2] = { { 0xFF00FF00, 10, 20, 0, 0 }, { 0xFF00FF00, 30, 25, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(10, 20), 0xFF00FF00u);
	CHECK_EQ(Pixel32(29, 24), 0xFF00FF00u);
	CHECK_EQ(Pixel32(9, 20), 0xFF000000u);
	CHECK_EQ(Pixel32(30, 20), 0xFF000000u);
	CHECK_EQ(Pixel32(10, 19), 0xFF000000u);
	CHECK_EQ(Pixel32(10, 25), 0xFF000000u);
}

TEST(gpu_scissor) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	dl.C(GE_CMD_SCISSOR1, 100 | (50 << 10)).C(GE_CMD_SCISSOR2, 199 | (99 << 10));
	TVert v[2] = { { 0xFFFFFFFF, 0, 0, 0, 0 }, { 0xFFFFFFFF, 480, 272, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(100, 50), 0xFFFFFFFFu);
	CHECK_EQ(Pixel32(199, 99), 0xFFFFFFFFu);
	CHECK_EQ(Pixel32(99, 50), 0xFF000000u);
	CHECK_EQ(Pixel32(200, 99), 0xFF000000u);
	CHECK_EQ(Pixel32(150, 100), 0xFF000000u);
}

TEST(gpu_triangle_through_coverage) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	// A right triangle with the corner at (0, 0) and legs of 16 pixels.
	TVert v[3] = { { 0xFFFF0000, 0, 0, 0, 0 }, { 0xFFFF0000, 16, 0, 0, 0 }, { 0xFFFF0000, 0, 16, 0, 0 } };
	PutVerts(VERTS, v, 3);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_TRIANGLES, 3, VERTS).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(0, 0), 0xFFFF0000u);
	CHECK_EQ(Pixel32(14, 0), 0xFFFF0000u);
	CHECK_EQ(Pixel32(0, 14), 0xFFFF0000u);
	CHECK_EQ(Pixel32(7, 7), 0xFFFF0000u);
	// The hypotenuse: pixel centers on it are on a right edge, excluded.
	CHECK_EQ(Pixel32(8, 8), 0xFF000000u);
	CHECK_EQ(Pixel32(15, 0), 0xFF000000u);
	int count = 0;
	for (int y = 0; y < 20; ++y)
		for (int x = 0; x < 20; ++x)
			count += Pixel32(x, y) == 0xFFFF0000u;
	// Pixel centers strictly inside x + y < 16: 15 + 14 + ... + 1.
	CHECK_EQ(count, 120);
}

TEST(gpu_gouraud_triangle) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	TVert v[3] = { { 0xFF0000FF, 0, 0, 0, 0 }, { 0xFF00FF00, 256, 0, 0, 0 }, { 0xFFFF0000, 0, 256, 0, 0 } };
	PutVerts(VERTS, v, 3);
	dl.C(GE_CMD_SHADEMODE, 1).C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_TRIANGLES, 3, VERTS).Finish();
	RunList(dl);
	const u32 c = Pixel32(64, 64);
	const int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
	// At (64.5, 64.5)/256 the weights are about 0.5, 0.25, 0.25.
	CHECK(std::abs(r - 127) <= 2);
	CHECK(std::abs(g - 64) <= 2);
	CHECK(std::abs(b - 64) <= 2);
	// Flat shading takes the last vertex's color.
	DL dl2(LIST + 0x1000);
	dl2.C(GE_CMD_SHADEMODE, 0).C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_TRIANGLES, 3, VERTS).Finish();
	RunList(dl2);
	CHECK_EQ(Pixel32(64, 64), 0xFFFF0000u);
}

TEST(gpu_textured_sprite_clut8) {
	GeEnv env;
	// A 16x16 CLUT8 texture: texel (x, y) = index x + 16 * (y & 1); palette entry i = 0xFF000000 | i * 0x010101.
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			GWrite8(TEX + y * 16 + x, (u8)(x + 16 * (y & 1)));
	for (int i = 0; i < 256; ++i)
		GWrite32(CLUTA + i * 4, 0xFF000000u | (u32)i * 0x010101u);
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1);
	dl.C(GE_CMD_TEXFORMAT, GE_TFMT_CLUT8).C(GE_CMD_TEXMODE, 0);
	dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 16 | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 4 | (4 << 8));
	dl.C(GE_CMD_CLUTADDR, CLUTA & 0xFFFFF0).C(GE_CMD_CLUTADDRUPPER, (CLUTA >> 8) & 0x0F0000);
	dl.C(GE_CMD_CLUTFORMAT, GE_CMODE_32BIT_ABGR8888 | (0xFF << 8)).C(GE_CMD_LOADCLUT, 1024 / 32);
	dl.C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100).C(GE_CMD_TEXFILTER, 0).C(GE_CMD_TEXWRAP, 0);
	TTVert v[2] = { { 0, 0, 0xFFFFFFFF, 100, 100, 0, 0 }, { 16, 16, 0xFFFFFFFF, 116, 116, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(100, 100), 0xFF000000u);
	CHECK_EQ(Pixel32(105, 100), 0xFF050505u);
	CHECK_EQ(Pixel32(115, 101), (0xFF000000u | 31u * 0x010101u));
	CHECK_EQ(Pixel32(103, 110), 0xFF030303u);
	CHECK_EQ(Pixel32(116, 100), 0xFF000000u);

	// CLUT index shift/mask/offset: shift 1, mask 0x7, offset 16 entries: index = ((i >> 1) & 7) | 16.
	DL dl2(LIST + 0x2000);
	dl2.C(GE_CMD_CLUTFORMAT, GE_CMODE_32BIT_ABGR8888 | (1 << 2) | (0x07 << 8) | (1 << 16)).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl2);
	CHECK_EQ(Pixel32(105, 100), (0xFF000000u | 18u * 0x010101u));
	CHECK_EQ(Pixel32(115, 100), (0xFF000000u | 23u * 0x010101u));
}

TEST(gpu_texture_formats_swizzle) {
	GeEnv env;
	// 32x8 8888 texture, swizzled: blocks of 16 bytes x 8 rows. Texel (x, y) = 0xFF000000 | y << 8 | x.
	const int w = 32, h = 8;
	std::vector<u32> linear(w * h);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			linear[y * w + x] = 0xFF000000u | (u32)(y << 8) | (u32)x;
	// Swizzle: rowBytes = 128; block (bx, by) of 16x8 bytes stored contiguously.
	const int rowBytes = w * 4;
	u8 *dst = GPtr(TEX);
	const u8 *src = (const u8 *)linear.data();
	int out = 0;
	for (int by = 0; by < h / 8; ++by)
		for (int bx = 0; bx < rowBytes / 16; ++bx)
			for (int yy = 0; yy < 8; ++yy) {
				memcpy(dst + out, src + (by * 8 + yy) * rowBytes + bx * 16, 16);
				out += 16;
			}
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_8888).C(GE_CMD_TEXMODE, 1);
	dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, w | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 5 | (3 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100);
	TTVert v[2] = { { 0, 0, 0xFFFFFFFF, 0, 0, 0, 0 }, { 32, 8, 0xFFFFFFFF, 32, 8, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl);
	bool ok = true;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			ok = ok && Pixel32(x, y) == linear[y * w + x];
	CHECK(ok);

	// 4444 and 5551 textures (unswizzled) through MODULATE with white: the texel expanded.
	GWrite16(TEX + 0x10000, 0xF84C);  // 4444: r=C g=4 b=8 a=F
	DL dl2(LIST + 0x2000);
	dl2.C(GE_CMD_TEXFORMAT, GE_TFMT_4444).C(GE_CMD_TEXMODE, 0).C(GE_CMD_TEXSIZE0, 0);
	dl2.C(GE_CMD_TEXADDR0, (TEX + 0x10000) & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 8 | (((TEX + 0x10000) >> 8) & 0x0F0000));
	dl2.C(GE_CMD_TEXFUNC, GE_TEXFUNC_MODULATE | 0x100);
	TTVert v2[2] = { { 0, 0, 0xFFFFFFFF, 40, 0, 0, 0 }, { 1, 1, 0xFFFFFFFF, 41, 1, 0, 0 } };
	PutVerts(VERTS + 0x100, v2, 2);
	dl2.Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x100).Finish();
	RunList(dl2);
	// Modulate: (255 + 1) * t / 256 = t; the written alpha stays the old (stencil) one.
	CHECK_EQ(Pixel32(40, 0), 0xFF8844CCu);
}

TEST(gpu_texture_dxt1) {
	GeEnv env;
	// One 4x4 DXT1 block: color1 = white (0xFFFF), color2 = black (0), indices: row 0 all 0, row 1 all 1,
	// row 2 all 2 (2/3 white), row 3 all 3 (1/3 white).
	u8 *b = GPtr(TEX);
	b[0] = 0x00;
	b[1] = 0x55;
	b[2] = 0xAA;
	b[3] = 0xFF;
	b[4] = 0xFF;
	b[5] = 0xFF;
	b[6] = 0x00;
	b[7] = 0x00;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_DXT1).C(GE_CMD_TEXMODE, 0);
	dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 4 | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 2 | (2 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100);
	TTVert v[2] = { { 0, 0, 0xFFFFFFFF, 0, 0, 0, 0 }, { 4, 4, 0xFFFFFFFF, 4, 4, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(0, 0) & 0xFFFFFF, 0xF8FCF8u);
	CHECK_EQ(Pixel32(0, 1) & 0xFFFFFF, 0x000000u);
	CHECK_EQ(Pixel32(0, 2) & 0xFFFFFF, 0xA5A8A5u);
	CHECK_EQ(Pixel32(0, 3) & 0xFFFFFF, 0x525452u);
}

TEST(gpu_alpha_blend) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF0000FF);  // red
	// Green at alpha 0x80, SRCALPHA / INVSRCALPHA.
	TVert v[2] = { { 0x8000FF00, 0, 0, 0, 0 }, { 0x8000FF00, 64, 64, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_ALPHABLENDENABLE, 1).C(GE_CMD_BLENDMODE, GE_BLEND_SRCALPHA | (GE_BLEND_INVSRCALPHA << 4) | (GE_BLENDMODE_MUL_AND_ADD << 8));
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS);
	// Fixed colors and subtract on another area.
	TVert v2[2] = { { 0xFF808080, 100, 0, 0, 0 }, { 0xFF808080, 164, 64, 0, 0 } };
	PutVerts(VERTS + 0x40, v2, 2);
	dl.C(GE_CMD_BLENDMODE, GE_BLEND_FIX | (GE_BLEND_FIX << 4) | (GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE << 8));
	dl.C(GE_CMD_BLENDFIXEDA, 0xFFFFFF).C(GE_CMD_BLENDFIXEDB, 0xFFFFFF).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x40);
	// Absolute difference.
	dl.C(GE_CMD_BLENDMODE, GE_BLENDMODE_ABSDIFF << 8);
	TVert v3[2] = { { 0xFF102040, 200, 0, 0, 0 }, { 0xFF102040, 264, 64, 0, 0 } };
	PutVerts(VERTS + 0x80, v3, 2);
	dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x80).Finish();
	RunList(dl);
	auto term = [](int c, int f) { return ((2 * c + 1) * (2 * f + 1)) >> 10; };
	const int r = term(0, 0x80) + term(255, 255 - 0x80);
	const int g = term(255, 0x80) + term(0, 255 - 0x80);
	// The written alpha is the old one (stencil), not the fragment's.
	CHECK_EQ(Pixel32(10, 10), 0xFF000000u | (u32)r | ((u32)g << 8));
	// dst - src with both factors 1: red 255 - 128, green 0 - 128 -> 0.
	CHECK_EQ(Pixel32(110, 10), 0xFF000000u | (u32)(term(255, 255) - term(128, 255)));
	CHECK_EQ(Pixel32(210, 10), 0xFF000000u | (255 - 0x40) | (0x20 << 8) | (0x10 << 16));
}

TEST(gpu_depth_test) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000, 0xFFFF);
	dl.C(GE_CMD_ZTESTENABLE, 1).C(GE_CMD_ZTEST, GE_COMP_LESS).C(GE_CMD_ZWRITEDISABLE, 0);
	// Near (z 100) drawn first, then far (z 200) over it: the far one only where the near wasn't.
	TVert v[4] = { { 0xFF0000FF, 0, 0, 100, 0 }, { 0xFF0000FF, 32, 32, 100, 0 }, { 0xFF00FF00, 16, 16, 200, 0 }, { 0xFF00FF00, 48, 48, 200, 0 } };
	PutVerts(VERTS, v, 4);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 4, VERTS).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(20, 20), 0xFF0000FFu);
	CHECK_EQ(Pixel32(40, 40), 0xFF00FF00u);
	CHECK_EQ(Depth(20, 20), 100u);
	CHECK_EQ(Depth(40, 40), 200u);
	CHECK_EQ(Depth(60, 60), 0xFFFFu);

	// Depth write disabled: tested but not written.
	DL dl2(LIST + 0x2000);
	TVert v2[2] = { { 0xFFFFFFFF, 0, 0, 50, 0 }, { 0xFFFFFFFF, 8, 8, 50, 0 } };
	PutVerts(VERTS + 0x100, v2, 2);
	dl2.C(GE_CMD_ZWRITEDISABLE, 1).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x100).Finish();
	RunList(dl2);
	CHECK_EQ(Pixel32(4, 4), 0xFFFFFFFFu);
	CHECK_EQ(Depth(4, 4), 100u);
}

TEST(gpu_stencil) {
	GeEnv env;
	DL dl;
	dl.Target();
	// Clear color and stencil (alpha) to 0.
	ClearScreen(dl, 0x00000000);
	// Write stencil 0x80 where a sprite is drawn: stencil always, zpass REPLACE.
	dl.C(GE_CMD_STENCILTESTENABLE, 1).C(GE_CMD_STENCILTEST, GE_COMP_ALWAYS | (0x80 << 8) | (0xFF << 16));
	dl.C(GE_CMD_STENCILOP, GE_STENCILOP_KEEP | (GE_STENCILOP_KEEP << 8) | (GE_STENCILOP_REPLACE << 16));
	TVert v[2] = { { 0xFF0000FF, 0, 0, 0, 0 }, { 0xFF0000FF, 16, 16, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS);
	// Then draw green only where stencil == 0x80 (ref == stencil), keeping the stencil.
	dl.C(GE_CMD_STENCILTEST, GE_COMP_EQUAL | (0x80 << 8) | (0xFF << 16)).C(GE_CMD_STENCILOP, 0);
	TVert v2[2] = { { 0xFF00FF00, 8, 8, 0, 0 }, { 0xFF00FF00, 24, 24, 0, 0 } };
	PutVerts(VERTS + 0x40, v2, 2);
	dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x40).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(4, 4), 0x800000FFu);
	CHECK_EQ(Pixel32(12, 12), 0x8000FF00u);
	CHECK_EQ(Pixel32(20, 20), 0x00000000u);
}

TEST(gpu_framebuffer_565_and_readback) {
	GeEnv env;
	DL dl;
	dl.Target(GE_FORMAT_565);
	ClearScreen(dl, 0xFF000000);
	TVert v[2] = { { 0xFFFF8040, 0, 0, 0, 0 }, { 0xFFFF8040, 4, 4, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl);
	// R 0x40 -> 8, G 0x80 -> 32, B 0xFF -> 31.
	CHECK_EQ(Pixel16(1, 1), (u16)(8 | (32 << 5) | (31 << 11)));
	CHECK_EQ(Pixel16(5, 1), 0u);
	u32 out[8 * 2];
	ReadFramebuffer(VRAM, 512, 0, 8, 2, out);
	// 5/6 bits expanded: 8 -> 0x42, 32 -> 0x82, 31 -> 0xFF.
	CHECK_EQ(out[1], 0xFFFF8242u);
	CHECK_EQ(out[5], 0xFF000000u);
}

TEST(gpu_dither_and_color_mask) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	// Mask out green writes.
	dl.C(GE_CMD_MASKRGB, 0x00FF00);
	TVert v[2] = { { 0xFFFFFFFF, 0, 0, 0, 0 }, { 0xFFFFFFFF, 4, 4, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).C(GE_CMD_MASKRGB, 0);
	// Dither -1 everywhere on a mid-gray.
	dl.C(GE_CMD_DITHERENABLE, 1);
	for (int i = 0; i < 4; ++i)
		dl.C(GE_CMD_DITH0 + i, 0xFFFF);
	TVert v2[2] = { { 0xFF808080, 8, 0, 0, 0 }, { 0xFF808080, 12, 4, 0, 0 } };
	PutVerts(VERTS + 0x40, v2, 2);
	dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x40).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(1, 1), 0xFFFF00FFu);
	CHECK_EQ(Pixel32(9, 1), 0xFF7F7F7Fu);
}

TEST(gpu_logic_op_and_alpha_test) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF0F0F0F);
	dl.C(GE_CMD_LOGICOPENABLE, 1).C(GE_CMD_LOGICOP, GE_LOGIC_XOR);
	TVert v[2] = { { 0xFFFF00FF, 0, 0, 0, 0 }, { 0xFFFF00FF, 4, 4, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS).C(GE_CMD_LOGICOPENABLE, 0);
	// Alpha test GREATER 0x80: a fragment with alpha 0x40 is discarded.
	dl.C(GE_CMD_ALPHATESTENABLE, 1).C(GE_CMD_ALPHATEST, GE_COMP_GREATER | (0x80 << 8) | (0xFF << 16));
	TVert v2[2] = { { 0x40FFFFFF, 8, 0, 0, 0 }, { 0x40FFFFFF, 12, 4, 0, 0 }, };
	PutVerts(VERTS + 0x40, v2, 2);
	dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x40).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(1, 1), 0xFFF00FF0u);
	CHECK_EQ(Pixel32(9, 1), 0xFF0F0F0Fu);
}

TEST(gpu_transformed_triangle) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	// Identity world and view, an orthographic projection mapping x, y in [-1, 1] to the viewport.
	dl.C(GE_CMD_WORLDMATRIXNUMBER, 0);
	const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	for (float f : ident43)
		dl.F(GE_CMD_WORLDMATRIXDATA, f);
	dl.C(GE_CMD_VIEWMATRIXNUMBER, 0);
	for (float f : ident43)
		dl.F(GE_CMD_VIEWMATRIXDATA, f);
	dl.C(GE_CMD_PROJMATRIXNUMBER, 0);
	const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	for (float f : ident44)
		dl.F(GE_CMD_PROJMATRIXDATA, f);
	// Viewport: 480x272 around (2048, 2048), offset (2048 - 240, 2048 - 136).
	dl.F(GE_CMD_VIEWPORTXSCALE, 240.0f).F(GE_CMD_VIEWPORTYSCALE, -136.0f).F(GE_CMD_VIEWPORTZSCALE, 32767.5f);
	dl.F(GE_CMD_VIEWPORTXCENTER, 2048.0f).F(GE_CMD_VIEWPORTYCENTER, 2048.0f).F(GE_CMD_VIEWPORTZCENTER, 32767.5f);
	dl.C(GE_CMD_OFFSETX, (2048 - 240) << 4).C(GE_CMD_OFFSETY, (2048 - 136) << 4);
	dl.C(GE_CMD_MINZ, 0).C(GE_CMD_MAXZ, 0xFFFF);
	// The left half of the screen: (-1, 1), (0, 1), (-1, -1), counter-clockwise on screen.
	FVert v[3] = { { 0xFF00FFFF, -1.0f, 1.0f, 0.0f }, { 0xFF00FFFF, -1.0f, -1.0f, 0.0f }, { 0xFF00FFFF, 0.0f, 1.0f, 0.0f } };
	PutVerts(VERTS, v, 3);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_PF).Prim(GE_PRIM_TRIANGLES, 3, VERTS).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(10, 10), 0xFF00FFFFu);
	CHECK_EQ(Pixel32(200, 10), 0xFF00FFFFu);
	CHECK_EQ(Pixel32(5, 260), 0xFF00FFFFu);
	CHECK_EQ(Pixel32(300, 10), 0xFF000000u);
	CHECK_EQ(Pixel32(230, 260), 0xFF000000u);

	// Backface culling: (0,0), (240,0), (0,272) is counter-clockwise on screen (y down).
	FVert v2[3] = { v[0], v[2], v[1] };
	PutVerts(VERTS + 0x100, v2, 3);
	DL dl2(LIST + 0x3000);
	ClearScreen(dl2, 0xFF000000);
	dl2.C(GE_CMD_CULLFACEENABLE, 1).C(GE_CMD_CULL, 0).C(GE_CMD_VERTEXTYPE, VT_C8888_PF).Prim(GE_PRIM_TRIANGLES, 3, VERTS + 0x100).Finish();
	RunList(dl2);
	CHECK_EQ(Pixel32(10, 10), 0xFF000000u);
	DL dl3(LIST + 0x4000);
	dl3.C(GE_CMD_CULL, 1).Prim(GE_PRIM_TRIANGLES, 3, VERTS + 0x100).Finish();
	RunList(dl3);
	CHECK_EQ(Pixel32(10, 10), 0xFF00FFFFu);
}

TEST(gpu_near_plane_clip) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	dl.C(GE_CMD_WORLDMATRIXNUMBER, 0);
	for (float f : ident43)
		dl.F(GE_CMD_WORLDMATRIXDATA, f);
	dl.C(GE_CMD_VIEWMATRIXNUMBER, 0);
	for (float f : ident43)
		dl.F(GE_CMD_VIEWMATRIXDATA, f);
	// Perspective: w = -z (camera looks down -z), z' = z * a + b.
	const float n = 1.0f, f = 100.0f;
	const float a = (f + n) / (n - f), b = 2 * f * n / (n - f);
	const float proj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, a, -1, 0, 0, b, 0 };
	dl.C(GE_CMD_PROJMATRIXNUMBER, 0);
	for (float p : proj)
		dl.F(GE_CMD_PROJMATRIXDATA, p);
	dl.F(GE_CMD_VIEWPORTXSCALE, 240.0f).F(GE_CMD_VIEWPORTYSCALE, -136.0f).F(GE_CMD_VIEWPORTZSCALE, 32767.5f);
	dl.F(GE_CMD_VIEWPORTXCENTER, 2048.0f).F(GE_CMD_VIEWPORTYCENTER, 2048.0f).F(GE_CMD_VIEWPORTZCENTER, 32767.5f);
	dl.C(GE_CMD_OFFSETX, (2048 - 240) << 4).C(GE_CMD_OFFSETY, (2048 - 136) << 4);
	dl.C(GE_CMD_MINZ, 0).C(GE_CMD_MAXZ, 0xFFFF).C(GE_CMD_DEPTHCLIPENABLE, 1);
	// A floor-like triangle crossing the near plane: one vertex behind the camera.
	FVert v[3] = { { 0xFF0000FF, -2.0f, -1.0f, -4.0f }, { 0xFF0000FF, 2.0f, -1.0f, -4.0f }, { 0xFF0000FF, 0.0f, -1.0f, 2.0f } };
	PutVerts(VERTS, v, 3);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_PF).Prim(GE_PRIM_TRIANGLES, 3, VERTS).Finish();
	RunList(dl);
	// The visible part covers the lower middle of the screen.
	CHECK_EQ(Pixel32(240, 250), 0xFF0000FFu);
	CHECK_EQ(Pixel32(240, 60), 0xFF000000u);
}

TEST(gpu_lighting_directional) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	dl.C(GE_CMD_WORLDMATRIXNUMBER, 0);
	for (float f : ident43)
		dl.F(GE_CMD_WORLDMATRIXDATA, f);
	dl.C(GE_CMD_VIEWMATRIXNUMBER, 0);
	for (float f : ident43)
		dl.F(GE_CMD_VIEWMATRIXDATA, f);
	const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	dl.C(GE_CMD_PROJMATRIXNUMBER, 0);
	for (float f : ident44)
		dl.F(GE_CMD_PROJMATRIXDATA, f);
	dl.F(GE_CMD_VIEWPORTXSCALE, 240.0f).F(GE_CMD_VIEWPORTYSCALE, -136.0f).F(GE_CMD_VIEWPORTZSCALE, 32767.5f);
	dl.F(GE_CMD_VIEWPORTXCENTER, 2048.0f).F(GE_CMD_VIEWPORTYCENTER, 2048.0f).F(GE_CMD_VIEWPORTZCENTER, 32767.5f);
	dl.C(GE_CMD_OFFSETX, (2048 - 240) << 4).C(GE_CMD_OFFSETY, (2048 - 136) << 4);
	dl.C(GE_CMD_MINZ, 0).C(GE_CMD_MAXZ, 0xFFFF);
	// One directional light along +z, diffuse white; material diffuse 0x808080, no ambient.
	dl.C(GE_CMD_LIGHTINGENABLE, 1).C(GE_CMD_LIGHTENABLE0, 1).C(GE_CMD_LIGHTTYPE0, GE_LIGHTCOMP_ONLYDIFFUSE);
	dl.F(GE_CMD_LX0, 0.0f).F(GE_CMD_LX0 + 1, 0.0f).F(GE_CMD_LX0 + 2, 1.0f);
	dl.C(GE_CMD_LAC0 + 1, 0xFFFFFF);
	dl.C(GE_CMD_MATERIALDIFFUSE, 0x808080).C(GE_CMD_MATERIALAMBIENT, 0).C(GE_CMD_MATERIALALPHA, 0xFF);
	dl.C(GE_CMD_AMBIENTCOLOR, 0).C(GE_CMD_AMBIENTALPHA, 0xFF).C(GE_CMD_MATERIALUPDATE, 0);
	// Normal along +z (float normals), position float, no color.
	struct NVert {
		float nx, ny, nz;
		float x, y, z;
	};
	NVert v[2] = { { 0, 0, 1, -0.5f, 0.5f, 0.0f }, { 0, 0, 1, 0.5f, -0.5f, 0.0f } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, GE_VTYPE_NRM_FLOAT | GE_VTYPE_POS_FLOAT).Prim(GE_PRIM_RECTANGLES, 2, VERTS).Finish();
	RunList(dl);
	const u32 c = Pixel32(240, 136);
	// N.L = 1: ((2*255+1) * (2*128+1)) >> 10 = 128 per channel.
	CHECK_EQ(c & 0xFFFFFF, 0x808080u);
	CHECK_EQ(Pixel32(10, 10), 0xFF000000u);
}

TEST(gpu_block_transfer) {
	GeEnv env;
	for (int i = 0; i < 64 * 16; ++i)
		GWrite32(RAM + 0x10000 + i * 4, 0xAB000000u | (u32)i);
	DL dl;
	dl.C(GE_CMD_TRANSFERSRC, (RAM + 0x10000) & 0xFFFFF0).C(GE_CMD_TRANSFERSRCW, 64 | (((RAM + 0x10000) >> 8) & 0xFF0000));
	dl.C(GE_CMD_TRANSFERDST, 0).C(GE_CMD_TRANSFERDSTW, 512 | ((VRAM >> 8) & 0xFF0000));
	dl.C(GE_CMD_TRANSFERSRCPOS, 2 | (1 << 10)).C(GE_CMD_TRANSFERDSTPOS, 10 | (20 << 10));
	dl.C(GE_CMD_TRANSFERSIZE, (8 - 1) | ((4 - 1) << 10)).C(GE_CMD_TRANSFERSTART, 1).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(10, 20), 0xAB000000u | (1 * 64 + 2));
	CHECK_EQ(Pixel32(17, 23), 0xAB000000u | (4 * 64 + 9));
	CHECK_EQ(Pixel32(18, 23), 0u);
	CHECK_EQ(Pixel32(10, 24), 0u);
}

TEST(gpu_bezier_patch) {
	GeEnv env;
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000);
	const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	dl.C(GE_CMD_WORLDMATRIXNUMBER, 0);
	for (float f : ident43)
		dl.F(GE_CMD_WORLDMATRIXDATA, f);
	dl.C(GE_CMD_VIEWMATRIXNUMBER, 0);
	for (float f : ident43)
		dl.F(GE_CMD_VIEWMATRIXDATA, f);
	const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	dl.C(GE_CMD_PROJMATRIXNUMBER, 0);
	for (float f : ident44)
		dl.F(GE_CMD_PROJMATRIXDATA, f);
	dl.F(GE_CMD_VIEWPORTXSCALE, 240.0f).F(GE_CMD_VIEWPORTYSCALE, -136.0f).F(GE_CMD_VIEWPORTZSCALE, 32767.5f);
	dl.F(GE_CMD_VIEWPORTXCENTER, 2048.0f).F(GE_CMD_VIEWPORTYCENTER, 2048.0f).F(GE_CMD_VIEWPORTZCENTER, 32767.5f);
	dl.C(GE_CMD_OFFSETX, (2048 - 240) << 4).C(GE_CMD_OFFSETY, (2048 - 136) << 4);
	dl.C(GE_CMD_MINZ, 0).C(GE_CMD_MAXZ, 0xFFFF);
	// A flat 4x4 patch over [-0.5, 0.5]^2.
	FVert cp[16];
	for (int j = 0; j < 4; ++j)
		for (int i = 0; i < 4; ++i)
			cp[j * 4 + i] = { 0xFF00FF00, -0.5f + i / 3.0f, 0.5f - j / 3.0f, 0.0f };
	PutVerts(VERTS, cp, 16);
	dl.C(GE_CMD_PATCHDIVISION, 4 | (4 << 8)).C(GE_CMD_PATCHPRIMITIVE, GE_PATCHPRIM_TRIANGLES);
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_PF).Addr(GE_CMD_VADDR, VERTS).C(GE_CMD_BEZIER, 4 | (4 << 8)).Finish();
	RunList(dl);
	CHECK_EQ(Pixel32(240, 136), 0xFF00FF00u);
	CHECK_EQ(Pixel32(100, 136), 0xFF000000u);
	CHECK_EQ(Pixel32(240, 50), 0xFF000000u);
	// Front-facing either way with culling off: something was drawn near the patch corners.
	CHECK_EQ(Pixel32(125, 72), 0xFF00FF00u);
}

TEST(gpu_context_and_matrix_readback) {
	GeEnv env;
	DL dl;
	dl.C(GE_CMD_WORLDMATRIXNUMBER, 0);
	for (int i = 0; i < 12; ++i)
		dl.F(GE_CMD_WORLDMATRIXDATA, (float)(i + 1));
	dl.C(GE_CMD_ALPHATEST, 0x123456).Finish();
	RunList(dl);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), ((u32)GE_CMD_ALPHATEST << 24) | 0x123456u);
	CHECK_EQ(GetCmd(GE_CMD_WORLDMATRIXNUMBER), ((u32)GE_CMD_WORLDMATRIXNUMBER << 24) | 12u);
	CHECK_EQ(GetMatrix(GE_MTX_WORLD, RAM + 0x1000), 0);
	CHECK_EQ(GRead32(RAM + 0x1000), 0x3F8000u);   // 1.0f >> 8
	CHECK_EQ(GRead32(RAM + 0x1000 + 44), 0x414000u);  // 12.0f >> 8
	CHECK_EQ(GetMatrix(99, RAM + 0x1000), (int)0x80000102);

	CHECK_EQ(SaveContext(RAM + 0x2000), 0);
	DL dl2(LIST + 0x1000);
	dl2.C(GE_CMD_ALPHATEST, 0).C(GE_CMD_WORLDMATRIXNUMBER, 0).F(GE_CMD_WORLDMATRIXDATA, 7.0f).Finish();
	RunList(dl2);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), (u32)GE_CMD_ALPHATEST << 24);
	CHECK_EQ(RestoreContext(RAM + 0x2000), 0);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), ((u32)GE_CMD_ALPHATEST << 24) | 0x123456u);
	CHECK_EQ(GetMatrix(GE_MTX_WORLD, RAM + 0x1000), 0);
	CHECK_EQ(GRead32(RAM + 0x1000), 0x3F8000u);
}

namespace {

// A busy scene: many overlapping blended, textured and depth tested triangles and sprites.
void DrawScene() {
	u32 seed = 12345;
	auto rnd = [&]() {
		seed = seed * 1103515245u + 12345u;
		return (seed >> 8) & 0xFFFF;
	};
	for (int i = 0; i < 64 * 64; ++i)
		GWrite16(TEX + i * 2, (u16)(rnd() | 0x8000));
	DL dl;
	dl.Target();
	ClearScreen(dl, 0xFF000000, 0xFFFF);
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_5551).C(GE_CMD_TEXMODE, 0);
	dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 64 | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 6 | (6 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_MODULATE | 0x100).C(GE_CMD_TEXFILTER, 1 | (1 << 8));
	dl.C(GE_CMD_ALPHABLENDENABLE, 1).C(GE_CMD_BLENDMODE, GE_BLEND_SRCALPHA | (GE_BLEND_INVSRCALPHA << 4));
	dl.C(GE_CMD_ZTESTENABLE, 1).C(GE_CMD_ZTEST, GE_COMP_LEQUAL).C(GE_CMD_SHADEMODE, 1);
	dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16);
	// Small triangles around random centers, then sprites.
	const int n = 3000, nr = 300;
	std::vector<TTVert> v(n * 3 + nr * 2);
	for (int i = 0; i < n * 3 + nr * 2; ++i) {
		const int cx = (int)(rnd() % 520) - 20, cy = (int)(rnd() % 312) - 20;
		v[i].u = (u16)(rnd() % 128);
		v[i].v = (u16)(rnd() % 128);
		v[i].color = 0x80000000u | (rnd() << 8) | (rnd() & 0xFF);
		v[i].z = (s16)(rnd() % 30000);
		v[i].pad = 0;
		if (i % 3 == 0 || i >= n * 3) {
			v[i].x = (s16)cx;
			v[i].y = (s16)cy;
		} else {
			v[i].x = (s16)(v[i - (i % 3)].x + (int)(rnd() % 64) - 32);
			v[i].y = (s16)(v[i - (i % 3)].y + (int)(rnd() % 64) - 32);
		}
		if (i >= n * 3 && (i - n * 3) % 2 == 1) {
			v[i].x = (s16)(v[i - 1].x + (int)(rnd() % 48));
			v[i].y = (s16)(v[i - 1].y + (int)(rnd() % 48));
		}
	}
	PutVerts(VERTS, v.data(), n * 3 + nr * 2);
	dl.Prim(GE_PRIM_TRIANGLES, n * 3, VERTS);
	dl.Prim(GE_PRIM_RECTANGLES, nr * 2, VERTS + n * 3 * sizeof(TTVert));
	dl.Finish();
	RunList(dl);
}

u64 HashVram() {
	u64 h = 1469598103934665603ull;
	const u8 *p = GPtr(VRAM);
	for (u32 i = 0; i < 0x200000; ++i)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

}  // namespace

TEST(gpu_threads_deterministic) {
	u64 single, multi;
	{
		GeEnv env(1);
		DrawScene();
		single = HashVram();
	}
	{
		GeEnv env(8);
		DrawScene();
		multi = HashVram();
		const Stats st = GetStats();
		CHECK(st.threadedFlushes > 0);
	}
	CHECK_EQ(single, multi);
}

TEST(gpu_perf_fullscreen_sprites) {
	// Not a pass/fail benchmark: 60 frames of 4 full-screen blended, textured sprites.
	GeEnv env(0);
	for (int i = 0; i < 256 * 256; ++i)
		GWrite32(TEX + i * 4, 0x80FFFFFFu ^ (u32)(i * 2654435761u));
	DL dl;
	dl.Target();
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_8888).C(GE_CMD_TEXMODE, 0);
	dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 256 | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 8 | (8 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_MODULATE | 0x100).C(GE_CMD_TEXFILTER, 1 | (1 << 8));
	dl.C(GE_CMD_ALPHABLENDENABLE, 1).C(GE_CMD_BLENDMODE, GE_BLEND_SRCALPHA | (GE_BLEND_INVSRCALPHA << 4));
	TTVert v[2] = { { 0, 0, 0xFFFFFFFF, 0, 0, 0, 0 }, { 256, 256, 0xFFFFFFFF, 480, 272, 0, 0 } };
	PutVerts(VERTS, v, 2);
	dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16);
	for (int i = 0; i < 4; ++i)
		dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS);
	dl.Finish();
	auto t0 = std::chrono::steady_clock::now();
	for (int frame = 0; frame < 60; ++frame) {
		EnqueueList(dl.start, 0, -1, 0, false);
		Run();
		DrawSync(0);
	}
	auto t1 = std::chrono::steady_clock::now();
	const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	printf("  60 frames x 4 full-screen textured blended sprites: %.1f ms (%.2f ms/frame)\n", ms, ms / 60);
	CHECK(ms < 60000.0);
}
