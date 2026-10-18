// SPDX-License-Identifier: GPL-2.0-or-later
// Hardware backends against the software reference: the same display lists and guest VRAM.
// Windows uses WARP; Linux uses a real OpenGL device when available.
#if PSPC_TEST_GPU_HW || PSPC_TEST_GPU_GL
#include "test_guest_memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "allegrex/guestmem.h"
#include "gpu/backend.h"
#include "gpu/ge.h"
#include "gpu/ge_constants.h"
#ifdef _WIN32
#include "gpu/hw/d3d11_backend.h"
#else
#include "gpu/hw/opengl_backend.h"
#include <SDL.h>
#endif
#include "testing.h"

using namespace gpu;

namespace {
#ifdef _WIN32
using BackendOptions = hw::D3D11Options;
auto MakeBackend(const BackendOptions &o, std::string *err) { return hw::CreateD3D11Backend(o,err); }
#else
using BackendOptions = hw::OpenGLOptions;
auto MakeBackend(const BackendOptions &o, std::string *err) {
	struct Video {
		bool initialized = SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
		~Video() { if (initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO); }
	};
	static Video video;
	auto backend = hw::CreateOpenGLBackend(o,err);
	if (!backend && getenv("PSPC_RENDERER_REQUIRE")) { printf("OpenGL unavailable: %s\n",err->c_str()); CHECK(backend != nullptr); }
	return backend;
}
#endif

constexpr u32 VRAM = 0x04000000;
constexpr u32 RAM = 0x08000000;
constexpr u32 RAM_SIZE = 0x00800000;
constexpr u32 ZBUF = 0x00088000;
constexpr u32 LIST = RAM + 0x100000;
constexpr u32 VERTS = RAM + 0x200000;
constexpr u32 TEX = RAM + 0x300000;
constexpr u32 CLUTA = RAM + 0x380000;

struct Env {
	u8 *mem = nullptr;
	u8 *saved = nullptr;
	Env() {
		mem = gputest::Reserve(0x0A000000);
		gputest::Commit(mem + VRAM, 0x200000);
		gputest::Commit(mem + RAM, RAM_SIZE);
		gputest::Commit(mem + 0x10000, 0x4000);
		saved = g_membase;
		g_membase = mem;
		SetRamSize(RAM_SIZE);
		SetSdkVersion(0x06060010);
		Init();
		SetRenderThreads(1);
	}
	~Env() {
		SetRenderBackend(nullptr);
		Shutdown();
		g_membase = saved;
		gputest::Release(mem, 0x0A000000);
	}
};

struct DL {
	u32 start, pc;
	DL() : start(LIST), pc(LIST) {}
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
	DL &Prim(int prim, int count, u32 verts) { return Addr(GE_CMD_VADDR, verts).C(GE_CMD_PRIM, ((u32)prim << 16) | count); }
};

struct TVert {
	u32 color;
	s16 x, y, z;
	u16 pad;
};
struct TTVert {
	u16 u, v;
	u32 color;
	s16 x, y, z;
	u16 pad;
};
struct FVert {
	u32 color;
	float x, y, z;
};
constexpr u32 VT_C8888_P16 = GE_VTYPE_THROUGH | GE_VTYPE_COL_8888 | GE_VTYPE_POS_16BIT;
constexpr u32 VT_T16_C8888_P16 = GE_VTYPE_THROUGH | GE_VTYPE_TC_16BIT | GE_VTYPE_COL_8888 | GE_VTYPE_POS_16BIT;
constexpr u32 VT_C8888_PF = GE_VTYPE_COL_8888 | GE_VTYPE_POS_FLOAT;

u32 g_seed;
u32 Rnd() {
	g_seed = g_seed * 1103515245u + 12345u;
	return (g_seed >> 8) & 0xFFFF;
}

// One scene: clears, CLUT and direct-color textures (bilinear and nearest), blending, depth, stencil,
// logic op, color mask, dithering, fog in transform mode, on a framebuffer of format fbFmt.
void DrawScene(int fbFmt) {
	g_seed = 4242;
	for (int i = 0; i < 64 * 64; ++i)
		GWrite16(TEX + i * 2, (u16)Rnd());
	for (int i = 0; i < 128 * 128; ++i)
		GWrite8(TEX + 0x10000 + i, (u8)Rnd());
	for (int i = 0; i < 256; ++i)
		GWrite32(CLUTA + i * 4, 0x40000000u * (i & 3) | (Rnd() << 8) | (Rnd() & 0xFF));
	DL dl;
	dl.C(GE_CMD_FRAMEBUFPTR, 0).C(GE_CMD_FRAMEBUFWIDTH, 512).C(GE_CMD_FRAMEBUFPIXFORMAT, fbFmt);
	dl.C(GE_CMD_ZBUFPTR, ZBUF).C(GE_CMD_ZBUFWIDTH, 512);
	dl.C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 479 | (271 << 10)).C(GE_CMD_REGION1, 0).C(GE_CMD_REGION2, 479 | (271 << 10));
	// Clear color, stencil and depth.
	const s16 cz = (s16)(u16)0x8000;
	TVert clr[2] = { { 0x80204060, 0, 0, cz, 0 }, { 0x80204060, 480, 272, cz, 0 } };
	memcpy(GPtr(VERTS + 0x70000), clr, sizeof(clr));
	dl.C(GE_CMD_CLEARMODE, 1 | (7 << 8)).C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x70000).C(GE_CMD_CLEARMODE, 0);
	// CLUT8 sprites, nearest, alpha test, stencil replace.
	dl.Addr(GE_CMD_CLUTADDR, CLUTA).C(GE_CMD_CLUTADDRUPPER, (CLUTA >> 8) & 0x0F0000).C(GE_CMD_CLUTFORMAT, GE_CMODE_32BIT_ABGR8888 | (0xFF << 8));
	dl.C(GE_CMD_LOADCLUT, 32);
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_CLUT8).C(GE_CMD_TEXMODE, 0);
	dl.C(GE_CMD_TEXADDR0, (TEX + 0x10000) & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 128 | (((TEX + 0x10000) >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 7 | (7 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100).C(GE_CMD_TEXFILTER, 0);
	dl.C(GE_CMD_ALPHATESTENABLE, 1).C(GE_CMD_ALPHATEST, GE_COMP_GREATER | (0x30 << 8) | (0xFF << 16));
	dl.C(GE_CMD_STENCILTESTENABLE, 1).C(GE_CMD_STENCILTEST, GE_COMP_ALWAYS | (0x5A << 8) | (0xFF << 16));
	dl.C(GE_CMD_STENCILOP, GE_STENCILOP_KEEP | (GE_STENCILOP_KEEP << 8) | (GE_STENCILOP_REPLACE << 16));
	std::vector<TTVert> sp(40);
	for (int i = 0; i < 20; ++i) {
		const s16 x = (s16)(Rnd() % 440), y = (s16)(Rnd() % 240);
		sp[2 * i] = { 0, 0, 0xFFFFFFFF, x, y, 0, 0 };
		sp[2 * i + 1] = { 128, 128, 0xFFFFFFFF, (s16)(x + 20 + Rnd() % 60), (s16)(y + 10 + Rnd() % 40), 0, 0 };
	}
	memcpy(GPtr(VERTS + 0x60000), sp.data(), sp.size() * sizeof(TTVert));
	dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16).Prim(GE_PRIM_RECTANGLES, 40, VERTS + 0x60000);
	dl.C(GE_CMD_ALPHATESTENABLE, 0);
	// Bilinear 5551 triangles, Gouraud, blended, depth tested, drawn where the stencil isn't 0x5A.
	dl.C(GE_CMD_STENCILTEST, GE_COMP_NOTEQUAL | (0x5A << 8) | (0xFF << 16)).C(GE_CMD_STENCILOP, 0);
	dl.C(GE_CMD_TEXFORMAT, GE_TFMT_5551).C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 64 | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 6 | (6 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_MODULATE | 0x100).C(GE_CMD_TEXFILTER, 1 | (1 << 8));
	dl.C(GE_CMD_ALPHABLENDENABLE, 1).C(GE_CMD_BLENDMODE, GE_BLEND_SRCALPHA | (GE_BLEND_INVSRCALPHA << 4));
	dl.C(GE_CMD_ZTESTENABLE, 1).C(GE_CMD_ZTEST, GE_COMP_LEQUAL).C(GE_CMD_SHADEMODE, 1);
	const int n = 600;
	std::vector<TTVert> tv(n * 3);
	for (int i = 0; i < n * 3; ++i) {
		TTVert &v = tv[i];
		v.u = (u16)(Rnd() % 128);
		v.v = (u16)(Rnd() % 128);
		v.color = 0x60000000u | (Rnd() << 8) | (Rnd() & 0xFF);
		v.z = (s16)(Rnd() % 40000);
		v.pad = 0;
		if (i % 3 == 0) {
			v.x = (s16)(Rnd() % 520 - 20);
			v.y = (s16)(Rnd() % 312 - 20);
		} else {
			v.x = (s16)(tv[i - i % 3].x + (int)(Rnd() % 80) - 40);
			v.y = (s16)(tv[i - i % 3].y + (int)(Rnd() % 80) - 40);
		}
	}
	memcpy(GPtr(VERTS), tv.data(), tv.size() * sizeof(TTVert));
	dl.Prim(GE_PRIM_TRIANGLES, n * 3, VERTS);
	// Additive blending, a logic op, a color mask and dithering on flat sprites.
	dl.C(GE_CMD_TEXTUREMAPENABLE, 0).C(GE_CMD_STENCILTESTENABLE, 0).C(GE_CMD_ZTESTENABLE, 0);
	dl.C(GE_CMD_BLENDMODE, GE_BLEND_FIX | (GE_BLEND_FIX << 4) | (GE_BLENDMODE_MUL_AND_ADD << 8)).C(GE_CMD_BLENDFIXEDA, 0x808080).C(GE_CMD_BLENDFIXEDB, 0xC0C0C0);
	TVert fl[6] = { { 0xFF3070F0, 20, 20, 0, 0 }, { 0xFF3070F0, 200, 120, 0, 0 },
		{ 0xFFF0F030, 150, 100, 0, 0 }, { 0xFFF0F030, 300, 200, 0, 0 }, { 0xFF808080, 250, 30, 0, 0 }, { 0xFF808080, 460, 250, 0, 0 } };
	memcpy(GPtr(VERTS + 0x50000), fl, sizeof(fl));
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x50000);
	dl.C(GE_CMD_ALPHABLENDENABLE, 0).C(GE_CMD_LOGICOPENABLE, 1).C(GE_CMD_LOGICOP, GE_LOGIC_XOR);
	dl.C(GE_CMD_MASKRGB, 0x0F0F0F).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x50000 + 2 * sizeof(TVert));
	dl.C(GE_CMD_LOGICOPENABLE, 0).C(GE_CMD_MASKRGB, 0).C(GE_CMD_DITHERENABLE, 1);
	dl.C(GE_CMD_DITH0, 0x1C3E).C(GE_CMD_DITH0 + 1, 0x2D4F).C(GE_CMD_DITH0 + 2, 0x3E1C).C(GE_CMD_DITH3, 0x4F2D);
	dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x50000 + 4 * sizeof(TVert));
	dl.C(GE_CMD_DITHERENABLE, 0);
	// Transform mode triangles with fog and depth writes.
	float id[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	float proj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1 };
	dl.C(GE_CMD_WORLDMATRIXNUMBER, 0);
	for (float f : id)
		dl.F(GE_CMD_WORLDMATRIXDATA, f);
	dl.C(GE_CMD_VIEWMATRIXNUMBER, 0);
	for (float f : id)
		dl.F(GE_CMD_VIEWMATRIXDATA, f);
	dl.C(GE_CMD_PROJMATRIXNUMBER, 0);
	for (float f : proj)
		dl.F(GE_CMD_PROJMATRIXDATA, f);
	dl.F(GE_CMD_VIEWPORTXSCALE, 240.0f).F(GE_CMD_VIEWPORTYSCALE, -136.0f).F(GE_CMD_VIEWPORTZSCALE, 30000.0f);
	dl.F(GE_CMD_VIEWPORTXCENTER, 2048.0f).F(GE_CMD_VIEWPORTYCENTER, 2048.0f).F(GE_CMD_VIEWPORTZCENTER, 32000.0f);
	dl.C(GE_CMD_OFFSETX, (2048 - 240) << 4).C(GE_CMD_OFFSETY, (2048 - 136) << 4);
	dl.C(GE_CMD_MINZ, 0).C(GE_CMD_MAXZ, 0xFFFF).C(GE_CMD_CULLFACEENABLE, 0);
	dl.C(GE_CMD_FOGENABLE, 1).F(GE_CMD_FOG1, 2.0f).F(GE_CMD_FOG2, 0.8f).C(GE_CMD_FOGCOLOR, 0x30A0C0);
	dl.C(GE_CMD_ZTESTENABLE, 1).C(GE_CMD_ZTEST, GE_COMP_GEQUAL).C(GE_CMD_ZWRITEDISABLE, 0);
	std::vector<FVert> fv(90);
	for (size_t i = 0; i < fv.size(); ++i) {
		fv[i].color = 0xFF000000u | (Rnd() << 8) | (Rnd() & 0xFF);
		fv[i].x = (float)((int)(Rnd() % 200) - 100) / 100.0f;
		fv[i].y = (float)((int)(Rnd() % 200) - 100) / 100.0f;
		fv[i].z = (float)(Rnd() % 100) / 100.0f;
	}
	memcpy(GPtr(VERTS + 0x40000), fv.data(), fv.size() * sizeof(FVert));
	dl.C(GE_CMD_VERTEXTYPE, VT_C8888_PF).Prim(GE_PRIM_TRIANGLES, (int)fv.size(), VERTS + 0x40000);
	dl.C(GE_CMD_FINISH).C(GE_CMD_END);
	EnqueueList(dl.start, 0, -1, 0, false);
	Run();
	DrawSync(0);
}

// A framebuffer pixel as 8-bit RGBA.
u32 Expand(const u8 *p, int fmt) {
	if (fmt == GE_FORMAT_8888) {
		u32 v;
		memcpy(&v, p, 4);
		return v;
	}
	u16 v;
	memcpy(&v, p, 2);
	u32 r, g, b, a = 0;
	if (fmt == GE_FORMAT_565) {
		r = (v & 0x1F) << 3, g = ((v >> 5) & 0x3F) << 2, b = ((v >> 11) & 0x1F) << 3;
	} else if (fmt == GE_FORMAT_5551) {
		r = (v & 0x1F) << 3, g = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3, a = (v >> 15) * 255;
	} else {
		r = (v & 0xF) << 4, g = ((v >> 4) & 0xF) << 4, b = ((v >> 8) & 0xF) << 4, a = (v >> 12) << 4;
	}
	return r | (g << 8) | (b << 16) | (a << 24);
}

// Pixels whose color (any channel, stencil included) differs by more than colorTol levels, or whose
// depth differs by more than depthTol, between two VRAM snapshots.
int CountDiffs(const std::vector<u8> &a, const std::vector<u8> &b, int fmt, int colorTol, int depthTol) {
	const int bpp = fmt == GE_FORMAT_8888 ? 4 : 2;
	int n = 0;
	for (int y = 0; y < 272; ++y) {
		for (int x = 0; x < 480; ++x) {
			const size_t f = (size_t)(y * 512 + x) * bpp, z = ZBUF + (size_t)(y * 512 + x) * 2;
			const u32 ca = Expand(&a[f], fmt), cb = Expand(&b[f], fmt);
			u16 za, zb;
			memcpy(&za, &a[z], 2);
			memcpy(&zb, &b[z], 2);
			bool off = std::abs((int)za - (int)zb) > depthTol;
			for (int c = 0; c < 4; ++c)
				off |= std::abs((int)((ca >> (8 * c)) & 0xFF) - (int)((cb >> (8 * c)) & 0xFF)) > colorTol;
			if (off) {
				if (n < 6 && getenv("PSPC_TEST_SHOWDIFF"))
					printf("  (%d,%d) color %08x vs %08x depth %04x vs %04x\n", x, y, ca, cb, za, zb);
				n++;
			}
		}
	}
	return n;
}

std::vector<u8> Render(int fbFmt, int scale) {
	Env env;
	std::unique_ptr<RenderBackend> backend;
	if (scale > 0) {
		BackendOptions o;
		o.scale = scale;
#ifdef _WIN32
		o.warp = true;
#endif
		std::string err;
		backend = MakeBackend(o, &err);
		if (!backend) {
			printf("  (D3D11 WARP unavailable: %s)\n", err.c_str());
			return {};
		}
		SetRenderBackend(backend.get());
	}
	DrawScene(fbFmt);
	std::vector<u8> vram(GPtr(VRAM), GPtr(VRAM) + 0x200000);
	SetRenderBackend(nullptr);
	return vram;
}

}  // namespace

TEST(gpu_hardware_matches_software) {
	for (int fmt : { GE_FORMAT_8888, GE_FORMAT_565, GE_FORMAT_5551, GE_FORMAT_4444 }) {
		const std::vector<u8> sw = Render(fmt, 0);
		const std::vector<u8> hw1 = Render(fmt, 1);
		if (hw1.empty())
			return;
		// The GPU's coverage and float interpolation aren't the GE's fixed-point planes: colors may be a
		// level or two off and depth a few units, so where triangles intersect or a texel boundary decides a
		// test, pixels can differ (about 2% in this busy scene).
		const int diffs = CountDiffs(sw, hw1, fmt, 8, 8);
		if (diffs > 480 * 272 / 25)
			printf("  format %d: %d pixels differ at 1x\n", fmt, diffs);
		CHECK(diffs <= 480 * 272 / 25);
		// Upscaled and written back: still close to the 1x image.
		const std::vector<u8> hw3 = Render(fmt, 3);
		const int diffs3 = CountDiffs(sw, hw3, fmt, 8, 8);
		if (diffs3 > 480 * 272 / 20)
			printf("  format %d: %d pixels differ at 3x\n", fmt, diffs3);
		CHECK(diffs3 <= 480 * 272 / 20);
		// Near the largest scale: a 7680 x 4080 surface (narrower than 2048 PSP pixels: within D3D11's
		// largest texture). (Odd scales: written back from the center sample, as at 1x; even ones average
		// the four around the center, further from the software image.)
		if (fmt == GE_FORMAT_8888) {
			const std::vector<u8> hw15 = Render(fmt, 15);
			const int diffs15 = CountDiffs(sw, hw15, fmt, 8, 8);
			if (diffs15 > 480 * 272 / 20)
				printf("  format %d: %d pixels differ at 15x\n", fmt, diffs15);
			CHECK(diffs15 <= 480 * 272 / 20);
		}
	}
}

namespace {

// A 32 x 32 texture split along its diagonal (white above, black below; alpha texAlpha) drawn 1:1, with
// nearest filtering, over mid gray at render scale 4, with the texture's alpha used or not (texture
// function RGBA / RGB). Returns the display image at outScale (0: the render scale).
std::vector<u32> RenderDiagonal(int textureFilter, int textureUpscale, int outScale, int *w, int *h,
                                u32 texAlpha = 0xFF000000u, bool textureAlpha = true) {
	Env env;
	BackendOptions o;
	o.scale = 4;
#ifdef _WIN32
	o.warp = true;
#endif
	o.textureFilter = textureFilter;
	o.textureUpscale = textureUpscale;
	std::string err;
	std::unique_ptr<RenderBackend> backend = MakeBackend(o, &err);
	if (!backend)
		return {};
	SetRenderBackend(backend.get());
	for (int v = 0; v < 32; ++v)
		for (int u = 0; u < 32; ++u)
			GWrite32(TEX + (u32)(v * 32 + u) * 4, texAlpha | (u + v < 32 ? 0xFFFFFFu : 0u));
	DL dl;
	dl.C(GE_CMD_FRAMEBUFPTR, 0).C(GE_CMD_FRAMEBUFWIDTH, 512).C(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888);
	dl.C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 479 | (271 << 10)).C(GE_CMD_REGION1, 0).C(GE_CMD_REGION2, 479 | (271 << 10));
	TVert clr[2] = { { 0xFF808080, 0, 0, 0, 0 }, { 0xFF808080, 480, 272, 0, 0 } };
	memcpy(GPtr(VERTS + 0x70000), clr, sizeof(clr));
	dl.C(GE_CMD_CLEARMODE, 1 | (7 << 8)).C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x70000).C(GE_CMD_CLEARMODE, 0);
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_8888).C(GE_CMD_TEXMODE, 0);
	dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 32 | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 5 | (5 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | (textureAlpha ? 0x100 : 0));
	dl.C(GE_CMD_TEXFILTER, 0).C(GE_CMD_TEXWRAP, 1 | (1 << 8));
	TTVert sp[2] = { { 0, 0, 0xFFFFFFFF, 100, 100, 0, 0 }, { 32, 32, 0xFFFFFFFF, 132, 132, 0, 0 } };
	memcpy(GPtr(VERTS + 0x60000), sp, sizeof(sp));
	dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x60000);
	dl.C(GE_CMD_FINISH).C(GE_CMD_END);
	EnqueueList(dl.start, 0, -1, 0, false);
	Run();
	DrawSync(0);
	std::vector<u32> img;
	bool displayed = backend->GetDisplayImage(VRAM, 512, GE_FORMAT_8888, 480, 272, img, w, h, outScale);
	CHECK(displayed);
	if (!displayed) img.clear();
	SetRenderBackend(nullptr);
	return img;
}

// Along the diagonal: the largest step between consecutive rows in where the white part ends, and how
// many pixels are neither white nor black (filtered).
void DiagonalShape(const std::vector<u32> &img, int w, int *maxStep, int *blended) {
	*maxStep = 0;
	*blended = 0;
	int last = -1;
	for (int y = 101 * 4; y < 131 * 4; ++y) {
		int x = 100 * 4;
		while (x < 132 * 4 && (img[(size_t)y * w + x] & 0xFF) >= 128)
			++x;
		if (last >= 0)
			*maxStep = std::max(*maxStep, std::abs(x - last));
		last = x;
		for (int i = 100 * 4; i < 132 * 4; ++i) {
			const u32 r = img[(size_t)y * w + i] & 0xFF;
			*blended += r > 16 && r < 239;
		}
	}
}

}  // namespace

TEST(gpu_hardware_texture_upscaling_and_filtering) {
	int w = 0, h = 0, step = 0, blended = 0;
	// The game's nearest filtering at 4x: a staircase, 4 render pixels a texel.
	std::vector<u32> plain = RenderDiagonal(0, 1, 0, &w, &h);
	if (plain.empty()) {
		printf("  (hardware device unavailable)\n");
		return;
	}
	CHECK(w == 1920 && h == 1088);
	DiagonalShape(plain, w, &step, &blended);
	CHECK(step == 4 && blended == 0);
	// Textures upscaled 4x with xBR: the diagonal is a line, a render pixel a row.
	std::vector<u32> xbr = RenderDiagonal(0, 4, 0, &w, &h);
	DiagonalShape(xbr, w, &step, &blended);
	if (step > 2)
		printf("  xBR diagonal: steps of %d pixels\n", step);
	CHECK(!xbr.empty() && step <= 2);
	// A texture drawn without its alpha (texture function RGB) that keeps 0 there: upscaled, its colors
	// stay (blended premultiplied by that alpha, all of it was black) and its diagonal is smooth too.
	std::vector<u32> noAlpha = RenderDiagonal(0, 4, 0, &w, &h, 0, false);
	CHECK(!noAlpha.empty());
	if (!noAlpha.empty()) {
		CHECK((noAlpha[(size_t)(102 * 4) * w + 102 * 4] & 0xFFFFFF) == 0xFFFFFF);
		CHECK((noAlpha[(size_t)(130 * 4) * w + 130 * 4] & 0xFFFFFF) == 0x000000);
		DiagonalShape(noAlpha, w, &step, &blended);
		if (step > 2)
			printf("  xBR diagonal without alpha: steps of %d pixels\n", step);
		CHECK(step <= 2);
	}
	// Linear filtering forced: the edge is blended.
	std::vector<u32> linear = RenderDiagonal(2, 1, 0, &w, &h);
	DiagonalShape(linear, w, &step, &blended);
	CHECK(!linear.empty() && blended > 100);
	// Read back at 2x: averaged down, flat areas keep their colors.
	std::vector<u32> half = RenderDiagonal(0, 1, 2, &w, &h);
	CHECK(w == 960 && h == 544 && !half.empty());
	if (!half.empty()) {
		CHECK((half[(size_t)(102 * 2) * w + 102 * 2] & 0xFFFFFF) == 0xFFFFFF);
		CHECK((half[(size_t)(130 * 2) * w + 130 * 2] & 0xFFFFFF) == 0x000000);
		CHECK((half[(size_t)(20 * 2) * w + 20 * 2] & 0xFFFFFF) == 0x808080);
	}
}

TEST(gpu_hardware_framebuffer_feedback_and_transfer) {
	for (int mode = 0; mode < 3; ++mode) {
		Env env;
		BackendOptions options; options.scale = mode == 2 ? 16 : 3;
#ifdef _WIN32
		options.warp = true;
#endif
		std::string error;
		auto backend = MakeBackend(options, &error);
		if (!backend) { printf("  hardware feedback test unavailable: %s\n", error.c_str()); return; }
		auto draw = [&](gpu::RenderBackend *renderer) {
			gpu::SetRenderBackend(nullptr);
			gpu::Shutdown(); gpu::Init();
			memset(GPtr(VRAM), 0, 2 * 1024 * 1024);
			gpu::SetRenderBackend(renderer);
			for (u32 i = 0; i < 256; ++i) GWrite32(CLUTA + i * 4, 0xFF000000 | (i << 8));
			GWrite32(TEX, 0xAA443322);
			DL dl;
			dl.C(GE_CMD_FRAMEBUFPTR, 0).C(GE_CMD_FRAMEBUFWIDTH, 64).C(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888);
			dl.C(GE_CMD_ZBUFPTR, 0x100000).C(GE_CMD_ZBUFWIDTH, 64);
			dl.C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 31 | (31 << 10)).C(GE_CMD_REGION2, 31 | (31 << 10));
			TVert clear[2] = {{0x01010101, 0, 0, 0, 0}, {0x01010101, 32, 32, 0, 0}};
			memcpy(GPtr(VERTS), clear, sizeof(clear));
			dl.C(GE_CMD_CLEARMODE, 1 | (7 << 8)).C(GE_CMD_VERTEXTYPE, VT_C8888_P16);
			dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS).C(GE_CMD_CLEARMODE, 0);
			// A guest block transfer changes one pixel of the just-rendered source.
			dl.C(GE_CMD_TRANSFERSRC, TEX & 0xFFFFF0).C(GE_CMD_TRANSFERSRCW, 1 | ((TEX >> 8) & 0xFF0000));
			dl.C(GE_CMD_TRANSFERDST, 0).C(GE_CMD_TRANSFERDSTW, 64 | ((VRAM >> 8) & 0xFF0000));
			dl.C(GE_CMD_TRANSFERSRCPOS, 0).C(GE_CMD_TRANSFERDSTPOS, 4 | (5 << 10));
			dl.C(GE_CMD_TRANSFERSIZE, 0).C(GE_CMD_TRANSFERSTART, 1);
			dl.C(GE_CMD_FRAMEBUFPTR, 0x20000).C(GE_CMD_TEXTUREMAPENABLE, 1);
			dl.C(GE_CMD_TEXFORMAT, mode ? GE_TFMT_CLUT8 : GE_TFMT_8888).C(GE_CMD_TEXMODE, mode == 2 ? 1 : 0);
			dl.C(GE_CMD_TEXADDR0, 0).C(GE_CMD_TEXBUFWIDTH0, (mode ? 256 : 64) | ((VRAM >> 8) & 0x0F0000));
			dl.C(GE_CMD_TEXSIZE0, 5 | (5 << 8)).C(GE_CMD_TEXFILTER, 0).C(GE_CMD_TEXWRAP, 0x101);
			dl.C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100);
			dl.Addr(GE_CMD_CLUTADDR, CLUTA).C(GE_CMD_CLUTADDRUPPER, (CLUTA >> 8) & 0x0F0000);
			dl.C(GE_CMD_CLUTFORMAT, GE_CMODE_32BIT_ABGR8888 | (0xFF << 8)).C(GE_CMD_LOADCLUT, 32);
			TTVert sprite[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0, 0}, {32, 32, 0xFFFFFFFF, 32, 32, 0, 0}};
			memcpy(GPtr(VERTS + 0x1000), sprite, sizeof(sprite));
			dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x1000);
			dl.C(GE_CMD_FINISH).C(GE_CMD_END);
			EnqueueList(dl.start, 0, -1, 0, false); Run(); DrawSync(0);
			std::vector<u32> pixels;
			for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
				pixels.push_back(GRead32(VRAM + 0x20000 + (y * 64 + x) * 4));
			gpu::SetRenderBackend(nullptr);
			return pixels;
		};
		auto reference = draw(nullptr), actual = draw(backend.get());
		CHECK(reference[0] != 0);
		CHECK(reference == actual);
	}
}

// Repeating a texture by whole periods must produce identical tiles even across negative UVs.
// Exercise both taps at the bilinear seam and non-power-of-two xBR/RTT periods.
TEST(gpu_hardware_texture_wrap_periods) {
	for (int mode = 0; mode < 6; ++mode) {
		Env env;
		BackendOptions options;
		options.scale = mode == 0 ? 1 : mode == 5 ? 16 : (mode & 1) ? 3 : 4;
		options.textureUpscale = mode == 1 ? 3 : mode == 2 ? 4 : 1;
#ifdef _WIN32
		options.warp = true;
#endif
		std::string error;
		auto backend = MakeBackend(options, &error);
		if (!backend) { printf("  hardware wrapping test unavailable: %s\n", error.c_str()); return; }
		SetRenderBackend(backend.get());
		const bool rtt = mode >= 3;
		const u32 texture = rtt ? VRAM + 0x20000 : TEX;
		for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
			GWrite32(texture + (y * 64 + x) * 4, 0xFF000000u | (x * 7) | ((y * 7) << 8) | (((x ^ y) * 7) << 16));
		DL dl;
		dl.C(GE_CMD_FRAMEBUFWIDTH, 512).C(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888).C(GE_CMD_ZWRITEDISABLE, 1);
		dl.C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 159 | (31 << 10)).C(GE_CMD_REGION2, 159 | (31 << 10));
		if (rtt) {
			// Touch the source with the GE so it is sampled as a rendered surface at the render scale.
			dl.C(GE_CMD_FRAMEBUFPTR, 0x20000).C(GE_CMD_FRAMEBUFWIDTH, 64);
			TVert clear[2] = {{0xFF000000u, 0, 0, 0, 0}, {0xFF000000u, 1, 1, 0, 0}};
			memcpy(GPtr(VERTS + 0x10000), clear, sizeof(clear));
			dl.C(GE_CMD_CLEARMODE, 1 | (3 << 8)).C(GE_CMD_VERTEXTYPE, VT_C8888_P16);
			dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x10000).C(GE_CMD_CLEARMODE, 0);
		}
		dl.C(GE_CMD_FRAMEBUFPTR, 0).C(GE_CMD_FRAMEBUFWIDTH, 512);
		dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_8888).C(GE_CMD_TEXMODE, 0);
		dl.C(GE_CMD_TEXADDR0, texture & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 64 | ((texture >> 8) & 0x0F0000));
		dl.C(GE_CMD_TEXSIZE0, 5 | (5 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100).C(GE_CMD_TEXWRAP, 0);
		struct V { float u, v; u32 color; float x, y, z; };
		const float coords[] = {0, 0.25f, 15.5f, 31.75f};
		for (int filter = 0; filter < 2; ++filter) {
			dl.C(GE_CMD_TEXFILTER, filter ? 0x101 : 0);
			for (int row = 0; row < 16; ++row)
			for (int tile = 0; tile < 5; ++tile) {
				// Constant UVs avoid interpolation rounding differences across the sign boundary. These
				// exact quarter-texel coordinates exercise bilinear wrap seams at every tested scale.
				const float u = (float)((tile - 2) * 32) + coords[row % 4], v = (float)((2 - tile) * 64) + coords[row / 4];
				const float y = (float)(filter * 16 + row);
				V verts[2] = {{u, v, 0xFFFFFFFFu, (float)(tile * 32), y, 0},
					{u, v, 0xFFFFFFFFu, (float)((tile + 1) * 32), y + 1, 0}};
				const u32 addr = VERTS + ((filter * 16 + row) * 5 + tile) * sizeof(verts);
				memcpy(GPtr(addr), verts, sizeof(verts));
				dl.C(GE_CMD_VERTEXTYPE, GE_VTYPE_THROUGH | GE_VTYPE_TC_FLOAT | GE_VTYPE_COL_8888 | GE_VTYPE_POS_FLOAT);
				dl.Prim(GE_PRIM_RECTANGLES, 2, addr);
			}
		}
		dl.C(GE_CMD_FINISH).C(GE_CMD_END);
		EnqueueList(dl.start, 0, -1, 0, false); Run(); DrawSync(0);
		std::vector<u32> image;
		int w = 0, h = 0;
		if (options.scale == 1) {
			w = 160; h = 32;
			for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
				image.push_back(GRead32(VRAM + (y * 512 + x) * 4));
		} else {
			CHECK(backend->GetDisplayImage(VRAM, 512, GE_FORMAT_8888, 160, 32, image, &w, &h, 0));
		}
		SetRenderBackend(nullptr);
		if (image.empty()) return;
		const int side = 32 * options.scale;
		int diffs = 0;
		for (int y = 0; y < h; ++y) for (int x = 0; x < side; ++x) for (int tile = 1; tile < 5; ++tile) {
			if (image[(size_t)y * w + x] != image[(size_t)y * w + tile * side + x]) {
				diffs++;
			}
		}
		if (diffs) printf("  wrap mode %d: %d differing repeated pixels\n", mode, diffs);
		CHECK(diffs == 0);
		CHECK((image[(size_t)(side / 2) * w + side / 2] & 0xFFFFFF) != 0);
	}
}

// Opt-in, bounded benchmark. Shader compilation and warm-up are outside the timer;
// reading VRAM forces completion on D3D11 as well as OpenGL. Use the hardware
// adapter, not WARP, when measuring Windows resolution scaling.
TEST(gpu_hardware_resolution_perf) {
	if (!getenv("GPU_HW_BENCH")) return;
	const int onlyScale = getenv("GPU_HW_BENCH_SCALE") ? atoi(getenv("GPU_HW_BENCH_SCALE")) : 0;
	const int frames = getenv("GPU_HW_BENCH_FRAMES") ? std::clamp(atoi(getenv("GPU_HW_BENCH_FRAMES")), 21, 10000) : 121;
	std::vector<int> scales{1, 2, 3, 4, 8, 16};
	if (onlyScale) {
		CHECK(onlyScale >= 1 && onlyScale <= 16);
		if (onlyScale < 1 || onlyScale > 16) return;
		scales = {onlyScale};
	}
	for (int scale : scales) {
		Env env;
		BackendOptions options;
		options.scale = scale;
		std::string error;
		auto backend = MakeBackend(options, &error);
		CHECK(backend != nullptr);
		if (!backend) { printf("  benchmark unavailable: %s\n", error.c_str()); return; }
		SetRenderBackend(backend.get());
		for (u32 i = 0; i < 256 * 256; ++i)
			GWrite32(TEX + i * 4, 0x80000000 | ((i * 2654435761u) & 0xFFFFFF));
		DL dl;
		dl.C(GE_CMD_FRAMEBUFPTR, 0).C(GE_CMD_FRAMEBUFWIDTH, 512).C(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888);
		dl.C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 479 | (271 << 10)).C(GE_CMD_REGION2, 479 | (271 << 10));
		dl.C(GE_CMD_ZWRITEDISABLE, 1);
		dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_8888).C(GE_CMD_TEXMODE, 0);
		dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 256 | ((TEX >> 8) & 0x0F0000));
		dl.C(GE_CMD_TEXSIZE0, 8 | (8 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100).C(GE_CMD_TEXFILTER, 0x101);
		dl.C(GE_CMD_ALPHABLENDENABLE, 1).C(GE_CMD_BLENDMODE, GE_BLEND_SRCALPHA | (GE_BLEND_INVSRCALPHA << 4));
		TTVert verts[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0, 0}, {512, 512, 0xFFFFFFFF, 480, 272, 0, 0}};
		memcpy(GPtr(VERTS), verts, sizeof(verts));
		dl.C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16);
		for (int i = 0; i < 4; ++i) dl.Prim(GE_PRIM_RECTANGLES, 2, VERTS);
		dl.C(GE_CMD_FINISH).C(GE_CMD_END);
		auto frame = [&] {
			EnqueueList(dl.start, 0, -1, 0, false); Run(); DrawSync(0);
			backend->SyncRange(VRAM, 512 * 272 * 4);
		};
		// A sustained warm-up lets a laptop GPU leave its idle clocks before sampling.
		const auto warmEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
		do { frame(); } while (std::chrono::steady_clock::now() < warmEnd);
		std::vector<double> times;
		for (int i = 0; i < frames; ++i) {
			const auto start = std::chrono::steady_clock::now();
			frame();
			times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		}
		std::sort(times.begin(), times.end());
		double mean = 0, variance = 0;
		for (double t : times) mean += t / times.size();
		for (double t : times) variance += (t - mean) * (t - mean) / (times.size() - 1);
		printf("  resolution %dx: median %.3f p99 %.3f stddev %.3f ms/frame (%d frames, 4 full-screen wrapped bilinear blended sprites)\n",
			scale, times[times.size() / 2], times[(times.size() - 1) * 99 / 100], std::sqrt(variance), frames);
		CHECK(GRead32(VRAM + (136 * 512 + 240) * 4) != 0);
		SetRenderBackend(nullptr);
	}
}

namespace {

// Lit, textured, fogged 3D drawn in transform mode: four light kinds, material colors from the vertex
// and not, specular as color 1 and added, a texture matrix and an environment map, skinning and
// morphing, flat shading, every cull mode, lists, indexed strips and fans, and triangles through the
// near plane with depth clipping on and off.
void DrawLitScene() {
	g_seed = 777;
	for (int i = 0; i < 64 * 64; ++i)
		GWrite16(TEX + i * 2, (u16)Rnd());
	auto frand = [](float lo, float hi) { return lo + (hi - lo) * (float)(Rnd() % 10000) / 10000.0f; };
	DL dl;
	dl.C(GE_CMD_FRAMEBUFPTR, 0).C(GE_CMD_FRAMEBUFWIDTH, 512).C(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888);
	dl.C(GE_CMD_ZBUFPTR, ZBUF).C(GE_CMD_ZBUFWIDTH, 512);
	dl.C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 479 | (271 << 10)).C(GE_CMD_REGION1, 0).C(GE_CMD_REGION2, 479 | (271 << 10));
	TVert clr[2] = { { 0xFF102030, 0, 0, 0, 0 }, { 0xFF102030, 480, 272, 0, 0 } };
	memcpy(GPtr(VERTS + 0x70000), clr, sizeof(clr));
	dl.C(GE_CMD_CLEARMODE, 1 | (7 << 8)).C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, VERTS + 0x70000).C(GE_CMD_CLEARMODE, 0);

	// A rotated world, the camera at the origin looking down -z, a perspective projection (near 1).
	const float c = std::cos(0.6f), s = std::sin(0.6f);
	const float world[12] = { c, 0, -s, 0.1f * s, 1, 0.2f, s, 0, c, 0.3f, -0.2f, -2.5f };
	const float view[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	const float n = 1.0f, f = 50.0f;
	const float proj[16] = { 1.2f, 0, 0, 0, 0, 2.0f, 0, 0, 0, 0, (f + n) / (n - f), -1, 0, 0, 2 * f * n / (n - f), 0 };
	const float tgen[12] = { 0.5f, 0.1f, 0, 0.2f, 0.5f, 0, 0.1f, 0.3f, 1, 0.5f, 0.5f, 0 };
	dl.C(GE_CMD_WORLDMATRIXNUMBER, 0);
	for (float v : world) dl.F(GE_CMD_WORLDMATRIXDATA, v);
	dl.C(GE_CMD_VIEWMATRIXNUMBER, 0);
	for (float v : view) dl.F(GE_CMD_VIEWMATRIXDATA, v);
	dl.C(GE_CMD_PROJMATRIXNUMBER, 0);
	for (float v : proj) dl.F(GE_CMD_PROJMATRIXDATA, v);
	dl.C(GE_CMD_TGENMATRIXNUMBER, 0);
	for (float v : tgen) dl.F(GE_CMD_TGENMATRIXDATA, v);
	// Two bones: nearly the identity, and turned and moved.
	const float bones[2][12] = { { 1.0f, 0.05f, 0, 0, 1.0f, 0, 0, 0, 0.9f, 0, 0, 0 },
	                             { 0.8f, -0.3f, 0.1f, 0.3f, 0.8f, 0, 0, 0.1f, 1.1f, 0.3f, -0.2f, 0.5f } };
	dl.C(GE_CMD_BONEMATRIXNUMBER, 0);
	for (const auto &bone : bones)
		for (float v : bone) dl.F(GE_CMD_BONEMATRIXDATA, v);
	dl.F(GE_CMD_VIEWPORTXSCALE, 240.0f).F(GE_CMD_VIEWPORTYSCALE, -136.0f).F(GE_CMD_VIEWPORTZSCALE, -30000.0f);
	dl.F(GE_CMD_VIEWPORTXCENTER, 2048.0f).F(GE_CMD_VIEWPORTYCENTER, 2048.0f).F(GE_CMD_VIEWPORTZCENTER, 32000.0f);
	dl.C(GE_CMD_OFFSETX, (2048 - 240) << 4).C(GE_CMD_OFFSETY, (2048 - 136) << 4);
	dl.C(GE_CMD_MINZ, 0).C(GE_CMD_MAXZ, 0xFFFF).C(GE_CMD_ZTESTENABLE, 1).C(GE_CMD_ZTEST, GE_COMP_GEQUAL).C(GE_CMD_ZWRITEDISABLE, 0);
	dl.C(GE_CMD_FOGENABLE, 1).F(GE_CMD_FOG1, 6.0f).F(GE_CMD_FOG2, 0.15f).C(GE_CMD_FOGCOLOR, 0x506070);
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFORMAT, GE_TFMT_5551).C(GE_CMD_TEXMODE, 0);
	dl.C(GE_CMD_TEXADDR0, TEX & 0xFFFFF0).C(GE_CMD_TEXBUFWIDTH0, 64 | ((TEX >> 8) & 0x0F0000));
	dl.C(GE_CMD_TEXSIZE0, 6 | (6 << 8)).C(GE_CMD_TEXFUNC, GE_TEXFUNC_MODULATE | 0x100).C(GE_CMD_TEXFILTER, 1 | (1 << 8));
	dl.F(GE_CMD_TEXSCALEU, 1.5f).F(GE_CMD_TEXSCALEV, 0.75f).F(GE_CMD_TEXOFFSETU, 0.25f).F(GE_CMD_TEXOFFSETV, -0.125f);

	// Lights: directional with specular, point with attenuation, spot with powered diffuse, point specular.
	dl.C(GE_CMD_LIGHTINGENABLE, 1);
	const u32 types[4] = { GE_LIGHTCOMP_BOTH | (GE_LIGHTTYPE_DIRECTIONAL << 8), GE_LIGHTCOMP_ONLYDIFFUSE | (GE_LIGHTTYPE_POINT << 8),
	                       GE_LIGHTCOMP_ONLYPOWDIFFUSE | (GE_LIGHTTYPE_SPOT << 8), GE_LIGHTCOMP_BOTH | (GE_LIGHTTYPE_POINT << 8) };
	const float lpos[4][3] = { { 0.3f, 0.8f, 0.5f }, { -1.0f, 1.0f, -1.5f }, { 0.5f, -0.5f, 0.0f }, { 1.5f, 0.5f, -3.0f } };
	const float ldir[4][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { -0.2f, 0.3f, -1.0f }, { 0, 0, 0 } };
	const float latt[4][3] = { { 1, 0, 0 }, { 0.5f, 0.3f, 0.1f }, { 1.0f, 0.2f, 0.0f }, { 0.2f, 0.0f, 0.4f } };
	const u32 lcol[4][3] = { { 0x202020, 0xC0C0C0, 0xFFFFFF }, { 0x101010, 0xFF8040, 0x000000 },
	                         { 0x000000, 0x40FF40, 0x000000 }, { 0x080808, 0x6060FF, 0xFFE0C0 } };
	for (int l = 0; l < 4; ++l) {
		dl.C(GE_CMD_LIGHTENABLE0 + l, 1).C(GE_CMD_LIGHTTYPE0 + l, types[l]);
		for (int i = 0; i < 3; ++i) {
			dl.F(GE_CMD_LX0 + 3 * l + i, lpos[l][i]).F(GE_CMD_LDX0 + 3 * l + i, ldir[l][i]).F(GE_CMD_LKA0 + 3 * l + i, latt[l][i]);
			dl.C(GE_CMD_LAC0 + 3 * l + i, lcol[l][i]);
		}
		dl.F(GE_CMD_LKS0 + l, 3.0f).F(GE_CMD_LKO0 + l, 0.6f);
	}
	dl.C(GE_CMD_MATERIALEMISSIVE, 0x101008).C(GE_CMD_MATERIALAMBIENT, 0x406080).C(GE_CMD_MATERIALALPHA, 0xC0);
	dl.C(GE_CMD_MATERIALDIFFUSE, 0xE0C0A0).C(GE_CMD_MATERIALSPECULAR, 0xFFFFFF).F(GE_CMD_MATERIALSPECULARCOEF, 12.0f);
	dl.C(GE_CMD_AMBIENTCOLOR, 0x303030).C(GE_CMD_AMBIENTALPHA, 0xFF);

	// Format A: float UV, 8888 color, float normal and position (36 bytes).
	struct VA { float u, v; u32 color; float nx, ny, nz, x, y, z; };
	const u32 VT_A = GE_VTYPE_TC_FLOAT | GE_VTYPE_COL_8888 | GE_VTYPE_NRM_FLOAT | GE_VTYPE_POS_FLOAT;
	auto randomA = [&](VA &v) {
		v.u = frand(-0.5f, 1.5f);
		v.v = frand(-0.5f, 1.5f);
		v.color = 0x80000000u | (Rnd() << 8) | (Rnd() & 0xFF);
		v.nx = frand(-1, 1);
		v.ny = frand(-1, 1);
		v.nz = frand(-1, 1);
		v.x = frand(-2.5f, 2.5f);
		v.y = frand(-1.5f, 1.5f);
		v.z = frand(-2.0f, 2.4f);  // the world puts some of them behind the near plane
	};
	struct Case { u32 vtype; int prim; int count; bool indexed; u32 matUpdate, lightMode, texMap, cull, gouraud, depthClip, reverse; };
	const Case cases[] = {
		{ VT_A, GE_PRIM_TRIANGLES, 300, false, 0, 0, 0, 0, 1, 1, 0 },
		{ VT_A, GE_PRIM_TRIANGLE_STRIP, 120, true, 7, 1, 0, 1, 1, 1, 0 },
		{ VT_A, GE_PRIM_TRIANGLE_FAN, 40, false, 2, 1, GE_TEXMAP_TEXTURE_MATRIX | (GE_PROJMAP_NORMAL << 8), 2, 0, 1, 1 },
		{ VT_A, GE_PRIM_TRIANGLES, 240, true, 1, 0, GE_TEXMAP_TEXTURE_MATRIX | (GE_PROJMAP_POSITION << 8), 3, 1, 0, 0 },
		{ VT_A, GE_PRIM_TRIANGLE_STRIP, 90, false, 0, 1, GE_TEXMAP_ENVIRONMENT_MAP, 0, 1, 1, 0 },
		{ VT_A, GE_PRIM_TRIANGLES, 150, false, 4, 0, GE_TEXMAP_TEXTURE_MATRIX | (GE_PROJMAP_NORMALIZED_NORMAL << 8), 1, 1, 1, 0 },
	};
	u32 at = VERTS;
	for (const Case &k : cases) {
		std::vector<VA> verts(k.indexed ? k.count / 2 + 3 : k.count);
		for (VA &v : verts)
			randomA(v);
		memcpy(GPtr(at), verts.data(), verts.size() * sizeof(VA));
		const u32 vaddr = at;
		at += (u32)(verts.size() * sizeof(VA) + 64) & ~15u;
		u32 vtype = k.vtype;
		if (k.indexed) {
			std::vector<u16> idx(k.count);
			for (u16 &i : idx)
				i = (u16)(Rnd() % verts.size());
			memcpy(GPtr(at), idx.data(), idx.size() * 2);
			dl.Addr(GE_CMD_IADDR, at);
			at += (u32)(idx.size() * 2 + 64) & ~15u;
			vtype |= GE_VTYPE_IDX_16BIT;
		}
		dl.C(GE_CMD_MATERIALUPDATE, k.matUpdate).C(GE_CMD_LIGHTMODE, k.lightMode).C(GE_CMD_TEXMAPMODE, k.texMap).C(GE_CMD_TEXSHADELS, 0 | (3 << 8));
		dl.C(GE_CMD_CULLFACEENABLE, k.cull != 0).C(GE_CMD_CULL, k.cull & 1).C(GE_CMD_SHADEMODE, k.gouraud);
		dl.C(GE_CMD_DEPTHCLIPENABLE, k.depthClip).C(GE_CMD_REVERSENORMAL, k.reverse);
		dl.C(GE_CMD_VERTEXTYPE, vtype).Prim(k.prim, k.count, vaddr);
	}

	// Skinned (two float weights), then morphed (two frames of 16-bit position and 8-bit normal), unlit
	// draws in between carrying the UV and normal over.
	dl.C(GE_CMD_TEXMAPMODE, 0).C(GE_CMD_MATERIALUPDATE, 0).C(GE_CMD_CULLFACEENABLE, 0).C(GE_CMD_SHADEMODE, 1).C(GE_CMD_DEPTHCLIPENABLE, 1);
	struct VS { float w0, w1, nx, ny, nz, x, y, z; };
	std::vector<VS> skin(60);
	for (VS &v : skin) {
		v.w0 = frand(0, 1);
		v.w1 = 1.0f - v.w0;
		v.nx = frand(-1, 1);
		v.ny = frand(-1, 1);
		v.nz = frand(-1, 1);
		v.x = frand(-2, 2);
		v.y = frand(-1.5f, 1.5f);
		v.z = frand(-1, 2);
	}
	memcpy(GPtr(at), skin.data(), skin.size() * sizeof(VS));
	dl.C(GE_CMD_VERTEXTYPE, GE_VTYPE_WEIGHT_FLOAT | (1 << 14) | GE_VTYPE_NRM_FLOAT | GE_VTYPE_POS_FLOAT).Prim(GE_PRIM_TRIANGLES, (int)skin.size(), at);
	at += (u32)(skin.size() * sizeof(VS) + 64) & ~15u;
	struct VM { s8 nx, ny, nz, pad; s16 x, y, z; u16 pad2; };
	std::vector<VM> morph(2 * 45);
	for (VM &v : morph) {
		v.nx = (s8)(Rnd() % 200 - 100);
		v.ny = (s8)(Rnd() % 200 - 100);
		v.nz = (s8)(Rnd() % 200 - 100);
		v.pad = 0;
		v.x = (s16)(Rnd() % 60000 - 30000);
		v.y = (s16)(Rnd() % 40000 - 20000);
		v.z = (s16)(Rnd() % 50000 - 20000);
		v.pad2 = 0;
	}
	memcpy(GPtr(at), morph.data(), morph.size() * sizeof(VM));
	dl.F(GE_CMD_MORPHWEIGHT0, 0.7f).F(GE_CMD_MORPHWEIGHT0 + 1, 0.4f);
	dl.C(GE_CMD_LIGHTINGENABLE, 0).C(GE_CMD_VERTEXTYPE, GE_VTYPE_NRM_8BIT | GE_VTYPE_POS_16BIT | (1 << 18)).Prim(GE_PRIM_TRIANGLES, 45, at);
	at += (u32)(morph.size() * sizeof(VM) + 64) & ~15u;

	// A batch the OpenGL renderer draws primitive by primitive (a rectangle past the framebuffer's
	// stride) between lit triangles, and triangles under a scissor past the stride, whose transform it
	// leaves to the CPU.
	auto lit = [&](int count) {
		std::vector<VA> verts(count);
		for (VA &v : verts)
			randomA(v);
		memcpy(GPtr(at), verts.data(), verts.size() * sizeof(VA));
		dl.C(GE_CMD_VERTEXTYPE, VT_A).Prim(GE_PRIM_TRIANGLES, count, at);
		at += (u32)(verts.size() * sizeof(VA) + 64) & ~15u;
	};
	dl.C(GE_CMD_LIGHTINGENABLE, 1);
	lit(60);
	TVert wide[2] = { { 0xFF4080C0, 400, 100, 0, 0 }, { 0xFF4080C0, 600, 140, 0, 0 } };
	memcpy(GPtr(at), wide, sizeof(wide));
	dl.C(GE_CMD_SCISSOR2, 1023 | (271 << 10)).C(GE_CMD_REGION2, 1023 | (271 << 10));
	dl.C(GE_CMD_TEXTUREMAPENABLE, 0).C(GE_CMD_ZTEST, GE_COMP_ALWAYS).C(GE_CMD_VERTEXTYPE, VT_C8888_P16).Prim(GE_PRIM_RECTANGLES, 2, at);
	at += 64;
	dl.C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_ZTEST, GE_COMP_GEQUAL);
	lit(60);
	dl.C(GE_CMD_SCISSOR2, 479 | (271 << 10)).C(GE_CMD_REGION2, 479 | (271 << 10));
	lit(60);
	dl.C(GE_CMD_FINISH).C(GE_CMD_END);
	EnqueueList(dl.start, 0, -1, 0, false);
	Run();
	DrawSync(0);
}

std::vector<u8> RenderLit(bool gpuTransform, TransformStats *stats) {
	Env env;
	BackendOptions o;
	o.scale = 1;
#ifdef _WIN32
	o.warp = true;
#endif
	o.gpuTransform = gpuTransform;
	o.waitForShaders = true;
	o.verifyTransform = gpuTransform;
	std::string err;
	std::unique_ptr<RenderBackend> backend = MakeBackend(o, &err);
	if (!backend)
		return {};
	SetRenderBackend(backend.get());
	DrawLitScene();
	std::vector<u8> vram(GPtr(VRAM), GPtr(VRAM) + 0x200000);
	*stats = backend->GetTransformStats();
	SetRenderBackend(nullptr);
	return vram;
}

}  // namespace

// The GPU's vertex transform and triangle assembly (gpu/geometry.h) make what the CPU's do, bit for bit.
TEST(gpu_hardware_transform_matches_cpu) {
	TransformStats cpuStats, gpuStats;
	const std::vector<u8> cpu = RenderLit(false, &cpuStats);
	const std::vector<u8> gpu = RenderLit(true, &gpuStats);
	if (cpu.empty() || gpu.empty())
		return;
	printf("  GPU transform: %llu vertices, %llu jobs\n", (unsigned long long)gpuStats.vertices, (unsigned long long)gpuStats.jobs);
	CHECK(cpuStats.jobs == 0);
	CHECK(gpuStats.jobs > 500);
	CHECK(gpuStats.checked == gpuStats.jobs);
	if (gpuStats.mismatches)
		printf("  %llu of %llu GPU transform jobs differ from the CPU\n", (unsigned long long)gpuStats.mismatches, (unsigned long long)gpuStats.jobs);
	CHECK(gpuStats.mismatches == 0);
	int diffs = 0;
	for (size_t i = 0; i < cpu.size(); ++i)
		diffs += cpu[i] != gpu[i];
	if (diffs)
		printf("  %d VRAM bytes differ\n", diffs);
	CHECK(diffs == 0);
	// The scene covers a good part of the screen.
	u32 drawn = 0;
	for (int i = 0; i < 480 * 272; ++i) {
		u32 px;
		memcpy(&px, &gpu[(size_t)(i / 480 * 512 + i % 480) * 4], 4);
		drawn += px != 0xFF102030;
	}
	CHECK(drawn > 480 * 272 / 10);
}

TEST(gpu_hardware_memory_coherence) {
	Env env;
	BackendOptions options;
	options.scale = 3;
#ifdef _WIN32
	options.warp = true;
#endif
	std::string error;
	auto backend = MakeBackend(options, &error);
	if (!backend) return;
	auto clear = [&](u32 offset, u32 color, bool depth = false) {
		RasterState state{};
		state.throughMode = true;
		state.scissor = {0, 0, 32 * SUBPIX - 1, 32 * SUBPIX - 1};
		state.pix.fb = GPtr(VRAM + offset);
		state.pix.fbOffset = offset;
		state.pix.fbStride = state.pix.zbStride = 64;
		state.pix.zbOffset = 0x180000;
		state.pix.fbFormat = GE_FORMAT_8888;
		state.pix.clearMode = state.pix.colorClear = state.pix.stencilClear = true;
		state.pix.depthClear = depth;
		state.pix.depthTestFunc = GE_COMP_ALWAYS;
		BatchPrim prim{};
		prim.type = PrimType::ClearRect;
		prim.range = state.scissor;
		prim.v[0].color0 = prim.v[1].color0 = color;
		prim.v[1].x = prim.v[1].y = 32 * SUBPIX;
		backend->DrawBatch(&prim, 1, &state, 1);
	};
	clear(0, 0xFF332211);
	backend->SyncRange(VRAM, 8192);
	CHECK(GRead32(VRAM + (5 * 64 + 5) * 4) == 0xFF332211);
	GWrite32(VRAM + (5 * 64 + 5) * 4, 0xFF776655);
	backend->Invalidate(VRAM + (5 * 64 + 5) * 4, 4);
	std::vector<u32> image;
	int w = 0, h = 0;
	CHECK(backend->GetDisplayImage(VRAM + 0x200000, 64, GE_FORMAT_8888, 32, 32, image, &w, &h, 0));
	CHECK(w == 96 && h == 96);
	if (image.size() == 96 * 96) {
		CHECK(image[16 * w + 16] == 0xFF776655);
		CHECK(image[16 * w + 20] == 0xFF332211);
	}
	// Revisit the oldest color target while creating a new depth target. Eviction
	// must never destroy a surface still in use by this draw.
	for (u32 offset : {0x20000, 0x40000, 0x60000}) clear(offset, 0xFF010203);
	clear(0, 0xFF998877, true);
	backend->Sync();
	CHECK(GRead32(VRAM + (8 * 64 + 8) * 4) == 0xFF998877);
	CHECK(backend->GetDisplayImage(VRAM, 64, GE_FORMAT_8888, 32, 32, image, &w, &h, 1));
	if (image.size() == 32 * 32) CHECK(image[8 * 32 + 8] == 0xFF998877);
	backend->ReleaseWindow();
	backend->ReleaseWindow();
}
#ifdef _WIN32
namespace {

typedef PVOID(WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
constexpr u32 VRAM_BYTES = 0x200000, WINDOW = 0x0A000000;

// Guest memory mapped as the runtime maps it (runtime/core/memory.cpp): VRAM is a view of a section, so
// the backend keeps the guest's VRAM pages inaccessible where rendering isn't written back yet and
// writes through a private view of the same memory.
struct GuardedEnv {
	u8 *mem = nullptr, *privateVram = nullptr, *saved = nullptr;
	HANDLE section = nullptr;
	bool ok = false;
	GuardedEnv() {
		HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
		auto alloc2 = kb ? (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
		auto map3 = kb ? (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
		if (!alloc2 || !map3)
			return;
		section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, VRAM_BYTES, nullptr);
		mem = (u8 *)alloc2(GetCurrentProcess(), nullptr, WINDOW, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
		if (!section || !mem)
			return;
		// The scratchpad, VRAM and RAM get placeholders of their own, then memory.
		const struct { u32 off, size; } parts[] = { { 0x10000, 0x10000 }, { VRAM, VRAM_BYTES }, { RAM, RAM_SIZE } };
		for (const auto &part : parts)
			VirtualFree(mem + part.off, part.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
		const ULONG commit = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER;
		if (!alloc2(GetCurrentProcess(), mem + 0x10000, 0x10000, commit, PAGE_READWRITE, nullptr, 0) ||
			!alloc2(GetCurrentProcess(), mem + RAM, RAM_SIZE, commit, PAGE_READWRITE, nullptr, 0) ||
			!map3(section, GetCurrentProcess(), mem + VRAM, 0, VRAM_BYTES, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
			return;
		privateVram = (u8 *)MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, VRAM_BYTES);
		if (!privateVram)
			return;
		saved = g_membase;
		g_membase = mem;
		SetRamSize(RAM_SIZE);
		SetSdkVersion(0x06060010);
		Init();
		SetRenderThreads(1);
		ok = true;
	}
	~GuardedEnv() {
		if (ok) {
			SetRenderBackend(nullptr);
			Shutdown();
			g_membase = saved;
		}
		if (privateVram)
			UnmapViewOfFile(privateVram);
		if (mem) {
			UnmapViewOfFile(mem + VRAM);
			// Every piece of the window, memory and placeholders.
			for (u8 *p = mem; p < mem + WINDOW;) {
				MEMORY_BASIC_INFORMATION mbi{};
				if (!VirtualQuery(p, &mbi, sizeof(mbi)))
					break;
				u8 *next = (u8 *)mbi.BaseAddress + mbi.RegionSize;
				if (mbi.State != MEM_FREE)
					VirtualFree(mbi.AllocationBase, 0, MEM_RELEASE);
				p = next;
			}
		}
		if (section)
			CloseHandle(section);
	}
};

void Target(DL &dl, u32 fb, int stride, int fmt) {
	dl.C(GE_CMD_FRAMEBUFPTR, fb).C(GE_CMD_FRAMEBUFWIDTH, stride).C(GE_CMD_FRAMEBUFPIXFORMAT, fmt);
}

void Texture(DL &dl, u32 addr, int fmt, int bufw, int wLog, int hLog) {
	dl.C(GE_CMD_TEXFORMAT, fmt).C(GE_CMD_TEXMODE, 0).C(GE_CMD_TEXADDR0, addr & 0xFFFFF0);
	dl.C(GE_CMD_TEXBUFWIDTH0, bufw | ((addr >> 8) & 0x0F0000)).C(GE_CMD_TEXSIZE0, wLog | (hLog << 8));
}

// A 1:1 textured sprite from (u0, v0); its two vertices at *verts.
void Sprite(DL &dl, u32 *verts, int x0, int y0, int x1, int y1, int u0, int v0) {
	const TTVert s[2] = { { (u16)u0, (u16)v0, 0xFFFFFFFF, (s16)x0, (s16)y0, 0, 0 },
	                      { (u16)(u0 + x1 - x0), (u16)(v0 + y1 - y0), 0xFFFFFFFF, (s16)x1, (s16)y1, 0, 0 } };
	memcpy(GPtr(*verts), s, sizeof(s));
	dl.Prim(GE_PRIM_RECTANGLES, 2, *verts);
	*verts += sizeof(s);
}

}  // namespace

// Rendering read back by the GE in other layouts and by the CPU, with the VRAM guard (as the runtime
// runs): Logan's Shadow's 128-wide 5551 targets one above the other in VRAM, drawn in turn with sprites
// ending on the other's first row, one sampled as 8-bit CLUT indices, a 512-row texture over both and
// the memory after them, a CPU read, more lists (one drawing and block-transferring the target after a
// GE run left a copy of it) and CPU reads, and then all of VRAM read by the CPU. The same bytes as the
// software renderer, with the textures decoded on the GPU and a write-back taking the copy made at the
// end of a GE run.
TEST(gpu_hardware_guarded_render_to_texture) {
	for (int scale : { 1, 3 }) {
		GuardedEnv env;
		if (!env.ok) {
			printf("  guarded VRAM unavailable\n");
			return;
		}
		BackendOptions options;
		options.scale = scale;
		options.warp = true;
		options.privateVram = env.privateVram;
		std::string error;
		auto backend = MakeBackend(options, &error);
		if (!backend) {
			printf("  hardware guarded test unavailable: %s\n", error.c_str());
			return;
		}
		constexpr u32 T1 = 0x100000, T2 = T1 + 128 * 128 * 2;
		auto draw = [&](RenderBackend *renderer) {
			SetRenderBackend(nullptr);
			Shutdown();
			Init();
			memset(GPtr(VRAM), 0, VRAM_BYTES);
			SetRenderBackend(renderer);
			g_seed = 77;
			for (u32 i = 0; i < 128 * 256; ++i)
				GWrite16(TEX + i * 2, (u16)Rnd());
			for (u32 i = 0; i < 256; ++i)
				GWrite32(CLUTA + i * 4, 0xFF000000 | (i * 0x010305));
			// What the 512-row texture reads past both targets.
			for (u32 i = 192 * 128; i < 512 * 128; ++i)
				GWrite16(VRAM + T1 + i * 2, (u16)(i * 37));
			DL dl;
			u32 verts = VERTS;
			dl.C(GE_CMD_SCISSOR1, 0).C(GE_CMD_SCISSOR2, 511 | (511 << 10)).C(GE_CMD_REGION2, 511 | (511 << 10));
			dl.C(GE_CMD_ZWRITEDISABLE, 1).C(GE_CMD_TEXTUREMAPENABLE, 1).C(GE_CMD_TEXFILTER, 0).C(GE_CMD_TEXWRAP, 0x101);
			dl.C(GE_CMD_TEXFUNC, GE_TEXFUNC_REPLACE | 0x100).C(GE_CMD_VERTEXTYPE, VT_T16_C8888_P16);
			Texture(dl, TEX, GE_TFMT_5551, 128, 7, 8);
			Target(dl, T1, 128, GE_FORMAT_5551);
			Sprite(dl, &verts, 0, 0, 128, 128, 0, 0);
			Target(dl, T2, 128, GE_FORMAT_5551);
			Sprite(dl, &verts, 0, 0, 128, 64, 0, 128);
			Target(dl, T1, 128, GE_FORMAT_5551);
			Sprite(dl, &verts, 32, 32, 96, 128, 0, 64);
			Target(dl, T2, 128, GE_FORMAT_5551);
			Sprite(dl, &verts, 0, 0, 64, 64, 64, 192);
			Target(dl, 0, 256, GE_FORMAT_8888);
			Texture(dl, VRAM + T1, GE_TFMT_CLUT8, 256, 8, 7);
			dl.Addr(GE_CMD_CLUTADDR, CLUTA).C(GE_CMD_CLUTADDRUPPER, (CLUTA >> 8) & 0x0F0000);
			dl.C(GE_CMD_CLUTFORMAT, GE_CMODE_32BIT_ABGR8888 | (0xFF << 8)).C(GE_CMD_LOADCLUT, 32);
			Sprite(dl, &verts, 0, 0, 256, 128, 0, 0);
			// Rows 64-127 of T1, T2's first 64 and 128 rows of memory.
			Texture(dl, VRAM + T1, GE_TFMT_5551, 128, 7, 9);
			Sprite(dl, &verts, 0, 128, 128, 384, 0, 64);
			dl.C(GE_CMD_FINISH).C(GE_CMD_END);
			EnqueueList(dl.start, 0, -1, 0, false);
			Run();
			DrawSync(0);
			const u16 read = GRead16(VRAM + T1 + (40 * 128 + 40) * 2);
			DL again;
			again.start = again.pc = LIST + 0x10000;
			Target(again, T1, 128, GE_FORMAT_5551);
			Texture(again, TEX, GE_TFMT_5551, 128, 7, 8);
			Sprite(again, &verts, 0, 0, 64, 64, 64, 0);
			again.C(GE_CMD_FINISH).C(GE_CMD_END);
			EnqueueList(again.start, 0, -1, 0, false);
			Run();
			DrawSync(0);
			// Drawn again and block-transferred in the same list: that write-back can't take the copy
			// made when the last run ended.
			DL third;
			third.start = third.pc = LIST + 0x20000;
			Sprite(third, &verts, 16, 8, 48, 40, 0, 200);
			third.C(GE_CMD_TRANSFERSRC, T1 & 0xFFFFF0).C(GE_CMD_TRANSFERSRCW, 128 | ((VRAM >> 8) & 0xFF0000));
			third.C(GE_CMD_TRANSFERDST, 0x180000).C(GE_CMD_TRANSFERDSTW, 128 | ((VRAM >> 8) & 0xFF0000));
			third.C(GE_CMD_TRANSFERSRCPOS, 0).C(GE_CMD_TRANSFERDSTPOS, 0);
			third.C(GE_CMD_TRANSFERSIZE, 127 | (63 << 10)).C(GE_CMD_TRANSFERSTART, 0);
			third.C(GE_CMD_FINISH).C(GE_CMD_END);
			EnqueueList(third.start, 0, -1, 0, false);
			Run();
			DrawSync(0);
			// Drawn once more: the CPU's next read takes the copy made when this run ended.
			DL fourth;
			fourth.start = fourth.pc = LIST + 0x30000;
			Sprite(fourth, &verts, 64, 64, 128, 96, 0, 100);
			fourth.C(GE_CMD_FINISH).C(GE_CMD_END);
			EnqueueList(fourth.start, 0, -1, 0, false);
			Run();
			DrawSync(0);
			const u16 last = GRead16(VRAM + T1 + (70 * 128 + 70) * 2);
			std::vector<u8> vram(VRAM_BYTES);
			memcpy(vram.data(), GPtr(VRAM), vram.size());
			for (u16 v : { read, last }) {
				vram.push_back((u8)v);
				vram.push_back((u8)(v >> 8));
			}
			SetRenderBackend(nullptr);
			return vram;
		};
		const std::vector<u8> reference = draw(nullptr), actual = draw(backend.get());
		size_t diffs = 0;
		for (size_t i = 0; i < reference.size() && i < actual.size(); ++i)
			diffs += reference[i] != actual[i];
		if (diffs)
			printf("  scale %d: %zu bytes of VRAM differ\n", scale, diffs);
		CHECK(reference.size() == actual.size() && diffs == 0);
		CHECK(GRead32(VRAM + (64 * 256 + 64) * 4) != 0);
		const hw::D3D11WriteBackStats stats = hw::GetD3D11WriteBackStats(backend.get());
		CHECK(stats.gpuTextures >= 2);
		CHECK(stats.earlyCopiesUsed >= 1);
	}
}

// The dialogs' overlay, presented over the picture in a window: composited on the CPU (at the window's
// size: tens of milliseconds when it is large) only when its revision or that size changes, not at every
// present.
TEST(gpu_hardware_overlay_drawn_when_it_changes) {
	HWND window = CreateWindowExW(0, L"STATIC", L"PSPC overlay test", WS_POPUP, 0, 0, 960, 544, nullptr, nullptr, nullptr,
	                              nullptr);
	CHECK(window != nullptr);
	if (!window)
		return;
	{
		Env env;
		BackendOptions o;
		o.scale = 2;
		o.warp = true;
		o.window = window;
		std::string err;
		auto backend = MakeBackend(o, &err);
		if (!backend) {
			printf("  (D3D11 WARP unavailable: %s)\n", err.c_str());
		} else {
			SetRenderBackend(backend.get());
			DrawScene(GE_FORMAT_8888);
			int calls = 0, drawnW = 0;
			auto overlay = [&](u32 *px, int w, int h) {
				calls++;
				drawnW = w;
				px[(size_t)w * h - 1] = 0xFFFFFFFF;
			};
			auto present = [&](const std::function<void(u32 *, int, int)> &o, u64 revision) {
				return backend->PresentDisplay(VRAM, 512, GE_FORMAT_8888, 480, 272, o, revision, false);
			};
			for (int i = 0; i < 3; i++)
				CHECK(present(overlay, 5));
			CHECK(calls == 1 && drawnW == 960);
			CHECK(present(overlay, 6));
			CHECK(calls == 2);
			// No overlay for a while, then the same one again: the one drawn is still right.
			CHECK(present(nullptr, 6));
			CHECK(present(overlay, 6));
			CHECK(calls == 2);
			// The window made larger (maximized): drawn again, at its new size.
			SetWindowPos(window, nullptr, 0, 0, 1920, 1088, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			CHECK(present(overlay, 6));
			CHECK(calls == 3 && drawnW == 1920);
			CHECK(present(overlay, 6));
			CHECK(calls == 3);
			backend->ReleaseWindow();
			SetRenderBackend(nullptr);
		}
	}
	DestroyWindow(window);
}
#endif
#endif
