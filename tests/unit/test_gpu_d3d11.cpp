// SPDX-License-Identifier: GPL-2.0-or-later
// The D3D11 renderer backend against the software renderer (the reference): the same display lists,
// guest VRAM compared afterwards. Uses the WARP device, so it runs without a GPU.
#if PSPC_TEST_GPU_HW
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "allegrex/guestmem.h"
#include "gpu/backend.h"
#include "gpu/ge.h"
#include "gpu/ge_constants.h"
#include "gpu/hw/d3d11_backend.h"
#include "testing.h"

using namespace gpu;

namespace {

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
		mem = (u8 *)VirtualAlloc(nullptr, 0x0A000000, MEM_RESERVE, PAGE_NOACCESS);
		VirtualAlloc(mem + VRAM, 0x200000, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(mem + RAM, RAM_SIZE, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(mem + 0x10000, 0x4000, MEM_COMMIT, PAGE_READWRITE);
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
		VirtualFree(mem, 0, MEM_RELEASE);
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
		hw::D3D11Options o;
		o.scale = scale;
		o.warp = true;
		std::string err;
		backend = hw::CreateD3D11Backend(o, &err);
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

TEST(gpu_d3d11_matches_software) {
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
	}
}
#endif
