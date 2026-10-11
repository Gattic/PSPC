// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// The Direct3D 11 renderer backend.
//
// Drawing. The binner hands over batches of screen-space primitives with their raster states. Each
// becomes GPU vertices (triangles as they are; sprites, clears and points as quads over exactly the
// pixels the software rasterizer covers, from rasterizer.cpp's edge rules; lines as lines), with
// colors, depth and fog interpolating linearly in screen space and texture coordinates as s/q, t/q
// planes, like the GE. Consecutive states that share a texture, sampler, scissor and shader variant go
// in one draw: the per-draw state (d3d11_shaders.cpp) is indexed per vertex.
//
// Surfaces. Every framebuffer and depth buffer the GE draws to (VRAM address, stride, format) is
// mirrored by an R32_UINT texture at the render scale holding raw PSP pixel values, written through
// rasterizer-ordered views by the pixel shader.
//
// Coherence:
//  * Each surface keeps a shadow copy of the guest VRAM rows it last exchanged with guest memory. Before
//    the GPU draws rows of a surface, rows whose VRAM differs from the shadow (the CPU, a block transfer
//    or another surface wrote them) are uploaded again; the others keep their (upscaled) GPU contents.
//  * Rows the GPU drew are "dirty" until written back (reduced to 1x first at a render scale above 1; a
//    pixel the CPU changed meanwhile keeps the CPU's value). They are written back when the GE reads
//    that memory itself (block transfer source, CLUT, a texture that isn't a surface), before a block
//    transfer writes it, before another surface overlapping it is validated, and
//    - with the VRAM guard (the default; it needs mem::PrivateVramView): the guest's VRAM pages under
//      dirty rows are made inaccessible, and the first guest access to one (CPU code, HLE, sceDisplay,
//      frame dumps) faults into a handler that writes the rendering back. The backend does its own
//      accesses through a private view of the same memory. Nothing is read back otherwise, so the CPU
//      never waits for the GPU at list ends;
//    - without it (PSPC_D3D11_SYNC=eager): at every list end and sceGeDrawSync/ListSync.
//  * Textures are hashed (d3d11_texcache.cpp) after their memory was made current; a texture that is
//    (part of) a surface in the same pixel format is sampled from the surface itself (render to texture).
//
// Presentation. With a host window, sceDisplay's framebuffer goes from its surface straight into the
// window's swap chain, scaled to fit by a pixel shader (averaged down when the render scale is larger
// than the window: supersampling), so even 8K rendering never comes back to the CPU. Frames that need
// the CPU (the dialogs and HOME menu drawn over the game, screenshots, framebuffers the GPU didn't draw)
// are read back at about the window's size, or come as CPU images, and are drawn the same way.
#include "gpu/hw/d3d11_backend.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "common/log.h"
#include "gpu/gemath.h"
#include "gpu/hw/d3d11_internal.h"

namespace gpu::hw {

namespace {

constexpr int FMT_DEPTH = 4;
constexpr u32 VRAM_MASK = VRAM_SIZE - 1;
constexpr int MAX_SURFACE_W = 2048;  // PSP pixels; at most MAX_TEXTURE_SIDE / scale at the render scale
constexpr int MAX_SURFACE_H = 1024;
constexpr int MAX_SCALE = 16;
static_assert(MAX_SURFACE_H * MAX_SCALE <= MAX_TEXTURE_SIDE, "surface height at the largest render scale");

struct TargetCB {
	float ndcScale[2];
	float ndcOffset[2];
	u32 scale;
	u32 pad[3];
};

struct CopyCB {
	u32 origin[2];
	u32 size[2];
	u32 scale;
	u32 format;
	u32 outScale;
	u32 pad;
};

struct PresentCB {
	float dstOrigin[2];
	float dstScale[2];
	s32 srcSize[2];
	u32 srcFormat;
	u32 magFilter;
	s32 overlaySize[2];
	u32 pad[2];
};

// A 24-bit BMP of an RGBA8888 image (PSPC_HOST_SNAPSHOT).
void WriteBmp(const char *path, const u8 *rgba, int w, int h, size_t pitch) {
	const u32 rowBytes = ((u32)w * 3 + 3) & ~3u, size = 54 + rowBytes * (u32)h;
	std::vector<u8> bmp(size, 0);
	auto put32 = [&](size_t at, u32 v) { memcpy(&bmp[at], &v, 4); };
	bmp[0] = 'B';
	bmp[1] = 'M';
	put32(2, size);
	put32(10, 54);
	put32(14, 40);
	put32(18, (u32)w);
	put32(22, (u32)h);
	bmp[26] = 1;
	bmp[28] = 24;
	put32(34, rowBytes * (u32)h);
	for (int y = 0; y < h; ++y) {
		const u8 *src = rgba + (size_t)y * pitch;
		u8 *row = &bmp[54 + (size_t)(h - 1 - y) * rowBytes];
		for (int x = 0; x < w; ++x) {
			row[x * 3 + 0] = src[x * 4 + 2];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 0];
		}
	}
	if (FILE *f = fopen(path, "wb")) {
		fwrite(bmp.data(), 1, bmp.size(), f);
		fclose(f);
	}
}

// rasterizer.cpp ClampFogDepth: the fog factor 0-255 from a vertex's fog depth.
inline float ClampFogDepth(float fogdepth) {
	const u32 u = FloatBits(fogdepth);
	const u32 exp = u >> 23;
	if ((u & 0x80000000) != 0 || exp <= 126 - 8)
		return 0.0f;
	if (exp > 126)
		return 255.0f;
	const u32 mantissa = (u & 0x007FFFFF) | 0x00800000;
	return (float)(u8)(mantissa >> (16 + 126 - exp));
}

// A linear function of screen subpixels through three points (a sprite's UV planes).
struct LinearFn {
	double a = 0, b = 0, c = 0;
	double At(double x, double y) const { return a + b * x + c * y; }
};

LinearFn PlaneThrough(const int64_t X[3], const int64_t Y[3], const double F[3]) {
	LinearFn f;
	const double det = (double)(X[1] - X[0]) * (double)(Y[2] - Y[0]) - (double)(X[2] - X[0]) * (double)(Y[1] - Y[0]);
	if (det == 0.0) {
		f.a = F[0];
		return f;
	}
	f.b = ((F[1] - F[0]) * (double)(Y[2] - Y[0]) - (F[2] - F[0]) * (double)(Y[1] - Y[0])) / det;
	f.c = ((F[2] - F[0]) * (double)(X[1] - X[0]) - (F[1] - F[0]) * (double)(X[2] - X[0])) / det;
	f.a = F[0] - f.b * (double)X[0] - f.c * (double)Y[0];
	return f;
}

// The pixels DrawRectangle (rasterizer.cpp) fills, inclusive.
bool RectBounds(const RVertex &v0, const RVertex &v1, const Range &range, int *px0, int *py0, int *px1, int *py1) {
	const int entireX1 = std::min(v0.x, v1.x);
	const int entireY1 = std::min(v0.y, v1.y);
	const int entireX2 = std::max(v0.x, v1.x) - 1;
	const int entireY2 = std::max(v0.y, v1.y) - 1;
	int minX = std::max(entireX1 & ~(SUBPIX - 1), range.x1) | (SUBPIX / 2 - 1);
	int minY = std::max(entireY1 & ~(SUBPIX - 1), range.y1) | (SUBPIX / 2 - 1);
	const int maxX = std::min(entireX2 - 1, range.x2);
	const int maxY = std::min(entireY2 - 1, range.y2);
	if (minX < entireX1 - 2)
		minX += SUBPIX;
	const bool rotated = v0.x < v1.x && v0.y > v1.y;
	if (minY < entireY1 - (rotated ? 2 : 1))
		minY += SUBPIX;
	if (minX > maxX || minY > maxY)
		return false;
	*px0 = minX / SUBPIX;
	*py0 = minY / SUBPIX;
	*px1 = *px0 + (maxX - minX) / SUBPIX;
	*py1 = *py0 + (maxY - minY) / SUBPIX;
	return true;
}

// The pixels ClearRectangle (rasterizer.cpp) fills, inclusive.
bool ClearBounds(const RVertex &v0, const RVertex &v1, const Range &range, int *px0, int *py0, int *px1, int *py1) {
	const int entireX1 = std::min(v0.x, v1.x);
	const int entireY1 = std::min(v0.y, v1.y);
	const int entireX2 = std::max(v0.x, v1.x) - 1;
	const int entireY2 = std::max(v0.y, v1.y) - 1;
	int minX = std::max(entireX1 & ~(SUBPIX - 1), range.x1) | (SUBPIX / 2 - 1);
	int minY = std::max(entireY1 & ~(SUBPIX - 1), range.y1) | (SUBPIX / 2 - 1);
	const int maxX = std::min(entireX2, range.x2);
	const int maxY = std::min(entireY2, range.y2);
	if (minX < entireX1 - 1)
		minX += SUBPIX;
	if (minY < entireY1 - 1)
		minY += SUBPIX;
	*px0 = minX / SUBPIX;
	*py0 = minY / SUBPIX;
	*px1 = (maxX - SUBPIX / 2) / SUBPIX;
	*py1 = (maxY - SUBPIX / 2) / SUBPIX;
	return *px1 >= *px0 && *py1 >= *py0;
}

bool SameTexture(const SamplerState &a, const SamplerState &b) {
	if (a.texfmt != b.texfmt || a.swizzle != b.swizzle || a.maxLevel != b.maxLevel || a.clut != b.clut ||
		a.clutfmt != b.clutfmt || a.clutShift != b.clutShift || a.clutMask != b.clutMask || a.clutOffset != b.clutOffset ||
		a.useSharedClut != b.useSharedClut)
		return false;
	for (int i = 0; i <= a.maxLevel; ++i) {
		const TexLevel &x = a.levels[i], &y = b.levels[i];
		if (x.addr != y.addr || x.bufw != y.bufw || x.w != y.w || x.h != y.h)
			return false;
	}
	return a.clampS == b.clampS && a.clampT == b.clampT && a.minFilt == b.minFilt && a.magFilt == b.magFilt && a.mipFilt == b.mipFilt;
}

// Whether two states can share a draw call.
bool Compatible(const RasterState &a, const RasterState &b) {
	if (a.pix.fbFormat != b.pix.fbFormat || a.pix.clearMode != b.pix.clearMode)
		return false;
	const bool ta = a.enableTextures && !a.pix.clearMode, tb = b.enableTextures && !b.pix.clearMode;
	if (ta != tb || memcmp(&a.scissor, &b.scissor, sizeof(Range)) != 0)
		return false;
	return !ta || SameTexture(a.samp, b.samp);
}

struct Surface {
	u32 addr = 0;  // VRAM offset
	int stride = 0;
	int fmt = 0;   // GE_FORMAT_*, FMT_DEPTH
	int bpp = 2;
	int width = 0, height = 0;  // PSP pixels
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11UnorderedAccessView> uav;
	ComPtr<ID3D11ShaderResourceView> srv;
	std::vector<u8> shadow;    // guest VRAM rows as last exchanged
	std::vector<u8> rowValid;  // the GPU copy of the row matches the shadow
	int dirtyY0 = INT_MAX, dirtyY1 = -1;  // rows drawn and not written back
	u64 validRun = 0, validGen = 0;
	int validY0 = 0, validY1 = -1;
	u64 lastUse = 0;
	bool drawn = false;
	int slot = -1;               // bit in the VRAM page masks
	int protP0 = 0, protP1 = 0;  // VRAM pages kept inaccessible for its dirty rows, [protP0, protP1)

	u32 RowBytes() const { return (u32)stride * (u32)bpp; }
	u32 Start(int y) const { return addr + (u32)y * RowBytes(); }
	bool Dirty() const { return dirtyY1 >= dirtyY0; }
};

inline bool Overlaps(u64 a0, u64 a1, u64 b0, u64 b1) {
	return a0 < b1 && b0 < a1 && a1 > a0 && b1 > b0;
}

// The backend's own view of VRAM: the private one when the guest's pages may be inaccessible.
u8 *g_vram = nullptr;

constexpr u32 PAGE_SIZE = 4096;
constexpr int NUM_PAGES = (int)(VRAM_SIZE / PAGE_SIZE);
constexpr int MAX_SURFACES = 64;

// Guest VRAM bytes at an offset, wrapping at its end.
void ReadVram(u32 off, u8 *out, u32 n) {
	const u8 *v = g_vram;
	off &= VRAM_MASK;
	const u32 first = std::min(n, VRAM_SIZE - off);
	memcpy(out, v + off, first);
	if (first < n)
		memcpy(out + first, v, n - first);
}

void WriteVram(u32 off, const u8 *in, u32 n) {
	u8 *v = g_vram;
	off &= VRAM_MASK;
	const u32 first = std::min(n, VRAM_SIZE - off);
	memcpy(v + off, in, first);
	if (first < n)
		memcpy(v, in + first, n - first);
}

float TriangleLodImpl(const GpuVertex *v, const SamplerState &ss, int scale);

struct Segment {
	u32 first, count;
	u32 state, lastState;
	bool lines;
};

class D3D11Backend final : public RenderBackend {
public:
	~D3D11Backend() override;
	bool Init(const D3D11Options &o, std::string *err);
	const char *Name() const override { return "d3d11"; }
	void BeginRun() override {
		std::lock_guard<std::recursive_mutex> lock(mu_);
		runSeq_++;
	}
	void DrawBatch(const BatchPrim *prims, size_t primCount, const RasterState *states, size_t stateCount) override;
	void SyncRange(u32 addr, u32 size) override;
	void Invalidate(u32 addr, u32 size) override;
	void Sync() override;
	// Lets the GPU start on what's queued while the CPU goes on.
	void EndRun() override {
		std::lock_guard<std::recursive_mutex> lock(mu_);
		if (pendingDraws_) {
			ctx_->Flush();
			pendingDraws_ = false;
		}
	}
	bool GetDisplayImage(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out, int *outW, int *outH,
	                     int outScale) override;
	bool PresentDisplay(u32 addr, int stride, int format, int width, int height, const std::function<void(u32 *, int, int)> &overlay,
	                    bool capture) override;
	bool PresentImage(const u32 *rgba, int width, int height, int stride) override;
	void ReleaseWindow() override;
	// A guest access to a VRAM page kept inaccessible: writes back what's rendered there.
	bool OnGuestFault(u32 off);
	bool Guarded() const { return guard_; }
	u8 *GuestVram(int mirror) const { return guestVram_[mirror]; }

private:
	ComPtr<ID3DBlob> Compile(ShaderKind kind, const char *entry, const char *target, const D3D_SHADER_MACRO *defines, std::string *err);
	ID3D11PixelShader *PixelShader(int fmt, bool clear, bool tex);
	bool MakeTexture(int w, int h, bool staging, ComPtr<ID3D11Texture2D> *tex, ComPtr<ID3D11UnorderedAccessView> *uav, ComPtr<ID3D11ShaderResourceView> *srv);
	bool EnsureScratch(ComPtr<ID3D11Texture2D> &tex, ComPtr<ID3D11UnorderedAccessView> *uav, ComPtr<ID3D11ShaderResourceView> *srv, int w, int h, int *curW, int *curH, bool staging);

	Surface *FindSurface(u32 addr, int stride, int fmt);
	Surface *GetSurface(u32 addr, int stride, int fmt, int rowsNeeded);
	bool ResizeSurface(Surface *s, int height);
	void ValidateRows(Surface *s, int y0, int y1);
	void UploadRows(Surface *s, int y0, int y1);
	void Download(Surface *s);
	void MarkDirty(Surface *s, int y0, int y1);
	void Protect(Surface *s);
	void Unprotect(Surface *s);
	void SetPagesAccess(int p0, int p1, bool accessible);
	void EvictSurface();
	void Throttle();
	void UnbindOM();
	void DrainDebugMessages();
	void RunCompute(ID3D11ComputeShader *cs, ID3D11ShaderResourceView *srv, ID3D11UnorderedAccessView *uav, const CopyCB &cb, UINT gx, UINT gy);
	static void SyncRangeThunk(void *user, u32 addr, u32 size) { static_cast<D3D11Backend *>(user)->SyncRange(addr, size); }

	void BuildVertices(const BatchPrim *prims, size_t n, const RasterState *states);
	float TriangleLod(const GpuVertex *v, const SamplerState &ss) const { return TriangleLodImpl(v, ss, scale_); }
	void EmitTriangle(const BatchPrim &p, const RasterState &st);
	void EmitRect(const BatchPrim &p, const RasterState &st, bool clear);
	void EmitPoint(const BatchPrim &p, const RasterState &st);
	void EmitLine(const BatchPrim &p, const RasterState &st);
	void FillStates(const RasterState *states, size_t n);
	void BindDraw(Surface *color, Surface *depth);
	Surface *FindTextureSurface(const SamplerState &ss, int *x0, int *y0);
	void ResolveRenderToTexture(const RasterState *states, size_t n);
	ID3D11ShaderResourceView *FeedbackCopy(Surface *s);
	void PrintStats();
	Surface *DisplaySurface(u32 addr, int stride, int format, int width);
	bool ReadDisplay(Surface *s, int width, int height, int format, int outScale, std::vector<u32> &out);
	bool EnsureSwapChain();
	void PictureRect(int *x, int *y, int *w, int *h) const;
	bool PresentImageLocked(const u32 *rgba, int width, int height, int stride);
	void DrawPresent(ID3D11ShaderResourceView *src, int srcW, int srcH, int format, bool bilinear, ID3D11ShaderResourceView *overlay = nullptr,
	                 int overlayW = 0, int overlayH = 0);
	bool UploadImage(const u32 *rgba, int width, int height, int stride, ComPtr<ID3D11Texture2D> &tex, ComPtr<ID3D11ShaderResourceView> &srv,
	                 int *texW, int *texH);

	int scale_ = 1;
	int maxSurfaceW_ = MAX_SURFACE_W;
	int texFilter_ = 0;  // D3D11Options::textureFilter
	int texUpscale_ = 1;
	bool loggedAllocFailure_ = false;
	ComPtr<ID3D11Device> dev_;
	ComPtr<ID3D11DeviceContext> ctx_;
	ComPtr<ID3D11VertexShader> vs_;
	ComPtr<ID3D11InputLayout> layout_;
	ComPtr<ID3D11PixelShader> ps_[16];
	ComPtr<ID3D11ComputeShader> csExpand_, csReduce_, csDisplay_, csUpscale_;
	ComPtr<ID3D11RasterizerState> raster_;
	ComPtr<ID3D11DepthStencilState> noDepth_;
	ComPtr<ID3D11Buffer> targetCB_, copyCB_;
	ComPtr<ID3D11Buffer> vb_;
	size_t vbBytes_ = 0;
	ComPtr<ID3D11Buffer> stateBuf_;
	ComPtr<ID3D11ShaderResourceView> stateSrv_;
	size_t stateCap_ = 0;
	ComPtr<ID3D11Texture2D> dummyTex_;
	ComPtr<ID3D11UnorderedAccessView> dummyUav_;

	// Scratch: 1x upload source, 1x reduce target, readback staging, display conversion.
	ComPtr<ID3D11Texture2D> uploadTex_, reduceTex_, stagingTex_, displayTex_, displayStaging_;
	ComPtr<ID3D11ShaderResourceView> uploadSrv_;
	ComPtr<ID3D11UnorderedAccessView> reduceUav_, displayUav_;
	int uploadW_ = 0, uploadH_ = 0, reduceW_ = 0, reduceH_ = 0, stagingW_ = 0, stagingH_ = 0;
	int displayW_ = 0, displayH_ = 0, displayStagingW_ = 0, displayStagingH_ = 0;

	std::vector<std::unique_ptr<Surface>> surfaces_;
	std::unique_ptr<TextureCache> texCache_;
	std::vector<Surface *> rttSurf_;  // per batch state: the surface its texture is (render to texture)
	ComPtr<ID3D11Texture2D> feedbackTex_;
	ComPtr<ID3D11ShaderResourceView> feedbackSrv_;
	int feedbackW_ = 0, feedbackH_ = 0;
	std::vector<GpuVertex> verts_;
	std::vector<Segment> segs_;
	std::vector<DrawStateGpu> gpuStates_;
	std::vector<u32> uploadBuf_;
	std::vector<u8> rowTmp_, rowTmp2_;

	// Presentation to the host window.
	HWND window_ = nullptr;
	bool presentFailed_ = false;
	ComPtr<IDXGISwapChain1> swapChain_;
	ComPtr<ID3D11RenderTargetView> backRtv_;
	int backW_ = 0, backH_ = 0;
	ComPtr<ID3D11VertexShader> presentVs_;
	ComPtr<ID3D11PixelShader> presentPs_;
	ComPtr<ID3D11Buffer> presentCB_;
	ComPtr<ID3D11Texture2D> imageTex_, overlayTex_;  // CPU images (PresentImage), overlays
	ComPtr<ID3D11ShaderResourceView> imageSrv_, overlaySrv_;
	int imageW_ = 0, imageH_ = 0, overlayW_ = 0, overlayH_ = 0;
	std::vector<u32> overlayBuf_;
	int presents_ = 0;

	ComPtr<ID3D11InfoQueue> infoQueue_;
	std::recursive_mutex mu_;
	// VRAM pages holding rendering not yet written back: which surfaces (bits by slot) hold them.
	bool guard_ = false;
	u8 *guestVram_[4] = {};
	u64 pageMask_[NUM_PAGES] = {};
	u64 usedSlots_ = 0;
	u64 faults_ = 0;
	// Frames in flight (event queries at list ends), so the CPU doesn't run far ahead of the GPU.
	ComPtr<ID3D11Query> frameQ_[3];
	int frameQi_ = 0;
	bool frameQIssued_[3] = {};

	u64 runSeq_ = 1;   // GE runs (the CPU may write memory between them)
	u64 vramGen_ = 1;  // the GE wrote VRAM (written back rendering, block transfers)
	u64 syncs_ = 0;
	bool omDirty_ = true;
	bool pendingDraws_ = false;

	struct Stats {
		u64 batches = 0, draws = 0, prims = 0, verts = 0;
		u64 downloads = 0, downloadRows = 0, uploads = 0, uploadRows = 0;
		double seconds = 0;
		double tDownloadWait = 0, tDownloadCopy = 0, tValidate = 0, tTexture = 0, tBuild = 0, tSubmit = 0, tDisplay = 0;
		u64 rttDraws = 0;
	} stats_;
	bool printStats_ = false;
	std::chrono::steady_clock::time_point statsStart_ = std::chrono::steady_clock::now();
};

struct ScopedTimer {
	double *acc;
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	explicit ScopedTimer(double *a) : acc(a) {}
	~ScopedTimer() { *acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

// The backend whose VRAM pages the fault handler serves.
D3D11Backend *g_guarded = nullptr;
PVOID g_guardVeh = nullptr;

LONG CALLBACK VramFaultHandler(EXCEPTION_POINTERS *ep) {
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_guarded)
		return EXCEPTION_CONTINUE_SEARCH;
	const uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
	for (int m = 0; m < 4; ++m) {
		const uintptr_t base = (uintptr_t)g_guarded->GuestVram(m);
		if (base && addr >= base && addr < base + VRAM_SIZE)
			return g_guarded->OnGuestFault((u32)(addr - base)) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

D3D11Backend::~D3D11Backend() {
	if (guard_) {
		std::lock_guard<std::recursive_mutex> lock(mu_);
		for (auto &sp : surfaces_)
			Download(sp.get());
		if (g_guarded == this) {
			RemoveVectoredExceptionHandler(g_guardVeh);
			g_guarded = nullptr;
			g_guardVeh = nullptr;
		}
	}
	if (ctx_) {
		ctx_->ClearState();
		ctx_->Flush();
	}
}

void D3D11Backend::SetPagesAccess(int p0, int p1, bool accessible) {
	if (p0 >= p1)
		return;
	DWORD old;
	for (int m = 0; m < 4; ++m) {
		if (guestVram_[m])
			VirtualProtect(guestVram_[m] + (size_t)p0 * PAGE_SIZE, (size_t)(p1 - p0) * PAGE_SIZE, accessible ? PAGE_READWRITE : PAGE_NOACCESS, &old);
	}
}

// The guest's VRAM pages under the surface's dirty rows become inaccessible.
void D3D11Backend::Protect(Surface *s) {
	if (!guard_ || !s->Dirty() || s->slot < 0)
		return;
	const u64 start = s->Start(s->dirtyY0), end = std::min<u64>(s->Start(s->dirtyY1 + 1), VRAM_SIZE);
	if (start >= end)
		return;
	int p0 = (int)(start / PAGE_SIZE), p1 = (int)((end + PAGE_SIZE - 1) / PAGE_SIZE);
	if (s->protP1 > s->protP0) {
		p0 = std::min(p0, s->protP0);
		p1 = std::max(p1, s->protP1);
	}
	const u64 bit = 1ull << s->slot;
	int runStart = -1;
	for (int pg = p0; pg <= p1; ++pg) {
		const bool newly = pg < p1 && pageMask_[pg] == 0;
		if (pg < p1)
			pageMask_[pg] |= bit;
		if (newly && runStart < 0) {
			runStart = pg;
		} else if (!newly && runStart >= 0) {
			SetPagesAccess(runStart, pg, false);
			runStart = -1;
		}
	}
	s->protP0 = p0;
	s->protP1 = p1;
}

void D3D11Backend::Unprotect(Surface *s) {
	if (s->protP1 <= s->protP0)
		return;
	const u64 bit = 1ull << s->slot;
	int runStart = -1;
	for (int pg = s->protP0; pg <= s->protP1; ++pg) {
		bool freed = false;
		if (pg < s->protP1 && (pageMask_[pg] & bit)) {
			pageMask_[pg] &= ~bit;
			freed = pageMask_[pg] == 0;
		}
		if (freed && runStart < 0) {
			runStart = pg;
		} else if (!freed && runStart >= 0) {
			SetPagesAccess(runStart, pg, true);
			runStart = -1;
		}
	}
	s->protP0 = s->protP1 = 0;
}

bool D3D11Backend::OnGuestFault(u32 off) {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	const int pg = (int)(off / PAGE_SIZE);
	if (pg < 0 || pg >= NUM_PAGES)
		return false;
	const u64 mask = pageMask_[pg];
	if (mask == 0) {
		// Not ours (any more): if the page is still inaccessible, something went wrong; let it be accessed.
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(guestVram_[0] + (size_t)pg * PAGE_SIZE, &mbi, sizeof(mbi)) && mbi.Protect == PAGE_NOACCESS) {
			SetPagesAccess(pg, pg + 1, true);
			return true;
		}
		return false;
	}
	faults_++;
	for (auto &sp : surfaces_) {
		if (sp->slot >= 0 && (mask & (1ull << sp->slot)))
			Download(sp.get());
	}
	return true;
}

// At most two frames (list ends) in flight on the GPU.
void D3D11Backend::Throttle() {
	if (!frameQ_[0]) {
		D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
		for (auto &q : frameQ_)
			dev_->CreateQuery(&qd, &q);
	}
	ctx_->End(frameQ_[frameQi_].Get());
	frameQIssued_[frameQi_] = true;
	frameQi_ = (frameQi_ + 1) % 3;
	if (frameQIssued_[frameQi_]) {
		BOOL done = FALSE;
		ScopedTimer tw(&stats_.tDownloadWait);
		while (ctx_->GetData(frameQ_[frameQi_].Get(), &done, sizeof(done), 0) == S_FALSE)
			std::this_thread::yield();
		frameQIssued_[frameQi_] = false;
	}
}

ComPtr<ID3DBlob> D3D11Backend::Compile(ShaderKind kind, const char *entry, const char *target, const D3D_SHADER_MACRO *defines, std::string *err) {
	const std::string src = ShaderSource(kind);
	ComPtr<ID3DBlob> code, errors;
	const HRESULT hr = D3DCompile(src.data(), src.size(), entry, defines, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr)) {
		std::string msg = errors ? std::string((const char *)errors->GetBufferPointer(), errors->GetBufferSize()) : "D3DCompile failed";
		if (err)
			*err = msg;
		LOGE("d3d11", "%s (%s): %s", entry, target, msg.c_str());
		return nullptr;
	}
	return code;
}

bool D3D11Backend::Init(const D3D11Options &o, std::string *err) {
	scale_ = std::clamp(o.scale, 1, MAX_SCALE);
	maxSurfaceW_ = std::min(MAX_SURFACE_W, MAX_TEXTURE_SIDE / scale_);
	texFilter_ = std::clamp(o.textureFilter, 0, 2);
	texUpscale_ = std::clamp(o.textureUpscale, 1, 4);
	window_ = (HWND)o.window;
	printStats_ = getenv("PSPC_GPU_STATS") != nullptr;
	g_vram = o.privateVram ? o.privateVram : VramPtr();
	const char *syncMode = getenv("PSPC_D3D11_SYNC");
	guard_ = o.privateVram && !(syncMode && !strcmp(syncMode, "eager")) && !g_guarded;
	if (guard_) {
		for (int m = 0; m < 4; ++m)
			guestVram_[m] = GuestPtr(VRAM_BASE + (u32)m * VRAM_SIZE);
	}
	UINT flags = 0;
	if (getenv("PSPC_D3D11_DEBUG"))
		flags |= D3D11_CREATE_DEVICE_DEBUG;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	ComPtr<IDXGIAdapter1> adapter;
	if (!o.warp) {
		ComPtr<IDXGIFactory6> factory;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			factory->EnumAdapterByGpuPreference((UINT)std::max(o.adapter, 0), DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
	}
	D3D_FEATURE_LEVEL fl{};
	auto create = [&](UINT f) {
		const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : (o.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
		return D3D11CreateDevice(adapter.Get(), type, nullptr, f, levels, 2, D3D11_SDK_VERSION, &dev_, &fl, &ctx_);
	};
	HRESULT hr = create(flags);
	if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))
		hr = create(0);
	if (FAILED(hr)) {
		*err = "D3D11CreateDevice failed";
		return false;
	}
	if (flags & D3D11_CREATE_DEVICE_DEBUG)
		dev_.As(&infoQueue_);
	D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
	if (FAILED(dev_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))) || !o2.ROVsSupported) {
		*err = "the device has no rasterizer ordered views (D3D11.3 ROVs)";
		return false;
	}
	std::string name = o.warp ? "WARP" : "?";
	{
		ComPtr<IDXGIDevice> dxgiDev;
		ComPtr<IDXGIAdapter> ad;
		DXGI_ADAPTER_DESC desc{};
		if (SUCCEEDED(dev_.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc))) {
			char buf[128];
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
			name = buf;
		}
	}

	ComPtr<ID3DBlob> vsCode = Compile(ShaderKind::Vertex, "VSMain", "vs_5_0", nullptr, err);
	if (!vsCode || FAILED(dev_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)))
		return false;
	const D3D11_INPUT_ELEMENT_DESC elems[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 1, DXGI_FORMAT_R8G8B8A8_UINT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	if (FAILED(dev_->CreateInputLayout(elems, 7, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout_))) {
		*err = "CreateInputLayout failed";
		return false;
	}
	struct { ShaderKind kind; const char *entry; ComPtr<ID3D11ComputeShader> *cs; } computes[] = {
		{ ShaderKind::Compute, "CSExpand", &csExpand_ }, { ShaderKind::Compute, "CSReduce", &csReduce_ },
		{ ShaderKind::Compute, "CSDisplay", &csDisplay_ }, { ShaderKind::Upscale, "CSUpscale", &csUpscale_ },
	};
	for (auto &c : computes) {
		ComPtr<ID3DBlob> code = Compile(c.kind, c.entry, "cs_5_0", nullptr, err);
		if (!code || FAILED(dev_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, c.cs->GetAddressOf())))
			return false;
	}
	{
		ComPtr<ID3DBlob> pvs = Compile(ShaderKind::Present, "VSPresent", "vs_5_0", nullptr, err);
		ComPtr<ID3DBlob> pps = Compile(ShaderKind::Present, "PSPresent", "ps_5_0", nullptr, err);
		if (!pvs || !pps || FAILED(dev_->CreateVertexShader(pvs->GetBufferPointer(), pvs->GetBufferSize(), nullptr, &presentVs_)) ||
			FAILED(dev_->CreatePixelShader(pps->GetBufferPointer(), pps->GetBufferSize(), nullptr, &presentPs_)))
			return false;
	}
	// Every pixel shader variant, compiled in parallel (so a compile error shows at startup).
	{
		const auto t0 = std::chrono::steady_clock::now();
		struct Variant { int fmt; bool clear, tex; ComPtr<ID3DBlob> code; };
		std::vector<Variant> variants;
		for (int fmt = 0; fmt < 4; ++fmt) {
			variants.push_back({ fmt, false, false, nullptr });
			variants.push_back({ fmt, false, true, nullptr });
			variants.push_back({ fmt, true, false, nullptr });
		}
		std::vector<std::thread> threads;
		for (Variant &v : variants) {
			threads.emplace_back([this, &v] {
				const char fmtStr[2] = { (char)('0' + v.fmt), 0 };
				const D3D_SHADER_MACRO defines[] = { { "FMT", fmtStr }, { "CLEAR", v.clear ? "1" : "0" }, { "TEX", v.tex ? "1" : "0" }, { nullptr, nullptr } };
				v.code = Compile(ShaderKind::Pixel, "PSMain", "ps_5_0", defines, nullptr);
			});
		}
		for (std::thread &t : threads)
			t.join();
		for (Variant &v : variants) {
			const int idx = v.fmt * 4 + (v.clear ? 2 : 0) + (v.tex ? 1 : 0);
			if (!v.code || FAILED(dev_->CreatePixelShader(v.code->GetBufferPointer(), v.code->GetBufferSize(), nullptr, &ps_[idx]))) {
				*err = "pixel shader compile failed";
				return false;
			}
		}
		LOGI("d3d11", "shaders compiled in %.0f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	}

	D3D11_RASTERIZER_DESC rd{};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = FALSE;
	rd.ScissorEnable = TRUE;
	D3D11_DEPTH_STENCIL_DESC dd{};
	dd.DepthEnable = FALSE;
	dd.StencilEnable = FALSE;
	D3D11_BUFFER_DESC cbd{};
	cbd.ByteWidth = 32;
	cbd.Usage = D3D11_USAGE_DEFAULT;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	D3D11_BUFFER_DESC presentCbd = cbd;
	presentCbd.ByteWidth = sizeof(PresentCB);
	if (FAILED(dev_->CreateRasterizerState(&rd, &raster_)) || FAILED(dev_->CreateDepthStencilState(&dd, &noDepth_)) ||
		FAILED(dev_->CreateBuffer(&cbd, nullptr, &targetCB_)) || FAILED(dev_->CreateBuffer(&cbd, nullptr, &copyCB_)) ||
		FAILED(dev_->CreateBuffer(&presentCbd, nullptr, &presentCB_)) ||
		!MakeTexture(1, 1, false, std::addressof(dummyTex_), std::addressof(dummyUav_), nullptr)) {
		*err = "D3D11 state creation failed";
		return false;
	}
	texCache_ = std::make_unique<TextureCache>(dev_.Get(), ctx_.Get());
	if (texUpscale_ > 1 && !texCache_->SetUpscale(texUpscale_, csUpscale_.Get()))
		texUpscale_ = 1;
	if (guard_) {
		g_guarded = this;
		g_guardVeh = AddVectoredExceptionHandler(1, VramFaultHandler);
	}
	static const char *const kFilters[] = { "as the game sets it", "nearest", "linear" };
	LOGI("d3d11", "D3D11 renderer on %s (feature level %x), render scale %d (%d x %d), textures %s%s, %s", name.c_str(), (unsigned)fl,
		scale_, 480 * scale_, 272 * scale_, kFilters[texFilter_], texUpscale_ > 1 ? (", upscaled " + std::to_string(texUpscale_) + "x").c_str() : "",
		guard_ ? "rendering written back when guest memory is touched" : "rendering written back at list ends and syncs");
	return true;
}

ID3D11PixelShader *D3D11Backend::PixelShader(int fmt, bool clear, bool tex) {
	if (clear)
		tex = false;
	const int idx = (fmt & 3) * 4 + (clear ? 2 : 0) + (tex ? 1 : 0);
	if (!ps_[idx]) {
		const char fmtStr[2] = { (char)('0' + (fmt & 3)), 0 };
		const D3D_SHADER_MACRO defines[] = { { "FMT", fmtStr }, { "CLEAR", clear ? "1" : "0" }, { "TEX", tex ? "1" : "0" }, { nullptr, nullptr } };
		ComPtr<ID3DBlob> code = Compile(ShaderKind::Pixel, "PSMain", "ps_5_0", defines, nullptr);
		if (!code || FAILED(dev_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps_[idx])))
			return nullptr;
	}
	return ps_[idx].Get();
}

// (Callers pass std::addressof(member): WRL's ComPtr::operator& releases the pointer it's applied to.)
bool D3D11Backend::MakeTexture(int w, int h, bool staging, ComPtr<ID3D11Texture2D> *tex, ComPtr<ID3D11UnorderedAccessView> *uav, ComPtr<ID3D11ShaderResourceView> *srv) {
	D3D11_TEXTURE2D_DESC td{};
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32_UINT;
	td.SampleDesc.Count = 1;
	if (staging) {
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	} else {
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	}
	tex->Reset();
	if (FAILED(dev_->CreateTexture2D(&td, nullptr, tex->GetAddressOf())))
		return false;
	if (uav) {
		uav->Reset();
		if (FAILED(dev_->CreateUnorderedAccessView(tex->Get(), nullptr, uav->GetAddressOf())))
			return false;
	}
	if (srv) {
		srv->Reset();
		if (FAILED(dev_->CreateShaderResourceView(tex->Get(), nullptr, srv->GetAddressOf())))
			return false;
	}
	return true;
}

bool D3D11Backend::EnsureScratch(ComPtr<ID3D11Texture2D> &tex, ComPtr<ID3D11UnorderedAccessView> *uav, ComPtr<ID3D11ShaderResourceView> *srv, int w, int h, int *curW, int *curH, bool staging) {
	if (tex && *curW >= w && *curH >= h)
		return true;
	const int nw = std::max(w, *curW), nh = std::max(h, *curH);
	if (!MakeTexture(nw, nh, staging, std::addressof(tex), uav, srv))
		return false;
	*curW = nw;
	*curH = nh;
	return true;
}

// --- Surfaces ---

Surface *D3D11Backend::FindSurface(u32 addr, int stride, int fmt) {
	for (auto &s : surfaces_) {
		if (s->addr == addr && s->stride == stride && s->fmt == fmt)
			return s.get();
	}
	return nullptr;
}

bool D3D11Backend::ResizeSurface(Surface *s, int height) {
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11UnorderedAccessView> uav;
	ComPtr<ID3D11ShaderResourceView> srv;
	if (!MakeTexture(s->width * scale_, height * scale_, false, std::addressof(tex), std::addressof(uav), std::addressof(srv))) {
		if (!loggedAllocFailure_) {
			loggedAllocFailure_ = true;
			LOGE("d3d11", "a %d x %d surface could not be created (out of video memory at render scale %d?): drawing to it is skipped",
				s->width * scale_, height * scale_, scale_);
		}
		return false;
	}
	if (s->tex) {
		UnbindOM();
		D3D11_BOX box{ 0, 0, 0, (UINT)(s->width * scale_), (UINT)(s->height * scale_), 1 };
		ctx_->CopySubresourceRegion(tex.Get(), 0, 0, 0, 0, s->tex.Get(), 0, &box);
	}
	s->tex = tex;
	s->uav = uav;
	s->srv = srv;
	s->height = height;
	s->shadow.resize((size_t)height * s->RowBytes());
	s->rowValid.resize((size_t)height, 0);
	omDirty_ = true;
	return true;
}

Surface *D3D11Backend::GetSurface(u32 addr, int stride, int fmt, int rowsNeeded) {
	if (stride <= 0)
		return nullptr;
	rowsNeeded = std::clamp(rowsNeeded, 1, MAX_SURFACE_H);
	Surface *s = FindSurface(addr, stride, fmt);
	if (!s) {
		if ((int)surfaces_.size() >= MAX_SURFACES)
			EvictSurface();
		surfaces_.push_back(std::make_unique<Surface>());
		s = surfaces_.back().get();
		for (int i = 0; i < MAX_SURFACES; ++i) {
			if (!(usedSlots_ & (1ull << i))) {
				s->slot = i;
				usedSlots_ |= 1ull << i;
				break;
			}
		}
		s->addr = addr;
		s->stride = stride;
		s->fmt = fmt;
		s->bpp = fmt == GE_FORMAT_8888 ? 4 : 2;
		s->width = std::min(stride, maxSurfaceW_);
	}
	if (s->height < rowsNeeded) {
		const int h = std::min(MAX_SURFACE_H, std::max((rowsNeeded + 15) & ~15, s->height == 0 ? 272 : 0));
		if (!ResizeSurface(s, std::max(h, rowsNeeded)))
			return nullptr;
	}
	s->lastUse = syncs_;
	return s;
}

// The least recently used surface goes (written back first).
void D3D11Backend::EvictSurface() {
	size_t victim = 0;
	for (size_t i = 1; i < surfaces_.size(); ++i) {
		if (surfaces_[i]->lastUse < surfaces_[victim]->lastUse)
			victim = i;
	}
	Surface *s = surfaces_[victim].get();
	Download(s);
	Unprotect(s);
	if (s->slot >= 0)
		usedSlots_ &= ~(1ull << s->slot);
	surfaces_.erase(surfaces_.begin() + (std::ptrdiff_t)victim);
}

// Surfaces may be bound as the target (OM UAVs) or the render-to-texture source (PS SRV): unbind them
// before compute work or copies use them.
void D3D11Backend::UnbindOM() {
	ID3D11UnorderedAccessView *nullUavs[2] = {};
	ctx_->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 2, nullUavs, nullptr);
	ID3D11ShaderResourceView *nullSrvs[3] = {};
	ctx_->PSSetShaderResources(0, 3, nullSrvs);
	omDirty_ = true;
}

void D3D11Backend::RunCompute(ID3D11ComputeShader *cs, ID3D11ShaderResourceView *srv, ID3D11UnorderedAccessView *uav, const CopyCB &cb, UINT gx, UINT gy) {
	UnbindOM();
	ctx_->UpdateSubresource(copyCB_.Get(), 0, nullptr, &cb, 0, 0);
	ctx_->CSSetShader(cs, nullptr, 0);
	ctx_->CSSetConstantBuffers(0, 1, copyCB_.GetAddressOf());
	ctx_->CSSetShaderResources(0, 1, &srv);
	ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	ctx_->Dispatch(gx, gy, 1);
	ID3D11ShaderResourceView *nullSrv = nullptr;
	ID3D11UnorderedAccessView *nullUav = nullptr;
	ctx_->CSSetShaderResources(0, 1, &nullSrv);
	ctx_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	DrainDebugMessages();
}

// PSPC_D3D11_DEBUG: the debug layer's messages go to the log.
void D3D11Backend::DrainDebugMessages() {
	if (!infoQueue_)
		return;
	const UINT64 n = infoQueue_->GetNumStoredMessages();
	for (UINT64 i = 0; i < n; ++i) {
		SIZE_T len = 0;
		infoQueue_->GetMessage(i, nullptr, &len);
		std::vector<u8> buf(len);
		D3D11_MESSAGE *msg = (D3D11_MESSAGE *)buf.data();
		if (SUCCEEDED(infoQueue_->GetMessage(i, msg, &len)))
			LOGW("d3d11", "debug layer: %.*s", (int)msg->DescriptionByteLength, msg->pDescription);
	}
	infoQueue_->ClearStoredMessages();
}

void D3D11Backend::UploadRows(Surface *s, int y0, int y1) {
	const int rows = y1 - y0 + 1, w = s->width;
	const u32 rb = s->RowBytes();
	uploadBuf_.resize((size_t)w * rows);
	rowTmp_.resize(rb);
	for (int r = 0; r < rows; ++r) {
		const int y = y0 + r;
		ReadVram(s->Start(y), rowTmp_.data(), rb);
		u32 *dst = &uploadBuf_[(size_t)r * w];
		if (s->bpp == 4) {
			memcpy(dst, rowTmp_.data(), (size_t)w * 4);
		} else {
			const u16 *src = (const u16 *)rowTmp_.data();
			for (int x = 0; x < w; ++x)
				dst[x] = src[x];
		}
		memcpy(&s->shadow[(size_t)y * rb], rowTmp_.data(), rb);
		s->rowValid[y] = 1;
	}
	stats_.uploads++;
	stats_.uploadRows += (u64)rows;
	if (scale_ == 1) {
		const D3D11_BOX box{ 0, (UINT)y0, 0, (UINT)w, (UINT)(y1 + 1), 1 };
		ctx_->UpdateSubresource(s->tex.Get(), 0, &box, uploadBuf_.data(), (UINT)w * 4, 0);
		return;
	}
	if (!EnsureScratch(uploadTex_, nullptr, std::addressof(uploadSrv_), w, rows, &uploadW_, &uploadH_, false))
		return;
	const D3D11_BOX box{ 0, 0, 0, (UINT)w, (UINT)rows, 1 };
	ctx_->UpdateSubresource(uploadTex_.Get(), 0, &box, uploadBuf_.data(), (UINT)w * 4, 0);
	CopyCB cb{};
	cb.origin[1] = (u32)y0;
	cb.size[0] = (u32)w;
	cb.size[1] = (u32)rows;
	cb.scale = (u32)scale_;
	RunCompute(csExpand_.Get(), uploadSrv_.Get(), s->uav.Get(), cb, (UINT)(w * scale_ + 7) / 8, (UINT)(rows * scale_ + 7) / 8);
}

void D3D11Backend::Download(Surface *s) {
	if (!s->Dirty())
		return;
	const int y0 = std::max(s->dirtyY0, 0), y1 = std::min(s->dirtyY1, s->height - 1);
	s->dirtyY0 = INT_MAX;
	s->dirtyY1 = -1;
	if (y0 > y1)
		return;
	const int rows = y1 - y0 + 1, w = s->width;
	ID3D11Texture2D *src = s->tex.Get();
	UINT srcY = (UINT)y0;
	UnbindOM();
	if (scale_ > 1) {
		if (!EnsureScratch(reduceTex_, std::addressof(reduceUav_), nullptr, w, rows, &reduceW_, &reduceH_, false))
			return;
		CopyCB cb{};
		cb.origin[1] = (u32)y0;
		cb.size[0] = (u32)w;
		cb.size[1] = (u32)rows;
		cb.scale = (u32)scale_;
		cb.format = (u32)s->fmt;
		RunCompute(csReduce_.Get(), s->srv.Get(), reduceUav_.Get(), cb, (UINT)(w + 7) / 8, (UINT)(rows + 7) / 8);
		src = reduceTex_.Get();
		srcY = 0;
	}
	if (!EnsureScratch(stagingTex_, nullptr, nullptr, w, rows, &stagingW_, &stagingH_, true))
		return;
	const D3D11_BOX box{ 0, srcY, 0, (UINT)w, srcY + (UINT)rows, 1 };
	ctx_->CopySubresourceRegion(stagingTex_.Get(), 0, 0, 0, 0, src, 0, &box);
	D3D11_MAPPED_SUBRESOURCE m{};
	{
		ScopedTimer tw(&stats_.tDownloadWait);
		if (FAILED(ctx_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
			LOGW("d3d11", "readback failed");
			return;
		}
	}
	ScopedTimer tc(&stats_.tDownloadCopy);
	const u32 rb = s->RowBytes(), bpp = (u32)s->bpp;
	const u32 gpuBytes = std::min(rb, (u32)w * bpp);
	rowTmp_.resize(rb);
	rowTmp2_.resize(rb);
	for (int r = 0; r < rows; ++r) {
		const int y = y0 + r;
		const u32 *g = (const u32 *)((const u8 *)m.pData + (size_t)r * m.RowPitch);
		u8 *gpu = rowTmp_.data();
		u8 *vram = rowTmp2_.data();
		u8 *shadow = &s->shadow[(size_t)y * rb];
		ReadVram(s->Start(y), vram, rb);
		// Bytes past the surface's width (a stride above 2048) stay as they are.
		memcpy(gpu, vram, rb);
		if (bpp == 4) {
			memcpy(gpu, g, (size_t)w * 4);
		} else {
			u16 *d = (u16 *)gpu;
			for (int x = 0; x < w; ++x)
				d[x] = (u16)g[x];
		}
		if (!s->rowValid[y] || memcmp(vram, shadow, rb) == 0) {
			WriteVram(s->Start(y), gpu, rb);
			memcpy(shadow, gpu, rb);
			s->rowValid[y] = 1;
			continue;
		}
		// The CPU changed some pixels of the row since: they keep the CPU's value, the GPU's copy of the
		// row is stale and is uploaded again before the next draw.
		for (u32 x = 0; x < gpuBytes; x += bpp) {
			if (memcmp(vram + x, shadow + x, bpp) == 0)
				memcpy(vram + x, gpu + x, bpp);
		}
		WriteVram(s->Start(y), vram, rb);
		memcpy(shadow, vram, rb);
		s->rowValid[y] = 0;
	}
	ctx_->Unmap(stagingTex_.Get(), 0);
	Unprotect(s);
	stats_.downloads++;
	stats_.downloadRows += (u64)rows;
	vramGen_++;
	texCache_->Invalidate(VRAM_BASE + s->Start(y0), VRAM_BASE + s->Start(y1 + 1));
}

void D3D11Backend::ValidateRows(Surface *s, int y0, int y1) {
	ScopedTimer tv(&stats_.tValidate);
	y0 = std::max(y0, 0);
	y1 = std::min(y1, s->height - 1);
	if (y0 > y1)
		return;
	if (s->validRun == runSeq_ && s->validGen == vramGen_ && y0 >= s->validY0 && y1 <= s->validY1)
		return;
	// What other surfaces rendered into this memory reaches VRAM first.
	const u32 start = s->Start(y0), end = s->Start(y1 + 1);
	for (auto &o : surfaces_) {
		if (o.get() != s && o->Dirty() && Overlaps(start, end, o->Start(o->dirtyY0), o->Start(o->dirtyY1 + 1)))
			Download(o.get());
	}
	const u32 rb = s->RowBytes();
	rowTmp_.resize(rb);
	// Rows the GPU drew and the CPU changed too: merge them (Download keeps the CPU's pixels). With the
	// guard the CPU can't have touched them.
	if (s->Dirty() && !guard_) {
		const int a = std::max(y0, s->dirtyY0), b = std::min(y1, s->dirtyY1);
		for (int y = a; y <= b; ++y) {
			ReadVram(s->Start(y), rowTmp_.data(), rb);
			if (s->rowValid[y] && memcmp(rowTmp_.data(), &s->shadow[(size_t)y * rb], rb) != 0) {
				Download(s);
				break;
			}
		}
	}
	// Rows whose memory changed since the GPU copy was made are uploaded again.
	int runStart = -1;
	for (int y = y0; y <= y1 + 1; ++y) {
		bool changed = false;
		if (y <= y1) {
			if (!s->rowValid[y]) {
				changed = true;
			} else if (guard_ && y >= s->dirtyY0 && y <= s->dirtyY1) {
				// Drawn and kept inaccessible to the guest since: unchanged.
			} else {
				const u32 off = s->Start(y) & VRAM_MASK;
				if (off + rb <= VRAM_SIZE) {
					changed = memcmp(g_vram + off, &s->shadow[(size_t)y * rb], rb) != 0;
				} else {
					ReadVram(off, rowTmp_.data(), rb);
					changed = memcmp(rowTmp_.data(), &s->shadow[(size_t)y * rb], rb) != 0;
				}
			}
		}
		if (changed && runStart < 0) {
			runStart = y;
		} else if (!changed && runStart >= 0) {
			UploadRows(s, runStart, y - 1);
			runStart = -1;
		}
	}
	if (s->validRun == runSeq_ && s->validGen == vramGen_) {
		s->validY0 = std::min(s->validY0, y0);
		s->validY1 = std::max(s->validY1, y1);
	} else {
		s->validRun = runSeq_;
		s->validGen = vramGen_;
		s->validY0 = y0;
		s->validY1 = y1;
	}
}

void D3D11Backend::MarkDirty(Surface *s, int y0, int y1) {
	y1 = std::min(y1, s->height - 1);
	if (y0 > y1)
		return;
	s->dirtyY0 = std::min(s->dirtyY0, y0);
	s->dirtyY1 = std::max(s->dirtyY1, y1);
	s->drawn = true;
}

void D3D11Backend::SyncRange(u32 addr, u32 size) {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	const u32 a = FoldAddress(addr);
	if (a < VRAM_BASE || a >= VRAM_BASE + VRAM_SIZE || size == 0)
		return;
	const u64 off = a - VRAM_BASE, end = off + size;
	for (auto &s : surfaces_) {
		if (!s->Dirty())
			continue;
		// Whole pages when they are kept inaccessible.
		const bool paged = guard_ && s->protP1 > s->protP0;
		const bool hit = paged ? Overlaps(off, end, (u64)s->protP0 * PAGE_SIZE, (u64)s->protP1 * PAGE_SIZE)
		                       : Overlaps(off, end, s->Start(s->dirtyY0), s->Start(s->dirtyY1 + 1));
		if (hit)
			Download(s.get());
	}
}

void D3D11Backend::Invalidate(u32 addr, u32 size) {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	const u32 a = FoldAddress(addr);
	if (size == 0)
		return;
	if (a >= VRAM_BASE && a < VRAM_BASE + VRAM_SIZE)
		vramGen_++;
	texCache_->Invalidate(a, a + size);
}

void D3D11Backend::Sync() {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	ScopedTimer t(&stats_.seconds);
	// With the guard, rendering reaches guest memory when it's touched; otherwise now.
	if (guard_) {
		Throttle();
	} else {
		for (auto &s : surfaces_)
			Download(s.get());
	}
	syncs_++;
	texCache_->EndFrame(syncs_);
	if (printStats_ && syncs_ % 300 == 0)
		PrintStats();
}

void D3D11Backend::PrintStats() {
	const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - statsStart_).count();
	LOGI("d3d11", "%.1fs: %llu batches, %llu draws, %llu prims, %llu downloads (%llu rows), %llu uploads (%llu rows), %zu textures "
		"(%llu decodes, %llu hashes), %zu surfaces, %.3fs in the backend",
		wall, (unsigned long long)stats_.batches, (unsigned long long)stats_.draws, (unsigned long long)stats_.prims,
		(unsigned long long)stats_.downloads, (unsigned long long)stats_.downloadRows, (unsigned long long)stats_.uploads,
		(unsigned long long)stats_.uploadRows, texCache_->Size(), (unsigned long long)texCache_->decodes,
		(unsigned long long)texCache_->hashes, surfaces_.size(), stats_.seconds);
	LOGI("d3d11", "  download wait %.3fs, download copy %.3fs, validate %.3fs, textures %.3fs, build %.3fs, submit %.3fs (incl. textures/downloads), display %.3fs",
		stats_.tDownloadWait, stats_.tDownloadCopy, stats_.tValidate, stats_.tTexture, stats_.tBuild, stats_.tSubmit, stats_.tDisplay);
	LOGI("d3d11", "  %llu draws textured from surfaces, %llu guest faults", (unsigned long long)stats_.rttDraws, (unsigned long long)faults_);
	LOGI("d3d11", "  texture hash %.3fs (pure %.3fs, %.1f MB), decode %.3fs (%.1f Mtexels)", texCache_->hashSeconds, texCache_->pureHashSeconds, texCache_->hashBytes / 1e6,
		texCache_->decodeSeconds, texCache_->decodeTexels / 1e6);
}

// --- Primitives ---

void TexCoordsPerspective(const RVertex &v, bool proj, GpuVertex &g) {
	const float w24 = TruncateToFloat24(v.clipw);
	double r;
	if (w24 > 0.0f && IsFiniteF(w24))
		r = GERecip(w24);
	else
		r = v.clipw != 0.0f ? 1.0 / v.clipw : 1.0;
	g.s = (float)(v.s * r);
	g.t = (float)(v.t * r);
	g.q = (float)((proj ? v.q : 1.0f) * r);
}

// rasterizer.cpp UVPlaneGradient for a triangle: the largest s and t plane gradient, in texels per
// pixel (of the render scale).
float TriangleLodImpl(const GpuVertex *v, const SamplerState &ss, int scale) {
	const double x1 = v[1].x - v[0].x, y1 = v[1].y - v[0].y, x2 = v[2].x - v[0].x, y2 = v[2].y - v[0].y;
	const double det = x1 * y2 - x2 * y1;
	if (det == 0.0)
		return 0.0f;
	auto grad = [&](double f0, double f1, double f2) {
		const double d1 = f1 - f0, d2 = f2 - f0;
		const double kx = (d1 * y2 - d2 * y1) / det, ky = (d2 * x1 - d1 * x2) / det;
		return std::max(std::abs(kx), std::abs(ky));
	};
	const double gs = grad(v[0].s, v[1].s, v[2].s) * (double)(1 << ss.width0Shift);
	const double gt = grad(v[0].t, v[1].t, v[2].t) * (double)(1 << ss.height0Shift);
	return (float)(std::max(gs, gt) / scale);
}

void D3D11Backend::EmitTriangle(const BatchPrim &p, const RasterState &st) {
	const bool textured = st.enableTextures && !st.pix.clearMode;
	const bool flat = !st.shadeGouraud;
	const bool proj = st.textureProj && !st.throughMode;
	const RVertex &pv = p.v[2];
	float iw = 1.0f, ih = 1.0f;
	if (textured && st.throughMode) {
		iw = 1.0f / (float)(1 << st.samp.width0Shift);
		ih = 1.0f / (float)(1 << st.samp.height0Shift);
	}
	for (int k = 0; k < 3; ++k) {
		const RVertex &v = p.v[k];
		GpuVertex g;
		g.x = (float)v.x * (1.0f / SUBPIX);
		g.y = (float)v.y * (1.0f / SUBPIX);
		g.z = (float)v.z;
		g.fog = st.pix.applyFog ? ClampFogDepth(v.fog) : 255.0f;
		g.c0 = flat ? pv.color0 : v.color0;
		g.c1 = flat ? pv.color1 : v.color1;
		g.s = g.t = 0.0f;
		g.q = 1.0f;
		if (textured) {
			if (st.throughMode) {
				g.s = v.s * iw;
				g.t = v.t * ih;
			} else {
				TexCoordsPerspective(v, proj, g);
			}
		}
		g.state = p.stateIndex;
		g.lod = 0.0f;
		verts_.push_back(g);
	}
	if (textured) {
		const float lod = TriangleLod(&verts_[verts_.size() - 3], st.samp);
		for (size_t k = verts_.size() - 3; k < verts_.size(); ++k)
			verts_[k].lod = lod;
	}
}

void D3D11Backend::EmitRect(const BatchPrim &p, const RasterState &st, bool clear) {
	const RVertex &v0 = p.v[0], &v1 = p.v[1];
	int px0, py0, px1, py1;
	if (clear ? !ClearBounds(v0, v1, p.range, &px0, &py0, &px1, &py1) : !RectBounds(v0, v1, p.range, &px0, &py0, &px1, &py1))
		return;
	GpuVertex base{};
	base.z = (float)v1.z;
	base.fog = st.pix.applyFog ? ClampFogDepth(v1.fog) : 255.0f;
	base.c0 = v1.color0;
	base.c1 = clear ? 0 : v1.color1;
	base.q = 1.0f;
	base.state = p.stateIndex;
	const bool textured = !clear && st.enableTextures && !st.pix.clearMode;
	LinearFn fs, ft, fq;
	if (textured) {
		// rasterizer.cpp DrawRectangle: UV planes from three corners whose s, t and q come from the two
		// vertices component by component (gpu/probe exp57, exp122, exp129).
		float tc0s = v0.s, tc0t = v0.t, tc1s = v1.s, tc1t = v1.t;
		if (st.throughMode) {
			const float iw = 1.0f / (float)(1 << st.samp.width0Shift), ih = 1.0f / (float)(1 << st.samp.height0Shift);
			tc0s *= iw;
			tc1s *= iw;
			tc0t *= ih;
			tc1t *= ih;
		}
		const bool right = v0.x < v1.x, down = v0.y < v1.y;
		const bool swapST = right != down;
		const RVertex *vv[2] = { &v0, &v1 };
		double S[2], T[2], Q[2];
		for (int k = 0; k < 2; ++k) {
			const double r = st.throughMode ? 1.0 : GERecip(TruncateToFloat24(vv[k]->clipw));
			Q[k] = r;
			S[k] = (double)(k == 0 ? tc0s : tc1s) * r;
			T[k] = (double)(k == 0 ? tc0t : tc1t) * r;
		}
		const int64_t left = std::min(v0.x, v1.x), top = std::min(v0.y, v1.y);
		const int64_t rightX = std::max(v0.x, v1.x), bottom = std::max(v0.y, v1.y);
		const bool bottomLeftFirst = right && !down;
		const int64_t CX[3] = { left, rightX, bottomLeftFirst ? rightX : left };
		const int64_t CY[3] = { bottomLeftFirst ? bottom : top, top, bottom };
		double cs[3], ct[3], cq[3];
		for (int k = 0; k < 3; ++k) {
			const int xs = CX[k] == v0.x ? 0 : 1, ys = CY[k] == v0.y ? 0 : 1;
			cs[k] = S[swapST ? ys : xs];
			ct[k] = T[swapST ? xs : ys];
			cq[k] = Q[xs];
		}
		fs = PlaneThrough(CX, CY, cs);
		ft = PlaneThrough(CX, CY, ct);
		fq = PlaneThrough(CX, CY, cq);
		// rasterizer.cpp UVPlaneGradient: texels per pixel.
		const double gs = std::max(std::abs(fs.b), std::abs(fs.c)) * SUBPIX * (double)(1 << st.samp.width0Shift);
		const double gt = std::max(std::abs(ft.b), std::abs(ft.c)) * SUBPIX * (double)(1 << st.samp.height0Shift);
		base.lod = (float)(std::max(gs, gt) / scale_);
	}
	const int xs[2] = { px0, px1 + 1 }, ys[2] = { py0, py1 + 1 };
	GpuVertex c[4];
	for (int k = 0; k < 4; ++k) {
		c[k] = base;
		c[k].x = (float)xs[k & 1];
		c[k].y = (float)ys[k >> 1];
		if (textured) {
			const double sx = (double)xs[k & 1] * SUBPIX, sy = (double)ys[k >> 1] * SUBPIX;
			c[k].s = (float)fs.At(sx, sy);
			c[k].t = (float)ft.At(sx, sy);
			c[k].q = (float)fq.At(sx, sy);
		}
	}
	verts_.push_back(c[0]);
	verts_.push_back(c[1]);
	verts_.push_back(c[2]);
	verts_.push_back(c[2]);
	verts_.push_back(c[1]);
	verts_.push_back(c[3]);
}

void D3D11Backend::EmitPoint(const BatchPrim &p, const RasterState &st) {
	const RVertex &v = p.v[0];
	const int px = v.x / SUBPIX, py = v.y / SUBPIX;
	GpuVertex base{};
	base.z = (float)v.z;
	base.fog = st.pix.applyFog ? ClampFogDepth(v.fog) : 255.0f;
	base.c0 = v.color0;
	base.c1 = v.color1 & 0x00FFFFFF;
	base.q = 1.0f;
	base.state = p.stateIndex;
	base.lod = -1e-30f;
	if (st.enableTextures && !st.pix.clearMode) {
		if (st.throughMode) {
			base.s = GETruncateTexCoord(v.s) * (1.0f / (float)(1 << st.samp.width0Shift));
			base.t = GETruncateTexCoord(v.t) * (1.0f / (float)(1 << st.samp.height0Shift));
		} else {
			// The level's q is 1/w (rasterizer.cpp DrawPoint).
			const double r = v.clipw != 0.0f ? 1.0 / v.clipw : 1.0;
			base.s = (float)(v.s * r);
			base.t = (float)(v.t * r);
			base.q = (float)((st.textureProj ? v.q : 1.0f) * r);
		}
	}
	GpuVertex c[4];
	for (int k = 0; k < 4; ++k) {
		c[k] = base;
		c[k].x = (float)(px + (k & 1));
		c[k].y = (float)(py + (k >> 1));
	}
	verts_.push_back(c[0]);
	verts_.push_back(c[1]);
	verts_.push_back(c[2]);
	verts_.push_back(c[2]);
	verts_.push_back(c[1]);
	verts_.push_back(c[3]);
}

void D3D11Backend::EmitLine(const BatchPrim &p, const RasterState &st) {
	const bool textured = st.enableTextures && !st.pix.clearMode;
	const RVertex &v0 = p.v[0], &v1 = p.v[1];
	const bool interpolate = st.shadeGouraud && !(v0.color0 == v1.color0 && v0.color1 == v1.color1);
	const bool proj = st.textureProj && !st.throughMode;
	for (int k = 0; k < 2; ++k) {
		const RVertex &v = p.v[k];
		GpuVertex g;
		g.x = (float)v.x * (1.0f / SUBPIX);
		g.y = (float)v.y * (1.0f / SUBPIX);
		g.z = (float)v.z;
		g.fog = st.pix.applyFog ? ClampFogDepth(v.fog) : 255.0f;
		g.c0 = interpolate ? v.color0 : v1.color0;
		g.c1 = interpolate ? v.color1 : v1.color1;
		g.s = g.t = 0.0f;
		g.q = 1.0f;
		if (textured) {
			if (st.throughMode) {
				g.s = v.s * (1.0f / (float)(1 << st.samp.width0Shift));
				g.t = v.t * (1.0f / (float)(1 << st.samp.height0Shift));
			} else {
				TexCoordsPerspective(v, proj, g);
			}
		}
		g.state = p.stateIndex;
		g.lod = 0.0f;
		verts_.push_back(g);
	}
	if (textured) {
		// The texel step along the line (rasterizer.cpp DrawLine's ds, dt).
		GpuVertex *a = &verts_[verts_.size() - 2], *b = a + 1;
		const float steps = std::max(1.0f, std::max(std::abs(b->x - a->x), std::abs(b->y - a->y)));
		const float ua = a->q != 0.0f ? a->s / a->q : 0.0f, ub = b->q != 0.0f ? b->s / b->q : 0.0f;
		const float va = a->q != 0.0f ? a->t / a->q : 0.0f, vb = b->q != 0.0f ? b->t / b->q : 0.0f;
		const float grad = std::max(std::abs(ub - ua) * (float)(1 << st.samp.width0Shift), std::abs(vb - va) * (float)(1 << st.samp.height0Shift)) / steps;
		a->lod = b->lod = -std::max(grad / (float)scale_, 1e-30f);
	}
}

void D3D11Backend::BuildVertices(const BatchPrim *prims, size_t n, const RasterState *states) {
	verts_.clear();
	segs_.clear();
	for (size_t i = 0; i < n; ++i) {
		const BatchPrim &p = prims[i];
		const RasterState &st = states[p.stateIndex];
		const bool lines = p.type == PrimType::Line;
		if (segs_.empty() || segs_.back().lines != lines ||
			(p.stateIndex != segs_.back().lastState && !Compatible(states[segs_.back().state], st))) {
			segs_.push_back({ (u32)verts_.size(), 0, p.stateIndex, p.stateIndex, lines });
		}
		Segment &seg = segs_.back();
		seg.lastState = p.stateIndex;
		const size_t before = verts_.size();
		switch (p.type) {
		case PrimType::Triangle: EmitTriangle(p, st); break;
		case PrimType::Rect: EmitRect(p, st, false); break;
		case PrimType::ClearRect: EmitRect(p, st, true); break;
		case PrimType::Point: EmitPoint(p, st); break;
		case PrimType::Line: EmitLine(p, st); break;
		}
		seg.count += (u32)(verts_.size() - before);
	}
}

void D3D11Backend::FillStates(const RasterState *states, size_t n) {
	gpuStates_.resize(n);
	for (size_t i = 0; i < n; ++i) {
		const RasterState &rs = states[i];
		const PixelState &ps = rs.pix;
		DrawStateGpu &d = gpuStates_[i];
		memset(&d, 0, sizeof(d));
		u32 f = 0;
		if (ps.alphaBlend) f |= F_ALPHA_BLEND;
		if (ps.applyLogicOp) f |= F_LOGIC_OP;
		if (ps.applyFog) f |= F_FOG;
		if (ps.colorTest) f |= F_COLOR_TEST;
		if (ps.stencilTest) f |= F_STENCIL_TEST;
		if (ps.applyDepthRange) f |= F_DEPTH_RANGE;
		if (ps.dithering) f |= F_DITHER;
		if (ps.applyColorWriteMask) f |= F_WRITE_MASK;
		if (ps.hasAlphaTestMask) f |= F_ALPHA_MASK;
		if (ps.hasStencilTestMask) f |= F_STENCIL_MASK;
		if (ps.depthWrite) f |= F_DEPTH_WRITE;
		if (ps.colorClear) f |= F_COLOR_CLEAR;
		if (ps.stencilClear) f |= F_STENCIL_CLEAR;
		if (ps.depthClear) f |= F_DEPTH_CLEAR;
		const bool textured = rs.enableTextures && !ps.clearMode;
		if (textured && rs.samp.useColorDoubling) f |= F_DBL_SEC;
		d.flags = f;
		d.alphaTest = (u32)ps.alphaTestFunc | ((u32)ps.alphaTestRef << 8) | ((u32)ps.alphaTestMask << 16);
		d.colorTestFunc = ps.colorTestFunc;
		d.colorTestRef = ps.colorTestRef;
		d.colorTestMask = ps.colorTestMask;
		d.stencilTest = (u32)ps.stencilTestFunc | ((u32)ps.stencilTestRef << 8) | ((u32)ps.stencilRef << 16) | ((u32)ps.stencilTestMask << 24);
		d.stencilOps = (u32)ps.sFail | ((u32)ps.zFail << 4) | ((u32)ps.zPass << 8);
		d.depthFunc = ps.depthTestFunc;
		d.minz = (u32)ps.minz;
		d.maxz = (u32)ps.maxz;
		d.blend = (u32)ps.blendEq | ((u32)ps.blendSrc << 4) | ((u32)ps.blendDst << 8);
		d.fixA = ps.fixA;
		d.fixB = ps.fixB;
		d.fogColor = ps.fogColor;
		d.writeMask = ps.colorWriteMask;
		d.logicOp = ps.logicOp;
		if (ps.dithering) {
			for (int k = 0; k < 16; ++k) {
				const u32 nib = (u32)(u8)ps.ditherMatrix[k] & 0xF;
				if (k < 8)
					d.dither0 |= nib << (k * 4);
				else
					d.dither1 |= nib << ((k - 8) * 4);
			}
		}
		if (textured) {
			const SamplerState &ss = rs.samp;
			d.texFunc = (u32)ss.texFunc | (ss.useTextureAlpha ? 0x10u : 0u) | (ss.useColorDoubling ? 0x20u : 0u);
			d.texEnv = ss.texBlendColor;
			// Texture filtering as the game sets it, or always nearest / linear.
			u32 filt = (ss.minFilt ? 4u : 0u) | (ss.magFilt ? 8u : 0u);
			if (texFilter_ == 1)
				filt = 0;
			else if (texFilter_ == 2)
				filt = 12;
			d.texMode = (ss.clampS ? 1u : 0u) | (ss.clampT ? 2u : 0u) | filt | (ss.mipFilt ? 0x10u : 0u) | ((u32)(ss.maxLevel & 7) << 5) |
				((u32)(ss.texLevelMode & 3) << 8);
			d.texLevelOffset = ss.texLevelOffset;
			d.lodSlopeLog = GELog16(ss.lodSlope);
			int aw, ah;
			TextureAtlasLayout(ss, d.texLevels, &aw, &ah);
			// The texture cache's textures are upscaled: sampled at their scale (render to texture replaces
			// this below).
			const int up = TextureUpscale(texUpscale_, aw, ah);
			if (up > 1) {
				for (u32 &lv : d.texLevels)
					lv = (lv & 0xFFFF0000u) | ((lv & 0xFFFFu) * (u32)up);
				d.rtt = (u32)up << 12;
			}
		}
	}
}

// A texture that is (part of) a surface the GPU holds, in the same pixel format and row length: it's
// sampled from the surface, at the render scale, without going through guest memory.
Surface *D3D11Backend::FindTextureSurface(const SamplerState &ss, int *x0, int *y0) {
	if (ss.swizzle || ss.maxLevel > 0 || ss.texfmt > GE_TFMT_8888)
		return nullptr;
	const TexLevel &lv = ss.levels[0];
	const u32 a = FoldAddress(lv.addr);
	if (a < VRAM_BASE || a >= VRAM_BASE + VRAM_SIZE)
		return nullptr;
	const u32 off = a - VRAM_BASE;
	const int w = std::min<int>(lv.w, 512), h = std::min<int>(lv.h, 512);
	Surface *best = nullptr;
	for (auto &sp : surfaces_) {
		Surface *s = sp.get();
		if (s->fmt != ss.texfmt || s->stride != lv.bufw || !s->drawn || off < s->addr)
			continue;
		const u32 rel = off - s->addr;
		if (rel % (u32)s->bpp != 0)
			continue;
		const int ty = (int)(rel / s->RowBytes()), tx = (int)((rel % s->RowBytes()) / (u32)s->bpp);
		if (tx + w > s->width || ty + h > s->height)
			continue;
		if (!best || s->Dirty() || s->lastUse > best->lastUse) {
			best = s;
			*x0 = tx;
			*y0 = ty;
		}
	}
	return best;
}

void D3D11Backend::ResolveRenderToTexture(const RasterState *states, size_t n) {
	rttSurf_.assign(n, nullptr);
	for (size_t i = 0; i < n; ++i) {
		const RasterState &st = states[i];
		if (!st.enableTextures || st.pix.clearMode)
			continue;
		if (i > 0 && states[i - 1].enableTextures && !states[i - 1].pix.clearMode && SameTexture(states[i - 1].samp, st.samp)) {
			rttSurf_[i] = rttSurf_[i - 1];
			gpuStates_[i].rtt = gpuStates_[i - 1].rtt;
			gpuStates_[i].texLevels[0] = gpuStates_[i - 1].texLevels[0];
			gpuStates_[i].rttY = gpuStates_[i - 1].rttY;
			continue;
		}
		int x0 = 0, y0 = 0;
		Surface *s = FindTextureSurface(st.samp, &x0, &y0);
		if (!s)
			continue;
		// The surface's copy of those rows must be current (the CPU may have written some).
		ValidateRows(s, y0, y0 + std::min<int>(st.samp.levels[0].h, 512) - 1);
		rttSurf_[i] = s;
		DrawStateGpu &d = gpuStates_[i];
		d.rtt = (u32)s->fmt | 0x100 | ((u32)scale_ << 12);
		d.texLevels[0] = (u32)(x0 * scale_) | (d.texLevels[0] & 0xFFFF0000u);
		d.rttY = (u32)(y0 * scale_);
	}
}

ID3D11ShaderResourceView *D3D11Backend::FeedbackCopy(Surface *s) {
	const int w = s->width * scale_, h = s->height * scale_;
	if (!feedbackTex_ || feedbackW_ != w || feedbackH_ != h) {
		if (!MakeTexture(w, h, false, std::addressof(feedbackTex_), nullptr, std::addressof(feedbackSrv_)))
			return nullptr;
		feedbackW_ = w;
		feedbackH_ = h;
	}
	UnbindOM();
	ctx_->CopyResource(feedbackTex_.Get(), s->tex.Get());
	return feedbackSrv_.Get();
}

void D3D11Backend::BindDraw(Surface *color, Surface *depth) {
	const UINT stride = sizeof(GpuVertex), offset = 0;
	ctx_->IASetInputLayout(layout_.Get());
	ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
	ctx_->VSSetShader(vs_.Get(), nullptr, 0);
	ctx_->VSSetConstantBuffers(0, 1, targetCB_.GetAddressOf());
	ctx_->PSSetConstantBuffers(0, 1, targetCB_.GetAddressOf());
	ctx_->PSSetShaderResources(1, 1, stateSrv_.GetAddressOf());
	ctx_->RSSetState(raster_.Get());
	ctx_->OMSetDepthStencilState(noDepth_.Get(), 0);
	ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)(color->width * scale_), (float)(color->height * scale_), 0.0f, 1.0f };
	ctx_->RSSetViewports(1, &vp);
	ID3D11UnorderedAccessView *uavs[2] = { color->uav.Get(), depth ? depth->uav.Get() : dummyUav_.Get() };
	ctx_->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 2, uavs, nullptr);
	omDirty_ = false;
}

void D3D11Backend::DrawBatch(const BatchPrim *prims, size_t n, const RasterState *states, size_t ns) {
	if (n == 0 || ns == 0)
		return;
	std::lock_guard<std::recursive_mutex> lock(mu_);
	ScopedTimer timer(&stats_.seconds);
	stats_.batches++;
	stats_.prims += n;
	const PixelState &p0 = states[0].pix;
	int minRow = INT_MAX, maxRow = -1;
	for (size_t i = 0; i < n; ++i) {
		minRow = std::min(minRow, prims[i].range.y1 / SUBPIX);
		maxRow = std::max(maxRow, prims[i].range.y2 / SUBPIX);
	}
	bool usesDepth = false, writesDepth = false;
	for (size_t i = 0; i < ns; ++i) {
		const PixelState &ps = states[i].pix;
		const bool clearZ = ps.clearMode && ps.depthClear;
		usesDepth |= ps.depthWrite || ps.depthTestFunc != GE_COMP_ALWAYS || clearZ;
		writesDepth |= ps.depthWrite || clearZ;
	}
	minRow = std::max(minRow, 0);
	maxRow = std::min(maxRow, MAX_SURFACE_H - 1);
	if (minRow > maxRow)
		return;
	Surface *color = GetSurface(p0.fbOffset, p0.fbStride, p0.fbFormat, maxRow + 1);
	if (!color)
		return;
	Surface *depth = usesDepth ? GetSurface(p0.zbOffset, p0.zbStride, FMT_DEPTH, maxRow + 1) : nullptr;
	ValidateRows(color, minRow, maxRow);
	if (depth)
		ValidateRows(depth, minRow, maxRow);

	{
		ScopedTimer tb(&stats_.tBuild);
		BuildVertices(prims, n, states);
		FillStates(states, ns);
	}
	if (verts_.empty())
		return;
	ResolveRenderToTexture(states, ns);
	stats_.verts += verts_.size();
	ScopedTimer tsub(&stats_.tSubmit);

	// Vertex and state buffers for the whole batch.
	const size_t vbNeed = verts_.size() * sizeof(GpuVertex);
	if (vbBytes_ < vbNeed) {
		size_t cap = std::max<size_t>(vbBytes_ * 2, 1 << 20);
		while (cap < vbNeed)
			cap *= 2;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (UINT)cap;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		vb_.Reset();
		if (FAILED(dev_->CreateBuffer(&bd, nullptr, &vb_)))
			return;
		vbBytes_ = cap;
	}
	if (stateCap_ < ns) {
		size_t cap = std::max<size_t>(stateCap_ * 2, 256);
		while (cap < ns)
			cap *= 2;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (UINT)(cap * sizeof(DrawStateGpu));
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(DrawStateGpu);
		stateBuf_.Reset();
		stateSrv_.Reset();
		if (FAILED(dev_->CreateBuffer(&bd, nullptr, &stateBuf_)) || FAILED(dev_->CreateShaderResourceView(stateBuf_.Get(), nullptr, &stateSrv_)))
			return;
		stateCap_ = cap;
	}
	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(ctx_->Map(vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return;
	memcpy(m.pData, verts_.data(), vbNeed);
	ctx_->Unmap(vb_.Get(), 0);
	if (FAILED(ctx_->Map(stateBuf_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return;
	memcpy(m.pData, gpuStates_.data(), ns * sizeof(DrawStateGpu));
	ctx_->Unmap(stateBuf_.Get(), 0);
	TargetCB tcb{};
	tcb.ndcScale[0] = 2.0f / (float)color->width;
	tcb.ndcScale[1] = -2.0f / (float)color->height;
	tcb.ndcOffset[0] = -1.0f;
	tcb.ndcOffset[1] = 1.0f;
	tcb.scale = (u32)scale_;
	ctx_->UpdateSubresource(targetCB_.Get(), 0, nullptr, &tcb, 0, 0);
	BindDraw(color, depth);

	D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	for (const Segment &seg : segs_) {
		if (seg.count == 0)
			continue;
		const RasterState &st = states[seg.state];
		const bool textured = st.enableTextures && !st.pix.clearMode;
		ID3D11ShaderResourceView *srv = nullptr;
		if (textured) {
			// May write rendering back to guest memory first (render to texture).
			if (Surface *rs = rttSurf_[seg.state]) {
				// Render to texture: the surface itself (a copy when it's the target being drawn).
				ID3D11ShaderResourceView *rtt = rs->srv.Get();
				if (rs == color)
					rtt = FeedbackCopy(color);
				if (!rtt)
					continue;
				ctx_->PSSetShaderResources(2, 1, &rtt);
				stats_.rttDraws++;
			} else {
				ScopedTimer tt(&stats_.tTexture);
				srv = texCache_->Lookup(st.samp, runSeq_, &SyncRangeThunk, this);
				if (!srv)
					continue;
			}
		}
		ID3D11PixelShader *ps = PixelShader(st.pix.fbFormat, st.pix.clearMode, textured);
		if (!ps)
			continue;
		if (omDirty_)
			BindDraw(color, depth);
		ctx_->PSSetShader(ps, nullptr, 0);
		ctx_->PSSetShaderResources(0, 1, &srv);
		D3D11_RECT sc;
		sc.left = std::clamp(st.scissor.x1 / SUBPIX, 0, color->width) * scale_;
		sc.top = std::clamp(st.scissor.y1 / SUBPIX, 0, color->height) * scale_;
		sc.right = std::clamp(st.scissor.x2 / SUBPIX + 1, 0, color->width) * scale_;
		sc.bottom = std::clamp(st.scissor.y2 / SUBPIX + 1, 0, color->height) * scale_;
		ctx_->RSSetScissorRects(1, &sc);
		const D3D11_PRIMITIVE_TOPOLOGY want = seg.lines ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		if (want != topo) {
			ctx_->IASetPrimitiveTopology(want);
			topo = want;
		}
		MarkDirty(color, minRow, maxRow);
		if (depth && writesDepth)
			MarkDirty(depth, minRow, maxRow);
		ctx_->Draw(seg.count, seg.first);
		stats_.draws++;
		pendingDraws_ = true;
	}
	Protect(color);
	if (depth)
		Protect(depth);
}

// The surface a displayed framebuffer is, when the GPU drew it; nullptr otherwise.
Surface *D3D11Backend::DisplaySurface(u32 addr, int stride, int format, int width) {
	const u32 a = FoldAddress(addr);
	if (a < VRAM_BASE || a >= VRAM_BASE + VRAM_SIZE || stride <= 0)
		return nullptr;
	Surface *s = FindSurface(a - VRAM_BASE, stride, format & 3);
	return s && s->drawn && s->width >= width ? s : nullptr;
}

// The surface's top left width x height PSP pixels as RGBA8888 at outScale (at most the render scale),
// read back.
bool D3D11Backend::ReadDisplay(Surface *s, int width, int height, int format, int outScale, std::vector<u32> &out) {
	const int w = width * outScale, h = height * outScale;
	if (!EnsureScratch(displayTex_, std::addressof(displayUav_), nullptr, w, h, &displayW_, &displayH_, false) ||
		!EnsureScratch(displayStaging_, nullptr, nullptr, w, h, &displayStagingW_, &displayStagingH_, true))
		return false;
	CopyCB cb{};
	cb.size[0] = (u32)width;
	cb.size[1] = (u32)height;
	cb.scale = (u32)scale_;
	cb.format = (u32)(format & 3);
	cb.outScale = (u32)outScale;
	RunCompute(csDisplay_.Get(), s->srv.Get(), displayUav_.Get(), cb, (UINT)(w + 7) / 8, (UINT)(h + 7) / 8);
	const D3D11_BOX box{ 0, 0, 0, (UINT)w, (UINT)h, 1 };
	ctx_->CopySubresourceRegion(displayStaging_.Get(), 0, 0, 0, 0, displayTex_.Get(), 0, &box);
	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(ctx_->Map(displayStaging_.Get(), 0, D3D11_MAP_READ, 0, &m)))
		return false;
	out.resize((size_t)w * h);
	for (int y = 0; y < h; ++y)
		memcpy(&out[(size_t)y * w], (const u8 *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
	ctx_->Unmap(displayStaging_.Get(), 0);
	return true;
}

bool D3D11Backend::GetDisplayImage(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out, int *outW, int *outH,
                                   int outScale) {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	Surface *s = scale_ > 1 ? DisplaySurface(addr, stride, format, width) : nullptr;
	if (!s) {
		SyncRange(addr, (u32)(height * stride * (format == GE_FORMAT_8888 ? 4 : 2)));
		return false;
	}
	ScopedTimer timer(&stats_.seconds);
	ScopedTimer td(&stats_.tDisplay);
	height = std::min(height, s->height);
	ValidateRows(s, 0, height - 1);
	const int k = outScale > 0 ? std::min(outScale, scale_) : scale_;
	if (!ReadDisplay(s, width, height, format, k, out))
		return false;
	*outW = width * k;
	*outH = height * k;
	return true;
}

// --- Presentation ---

// The swap chain on the host window, at the window's size.
bool D3D11Backend::EnsureSwapChain() {
	if (!window_ || presentFailed_)
		return false;
	RECT rc{};
	GetClientRect(window_, &rc);
	const int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (!swapChain_) {
		ComPtr<IDXGIDevice> dxgiDev;
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<IDXGIFactory2> factory;
		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.Width = (UINT)std::max(w, 1);
		sd.Height = (UINT)std::max(h, 1);
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 3;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		if (FAILED(dev_.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) ||
			FAILED(factory->CreateSwapChainForHwnd(dev_.Get(), window_, &sd, nullptr, nullptr, &swapChain_))) {
			LOGW("d3d11", "no swap chain on the window: frames are read back and presented by the host");
			presentFailed_ = true;
			swapChain_.Reset();
			return false;
		}
		// The host toggles fullscreen (borderless) itself.
		factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
		backW_ = (int)sd.Width;
		backH_ = (int)sd.Height;
		LOGI("d3d11", "presenting to the window (%d x %d)", backW_, backH_);
	} else if (w > 0 && h > 0 && (w != backW_ || h != backH_)) {
		backRtv_.Reset();
		ctx_->OMSetRenderTargets(0, nullptr, nullptr);
		omDirty_ = true;
		if (FAILED(swapChain_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0))) {
			LOGW("d3d11", "resizing the swap chain to %d x %d failed", w, h);
			return false;
		}
		backW_ = w;
		backH_ = h;
	}
	if (!backRtv_) {
		ComPtr<ID3D11Texture2D> back;
		if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))) || FAILED(dev_->CreateRenderTargetView(back.Get(), nullptr, &backRtv_)))
			return false;
	}
	return true;
}

// Where the 480:272 picture goes in the back buffer: as large as fits, centered.
void D3D11Backend::PictureRect(int *x, int *y, int *w, int *h) const {
	int pw = backW_, ph = (int)((s64)backW_ * 272 / 480);
	if (ph > backH_) {
		ph = backH_;
		pw = (int)((s64)backH_ * 480 / 272);
	}
	*w = std::max(pw, 1);
	*h = std::max(ph, 1);
	*x = (backW_ - *w) / 2;
	*y = (backH_ - *h) / 2;
}

// Draws an image (raw pixels in `format`, srcW x srcH texels), and an overlay over it, into the back
// buffer and presents it.
void D3D11Backend::DrawPresent(ID3D11ShaderResourceView *src, int srcW, int srcH, int format, bool bilinear, ID3D11ShaderResourceView *overlay,
                               int overlayW, int overlayH) {
	int x, y, w, h;
	PictureRect(&x, &y, &w, &h);
	PresentCB cb{};
	cb.dstOrigin[0] = (float)x;
	cb.dstOrigin[1] = (float)y;
	cb.dstScale[0] = (float)srcW / (float)w;
	cb.dstScale[1] = (float)srcH / (float)h;
	cb.srcSize[0] = srcW;
	cb.srcSize[1] = srcH;
	cb.srcFormat = (u32)(format & 3);
	cb.magFilter = bilinear ? 1u : 0u;
	cb.overlaySize[0] = overlay ? overlayW : 0;
	cb.overlaySize[1] = overlay ? overlayH : 0;
	UnbindOM();
	ctx_->UpdateSubresource(presentCB_.Get(), 0, nullptr, &cb, 0, 0);
	const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	ctx_->ClearRenderTargetView(backRtv_.Get(), black);
	const D3D11_VIEWPORT vp{ (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
	const D3D11_RECT sc{ 0, 0, backW_, backH_ };
	ctx_->RSSetViewports(1, &vp);
	ctx_->RSSetScissorRects(1, &sc);
	ctx_->RSSetState(raster_.Get());
	ctx_->IASetInputLayout(nullptr);
	ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx_->VSSetShader(presentVs_.Get(), nullptr, 0);
	ctx_->PSSetShader(presentPs_.Get(), nullptr, 0);
	ctx_->PSSetConstantBuffers(0, 1, presentCB_.GetAddressOf());
	ID3D11ShaderResourceView *srvs[2] = { src, overlay };
	ctx_->PSSetShaderResources(0, 2, srvs);
	ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	ctx_->OMSetDepthStencilState(noDepth_.Get(), 0);
	ctx_->OMSetRenderTargets(1, backRtv_.GetAddressOf(), nullptr);
	ctx_->Draw(3, 0);
	ID3D11ShaderResourceView *nullSrvs[2] = {};
	ctx_->PSSetShaderResources(0, 2, nullSrvs);
	// Diagnostics: PSPC_HOST_SNAPSHOT=<file.bmp> saves the window's image of the 600th presented frame, or
	// the PSPC_HOST_SNAPSHOT_AT=<n>th (screen captures fail while the desktop is locked).
	static const char *snap = getenv("PSPC_HOST_SNAPSHOT");
	static const int snapAt = [] {
		const char *at = getenv("PSPC_HOST_SNAPSHOT_AT");
		return at && atoi(at) > 0 ? atoi(at) : 600;
	}();
	if (snap && ++presents_ == snapAt) {
		ComPtr<ID3D11Texture2D> back, staging;
		D3D11_TEXTURE2D_DESC td{};
		if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)))) {
			back->GetDesc(&td);
			td.Usage = D3D11_USAGE_STAGING;
			td.BindFlags = 0;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			td.MiscFlags = 0;
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &staging))) {
				ctx_->CopyResource(staging.Get(), back.Get());
				if (SUCCEEDED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
					WriteBmp(snap, (const u8 *)m.pData, (int)td.Width, (int)td.Height, m.RowPitch);
					ctx_->Unmap(staging.Get(), 0);
					LOGI("d3d11", "window snapshot (%u x %u) saved to %s", td.Width, td.Height, snap);
				}
			}
		}
	}
	ctx_->OMSetRenderTargets(0, nullptr, nullptr);
	omDirty_ = true;
	// Never waits for the display: the newest frame is shown at its next refresh.
	const HRESULT hr = swapChain_->Present(0, 0);
	pendingDraws_ = false;
	if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED)
		LOGW("d3d11", "Present failed (0x%08x)", (unsigned)hr);
}

bool D3D11Backend::PresentDisplay(u32 addr, int stride, int format, int width, int height,
                                  const std::function<void(u32 *, int, int)> &overlay, bool capture) {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	if (!window_ || presentFailed_)
		return false;
	Surface *s = DisplaySurface(addr, stride, format, width);
	if (!s || !EnsureSwapChain())
		return false;
	ScopedTimer timer(&stats_.seconds);
	ScopedTimer td(&stats_.tDisplay);
	height = std::min(height, s->height);
	ValidateRows(s, 0, height - 1);
	if (overlay) {
		// Drawn at the smallest whole scale that covers the window's picture.
		int x, y, w, h;
		PictureRect(&x, &y, &w, &h);
		const int k = std::clamp((w + width - 1) / width, 1, MAX_SCALE);
		const int ow = width * k, oh = height * k;
		if (capture) {
			// Onto the picture, read back.
			if (!ReadDisplay(s, width, height, format, std::min(k, scale_), overlayBuf_))
				return false;
			const int iw = width * std::min(k, scale_), ih = height * std::min(k, scale_);
			overlay(overlayBuf_.data(), iw, ih);
			return PresentImageLocked(overlayBuf_.data(), iw, ih, iw);
		}
		overlayBuf_.assign((size_t)ow * oh, 0);
		overlay(overlayBuf_.data(), ow, oh);
		if (!UploadImage(overlayBuf_.data(), ow, oh, ow, overlayTex_, overlaySrv_, &overlayW_, &overlayH_))
			return false;
		DrawPresent(s->srv.Get(), width * scale_, height * scale_, format, scale_ > 1, overlaySrv_.Get(), ow, oh);
		return true;
	}
	DrawPresent(s->srv.Get(), width * scale_, height * scale_, format, scale_ > 1);
	return true;
}

// A CPU image into a texture of the backend's (grown as needed).
bool D3D11Backend::UploadImage(const u32 *rgba, int width, int height, int stride, ComPtr<ID3D11Texture2D> &tex,
                               ComPtr<ID3D11ShaderResourceView> &srv, int *texW, int *texH) {
	if (!tex || *texW < width || *texH < height) {
		const int w = std::max(width, *texW), h = std::max(height, *texH);
		if (!MakeTexture(w, h, false, std::addressof(tex), nullptr, std::addressof(srv)))
			return false;
		*texW = w;
		*texH = h;
	}
	const D3D11_BOX box{ 0, 0, 0, (UINT)width, (UINT)height, 1 };
	ctx_->UpdateSubresource(tex.Get(), 0, &box, rgba, (UINT)stride * 4, 0);
	return true;
}

bool D3D11Backend::PresentImage(const u32 *rgba, int width, int height, int stride) {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	return PresentImageLocked(rgba, width, height, stride);
}

bool D3D11Backend::PresentImageLocked(const u32 *rgba, int width, int height, int stride) {
	if (!window_ || presentFailed_ || width <= 0 || height <= 0 || !EnsureSwapChain() ||
		!UploadImage(rgba, width, height, stride, imageTex_, imageSrv_, &imageW_, &imageH_))
		return false;
	// A PSP-size image enlarged stays sharp, as the software renderer's always has.
	DrawPresent(imageSrv_.Get(), width, height, GE_FORMAT_8888, width > 480);
	return true;
}

void D3D11Backend::ReleaseWindow() {
	std::lock_guard<std::recursive_mutex> lock(mu_);
	if (ctx_)
		ctx_->OMSetRenderTargets(0, nullptr, nullptr);
	backRtv_.Reset();
	swapChain_.Reset();
	window_ = nullptr;
}

}  // namespace

std::unique_ptr<RenderBackend> CreateD3D11Backend(const D3D11Options &opts, std::string *error) {
	auto b = std::make_unique<D3D11Backend>();
	std::string err;
	if (!b->Init(opts, &err)) {
		if (error)
			*error = err;
		return nullptr;
	}
	return b;
}

}  // namespace gpu::hw
