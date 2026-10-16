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
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "common/app_paths.h"
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

class D3D11Backend final : public RenderBackend, private DrawData {
public:
	~D3D11Backend() override;
	bool Init(const D3D11Options &o, std::string *err);
	const char *Name() const override { return "d3d11"; }
	void BeginRun() override {
		std::lock_guard<std::recursive_mutex> lock(mu_);
		runSeq_++;
	}
	void DrawBatch(const BatchPrim *prims, size_t primCount, const RasterState *states, size_t stateCount) override;
	bool TransformsGeometry() const override { return geoEnabled_.load(std::memory_order_acquire); }
	bool VerifiesGeometry() const override { return geoVerify_; }
	D3D11TransformStats TransformStats() const { return { geoVerts, geoJobs, geoChecked, geoMismatches }; }
	void DrawGeometryBatch(const BatchPrim *prims, size_t primCount, const RasterState *states, size_t stateCount,
	                       const GeometryBatch &geo) override;
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
	void DrawBatchImpl(const BatchPrim *prims, size_t n, const RasterState *states, size_t ns, const GeometryBatch *geo);
	// Geometry (gpu/geometry.h): CSVertex and CSTriangle over the batch's vertices and jobs, into geoOut_.
	void InitGeometry(const D3D11Options &o);
	bool CreateGeometryShaders(bool compile);
	bool RunGeometry(const GeometryBatch &g);
	void VerifyGeometry(const GeometryBatch &g, const RasterState *states);
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

	void BindDraw(Surface *color, Surface *depth);
	Surface *FindTextureSurface(const SamplerState &ss, int *x0, int *y0);
	void ResolveRenderToTexture(const RasterState *states, size_t n);
	ID3D11ShaderResourceView *FeedbackCopy(Surface *s);
	void PrintStats();
	// Optional GPU timestamps (PSPC_GPU_PROFILE): collect completed queries without stalling draws.
	void BeginGpuSample(int category);
	void EndGpuSample();
	void CollectGpuSamples();
	Surface *DisplaySurface(u32 addr, int stride, int format, int width);
	bool ReadDisplay(Surface *s, int width, int height, int format, int outScale, std::vector<u32> &out);
	bool EnsureSwapChain();
	void PictureRect(int *x, int *y, int *w, int *h) const;
	bool PresentImageLocked(const u32 *rgba, int width, int height, int stride);
	void DrawPresent(ID3D11ShaderResourceView *src, int srcW, int srcH, int format, bool bilinear, ID3D11ShaderResourceView *overlay = nullptr,
	                 int overlayW = 0, int overlayH = 0);
	bool UploadImage(const u32 *rgba, int width, int height, int stride, ComPtr<ID3D11Texture2D> &tex, ComPtr<ID3D11ShaderResourceView> &srv,
	                 int *texW, int *texH);

	int maxSurfaceW_ = MAX_SURFACE_W;
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

	// Vertex transform on the GPU (geometry.h).
	struct GeoBuffer {
		ComPtr<ID3D11Buffer> buf;
		ComPtr<ID3D11ShaderResourceView> srv;
		ComPtr<ID3D11UnorderedAccessView> uav;
		size_t bytes = 0;
	};
	// An upload buffer (structured, dynamic) for count elements of `stride` bytes, filled from data.
	bool UploadGeo(GeoBuffer &b, const void *data, size_t count, size_t stride);
	std::atomic<bool> geoEnabled_{ false };
	bool geoVerify_ = false, geoDoubles_ = false;
	std::thread geoCompile_;  // compiles the shaders the cache doesn't have
	ComPtr<ID3D11ComputeShader> csGeoVertex_, csGeoTriangle_;
	GeoBuffer geoInputs_, geoDraws_, geoTransforms_, geoJobs_;
	GeoBuffer geoClip_;  // CSVertex's ClipVertex, structured, UAV and SRV
	GeoBuffer geoOut_;   // CSTriangle's GPU vertices, raw UAV and vertex buffer
	ComPtr<ID3D11Buffer> geoCB_, geoStaging_;
	size_t geoStagingBytes_ = 0;
	u64 geoJobs = 0, geoVerts = 0, geoChecked = 0, geoMismatches = 0;

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
	bool profileGpu_ = false;
	struct GpuSample {
		ComPtr<ID3D11Query> disjoint, start, end;
		int category = 0;
	};
	std::deque<GpuSample> gpuPending_;
	std::vector<GpuSample> gpuFree_;
	bool gpuSampleActive_ = false;
	u64 gpuSkipped_ = 0, gpuDisjoint_ = 0, gpuFailed_ = 0;
	struct GpuTiming { double ms = 0; u64 count = 0; } gpuTiming_[6];
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
	if (geoCompile_.joinable())
		geoCompile_.join();
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
		if (profileGpu_ && texCache_) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!gpuPending_.empty() && std::chrono::steady_clock::now() < deadline) {
				CollectGpuSamples();
				if (!gpuPending_.empty()) std::this_thread::yield();
			}
			PrintStats();
		}
	}
}

void D3D11Backend::BeginGpuSample(int category) {
	if (!profileGpu_) return;
	CollectGpuSamples();
	// Bound diagnostic memory even if the GPU stops making progress.
	if (gpuPending_.size() >= 512) { gpuSkipped_++; return; }
	GpuSample sample;
	if (!gpuFree_.empty()) {
		sample = std::move(gpuFree_.back());
		gpuFree_.pop_back();
	} else {
		D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		if (FAILED(dev_->CreateQuery(&desc, &sample.disjoint))) { gpuFailed_++; return; }
		desc.Query = D3D11_QUERY_TIMESTAMP;
		if (FAILED(dev_->CreateQuery(&desc, &sample.start)) || FAILED(dev_->CreateQuery(&desc, &sample.end))) {
			gpuFailed_++;
			return;
		}
	}
	sample.category = category;
	gpuPending_.push_back(std::move(sample));
	auto &q = gpuPending_.back();
	ctx_->Begin(q.disjoint.Get());
	ctx_->End(q.start.Get());
	gpuSampleActive_ = true;
}

void D3D11Backend::EndGpuSample() {
	if (!gpuSampleActive_) return;
	auto &q = gpuPending_.back();
	ctx_->End(q.end.Get());
	ctx_->End(q.disjoint.Get());
	gpuSampleActive_ = false;
}

void D3D11Backend::CollectGpuSamples() {
	while (!gpuPending_.empty()) {
		auto &q = gpuPending_.front();
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		UINT64 start = 0, end = 0;
		const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
		HRESULT status = ctx_->GetData(q.disjoint.Get(), &disjoint, sizeof(disjoint), flags);
		if (status == S_OK) status = ctx_->GetData(q.start.Get(), &start, sizeof(start), flags);
		if (status == S_OK) status = ctx_->GetData(q.end.Get(), &end, sizeof(end), flags);
		if (status == S_FALSE) break;
		if (FAILED(status)) {
			// Device/query failures cannot become ready later; discard rather than clogging the queue.
			gpuFailed_++;
			gpuPending_.pop_front();
			continue;
		}
		if (!disjoint.Disjoint && disjoint.Frequency && end >= start) {
			auto &t = gpuTiming_[q.category];
			t.ms += (double)(end - start) * 1000.0 / (double)disjoint.Frequency;
			t.count++;
		} else {
			gpuDisjoint_++;
		}
		gpuFree_.push_back(std::move(q));
		gpuPending_.pop_front();
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
	// The GE's arithmetic must stay as written (no reassociation, NaNs kept).
	const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | (kind == ShaderKind::Geometry ? D3DCOMPILE_IEEE_STRICTNESS : 0);
	const HRESULT hr = D3DCompile(src.data(), src.size(), entry, defines, nullptr, entry, target, flags, 0, &code, &errors);
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
	profileGpu_ = getenv("PSPC_GPU_PROFILE") != nullptr;
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
	InitGeometry(o);

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
	BeginGpuSample(cs == csExpand_.Get() ? 3 : cs == csReduce_.Get() ? 4 : 5);
	ctx_->Dispatch(gx, gy, 1);
	EndGpuSample();
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
	if (geoEnabled_)
		LOGI("d3d11", "  GPU transform: %llu vertices, %llu triangle jobs%s", (unsigned long long)geoVerts, (unsigned long long)geoJobs,
		     geoVerify_ ? (", " + std::to_string(geoMismatches) + " of " + std::to_string(geoChecked) + " jobs differ from the CPU").c_str() : "");
	LOGI("d3d11", "  texture hash %.3fs (pure %.3fs, %.1f MB), decode %.3fs (%.1f Mtexels)", texCache_->hashSeconds, texCache_->pureHashSeconds, texCache_->hashBytes / 1e6,
		texCache_->decodeSeconds, texCache_->decodeTexels / 1e6);
	if (profileGpu_) {
		CollectGpuSamples();
		const char *names[] = {"textured draws", "untextured draws", "clears", "upload expansion", "readback reduction", "display"};
		for (int i = 0; i < 6; ++i)
			LOGI("d3d11", "  GPU %s: %.3f ms / %llu samples", names[i], gpuTiming_[i].ms, (unsigned long long)gpuTiming_[i].count);
		if (gpuSkipped_ || gpuDisjoint_ || gpuFailed_ || !gpuPending_.empty())
			LOGI("d3d11", "  GPU samples: %llu skipped, %llu disjoint, %llu failed, %zu pending", (unsigned long long)gpuSkipped_,
				(unsigned long long)gpuDisjoint_, (unsigned long long)gpuFailed_, gpuPending_.size());
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
	DrawBatchImpl(prims, n, states, ns, nullptr);
}

void D3D11Backend::DrawGeometryBatch(const BatchPrim *prims, size_t n, const RasterState *states, size_t ns, const GeometryBatch &geo) {
	DrawBatchImpl(prims, n, states, ns, &geo);
}

void D3D11Backend::DrawBatchImpl(const BatchPrim *prims, size_t n, const RasterState *states, size_t ns, const GeometryBatch *geo) {
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
		BuildVertices(prims, n, states, geo);
		FillStates(states, ns);
	}
	if (segs_.empty())
		return;
	if (geo && !geo->Empty()) {
		if (!RunGeometry(*geo)) {
			if (!loggedAllocFailure_)
				LOGE("d3d11", "GPU transform buffers can't be made; the batch isn't drawn");
			loggedAllocFailure_ = true;
			return;
		}
		if (geoVerify_)
			VerifyGeometry(*geo, states);
	}
	ResolveRenderToTexture(states, ns);
	stats_.verts += verts_.size();
	ScopedTimer tsub(&stats_.tSubmit);

	// Vertex and state buffers for the whole batch.
	const size_t vbNeed = std::max<size_t>(verts_.size(), 1) * sizeof(GpuVertex);
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
	if (!verts_.empty()) {
		if (FAILED(ctx_->Map(vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
			return;
		memcpy(m.pData, verts_.data(), verts_.size() * sizeof(GpuVertex));
		ctx_->Unmap(vb_.Get(), 0);
	}
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
	bool gpuVb = false;  // BindDraw binds vb_
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
		if (omDirty_) {
			BindDraw(color, depth);
			gpuVb = false;
		}
		if (seg.gpu != gpuVb) {
			const UINT stride = sizeof(GpuVertex), offset = 0;
			ID3D11Buffer *vb = seg.gpu ? geoOut_.buf.Get() : vb_.Get();
			ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			gpuVb = seg.gpu;
		}
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
		BeginGpuSample(st.pix.clearMode ? 2 : textured ? 0 : 1);
		ctx_->Draw(seg.count, seg.first);
		EndGpuSample();
		stats_.draws++;
		pendingDraws_ = true;
	}
	Protect(color);
	if (depth)
		Protect(depth);
}

// --- Vertex transform on the GPU (gpu/geometry.h) ---

struct GeoCB {
	u32 count;
	u32 scale;
	u32 base;  // CSVertex: the first input
	u32 pad;
};

// Compiled geometry shaders, under the cache root, named by a hash of all that makes them.
fs::path GeometryShaderCache(const char *entry, const std::string &src, bool doubles) {
	u64 h = 0xCBF29CE484222325ULL;
	auto mix = [&](const void *p, size_t n) {
		for (size_t i = 0; i < n; ++i)
			h = (h ^ ((const u8 *)p)[i]) * 0x100000001B3ULL;
	};
	mix(src.data(), src.size());
	mix(entry, strlen(entry));
	const u32 extra[2] = { doubles ? 1u : 0u, (u32)D3D_COMPILER_VERSION };
	mix(extra, sizeof(extra));
	char name[64];
	snprintf(name, sizeof(name), "%s-%016llx.cso", entry, (unsigned long long)h);
	return apppaths::CacheRoot() / "shaders" / name;
}

// The geometry shaders from the cache, or with compile, compiled (and cached) when the cache doesn't
// have them or they don't load. True when both exist.
bool D3D11Backend::CreateGeometryShaders(bool compile) {
	const std::string src = ShaderSource(ShaderKind::Geometry);
	const D3D_SHADER_MACRO defines[] = { { "DOUBLES", geoDoubles_ ? "1" : "0" }, { nullptr, nullptr } };
	auto make = [&](const char *entry, ComPtr<ID3D11ComputeShader> *cs) {
		const fs::path path = GeometryShaderCache(entry, src, geoDoubles_);
		std::vector<u8> code;
		std::error_code ec;
		if (fs::exists(path, ec) && ReadWholeFile(path, code) && !code.empty() &&
			SUCCEEDED(dev_->CreateComputeShader(code.data(), code.size(), nullptr, cs->ReleaseAndGetAddressOf())))
			return true;
		if (!compile)
			return false;
		ComPtr<ID3DBlob> blob = Compile(ShaderKind::Geometry, entry, "cs_5_0", defines, nullptr);
		if (!blob || FAILED(dev_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, cs->ReleaseAndGetAddressOf())))
			return false;
		// Written whole under a name of this process's, then renamed: other instances may do the same.
		fs::create_directories(path.parent_path(), ec);
		const fs::path tmp = path.string() + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
		if (WriteWholeFile(tmp, blob->GetBufferPointer(), blob->GetBufferSize()))
			fs::rename(tmp, path, ec);
		fs::remove(tmp, ec);
		return true;
	};
	bool vertexOk = false;
	// Both at once: the vertex shader takes several seconds to compile.
	std::thread other([&] { vertexOk = make("CSVertex", &csGeoVertex_); });
	const bool triangleOk = make("CSTriangle", &csGeoTriangle_);
	other.join();
	return vertexOk && triangleOk;
}

void D3D11Backend::InitGeometry(const D3D11Options &o) {
	const char *env = getenv("PSPC_GPU_TRANSFORM");
	if (!o.gpuTransform || (env && *env == '0'))
		return;
	geoVerify_ = o.verifyTransform || getenv("PSPC_GPU_TRANSFORM_VERIFY") != nullptr;
	// The texture-level gradient and reciprocal w behind the camera are double divisions on the CPU.
	D3D11_FEATURE_DATA_DOUBLES dbl{};
	D3D11_FEATURE_DATA_D3D11_OPTIONS o1{};
	geoDoubles_ = SUCCEEDED(dev_->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &dbl, sizeof(dbl))) && dbl.DoublePrecisionFloatShaderOps &&
		SUCCEEDED(dev_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o1, sizeof(o1))) && o1.ExtendedDoublesShaderInstructions;
	D3D11_BUFFER_DESC cbd{};
	cbd.ByteWidth = sizeof(GeoCB);
	cbd.Usage = D3D11_USAGE_DEFAULT;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(dev_->CreateBuffer(&cbd, nullptr, &geoCB_)))
		return;
	auto ready = [this](double ms) {
		LOGI("d3d11", "vertex transform on the GPU%s%s%s", geoDoubles_ ? "" : " (without double precision)",
		     ms >= 0 ? (" (shaders compiled in " + std::to_string((int)ms) + " ms)").c_str() : "", geoVerify_ ? ", verified against the CPU" : "");
		geoEnabled_.store(true, std::memory_order_release);
	};
	if (CreateGeometryShaders(false)) {
		ready(-1);
		return;
	}
	// Not compiled yet: the CPU transforms until the shaders are ready (PSPC_GPU_TRANSFORM_WAIT: wait).
	auto compile = [this, ready] {
		const auto t0 = std::chrono::steady_clock::now();
		if (CreateGeometryShaders(true))
			ready(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		else
			LOGW("d3d11", "vertex transform shaders unavailable; transforming on the CPU");
	};
	if (o.waitForShaders || getenv("PSPC_GPU_TRANSFORM_WAIT"))
		compile();
	else
		geoCompile_ = std::thread(compile);
}

bool D3D11Backend::UploadGeo(GeoBuffer &b, const void *data, size_t count, size_t stride) {
	const size_t need = std::max<size_t>(count, 1) * stride;
	if (b.bytes < need) {
		// A whole number of elements (structured buffers need that).
		size_t cap = std::max<size_t>(b.bytes * 2, 64 * 1024);
		while (cap < need)
			cap *= 2;
		cap = (cap + stride - 1) / stride * stride;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (UINT)cap;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = (UINT)stride;
		b = GeoBuffer{};
		if (FAILED(dev_->CreateBuffer(&bd, nullptr, &b.buf)) || FAILED(dev_->CreateShaderResourceView(b.buf.Get(), nullptr, &b.srv)))
			return false;
		b.bytes = cap;
	}
	if (count == 0)
		return true;
	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(ctx_->Map(b.buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return false;
	memcpy(m.pData, data, count * stride);
	ctx_->Unmap(b.buf.Get(), 0);
	return true;
}

bool D3D11Backend::RunGeometry(const GeometryBatch &g) {
	constexpr size_t CLIP_STRIDE = 56;  // ClipV in the HLSL
	const size_t nIn = g.inputs.size(), nJobs = g.jobs.size(), nOut = nJobs * GEO_JOB_VERTS;
	// A batch split while its draws were assembled shares their inputs with the batch before: only those
	// from the first its jobs use are transformed again.
	u32 firstIn = (u32)nIn;
	for (const GeJob &j : g.jobs)
		firstIn = std::min({ firstIn, j.v[0], j.v[1], j.v[2] });
	if (!UploadGeo(geoInputs_, g.inputs.data(), nIn, sizeof(GeVertexIn)) || !UploadGeo(geoDraws_, g.draws.data(), g.draws.size(), sizeof(GeDraw)) ||
		!UploadGeo(geoTransforms_, g.transforms.data(), g.transforms.size(), sizeof(GeTransform)) ||
		!UploadGeo(geoJobs_, g.jobs.data(), nJobs, sizeof(GeJob)))
		return false;
	if (geoClip_.bytes < nIn * CLIP_STRIDE) {
		size_t cap = std::max<size_t>(geoClip_.bytes * 2, 64 * 1024);
		while (cap < nIn * CLIP_STRIDE)
			cap *= 2;
		cap = (cap / CLIP_STRIDE) * CLIP_STRIDE;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (UINT)cap;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = (UINT)CLIP_STRIDE;
		geoClip_ = GeoBuffer{};
		if (FAILED(dev_->CreateBuffer(&bd, nullptr, &geoClip_.buf)) || FAILED(dev_->CreateShaderResourceView(geoClip_.buf.Get(), nullptr, &geoClip_.srv)) ||
			FAILED(dev_->CreateUnorderedAccessView(geoClip_.buf.Get(), nullptr, &geoClip_.uav)))
			return false;
		geoClip_.bytes = cap;
	}
	if (geoOut_.bytes < nOut * sizeof(GpuVertex)) {
		size_t cap = std::max<size_t>(geoOut_.bytes * 2, 1 << 20);
		while (cap < nOut * sizeof(GpuVertex))
			cap *= 2;
		cap &= ~(size_t)3;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (UINT)cap;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_VERTEX_BUFFER;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		geoOut_ = GeoBuffer{};
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = DXGI_FORMAT_R32_TYPELESS;
		ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		ud.Buffer.NumElements = (UINT)(cap / 4);
		ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(dev_->CreateBuffer(&bd, nullptr, &geoOut_.buf)) || FAILED(dev_->CreateUnorderedAccessView(geoOut_.buf.Get(), &ud, &geoOut_.uav)))
			return false;
		geoOut_.bytes = cap;
		omDirty_ = true;  // the vertex buffer binding goes with the old buffer
	}
	// The vertex buffer can't stay bound while the shader writes it.
	ID3D11Buffer *nullVb = nullptr;
	const UINT zero = 0;
	ctx_->IASetVertexBuffers(0, 1, &nullVb, &zero, &zero);
	omDirty_ = true;

	ID3D11ShaderResourceView *nullSrv[5] = {};
	ID3D11UnorderedAccessView *nullUav[2] = {};
	GeoCB cb{};
	cb.count = (u32)nIn - firstIn;
	cb.scale = (u32)scale_;
	cb.base = firstIn;
	ctx_->UpdateSubresource(geoCB_.Get(), 0, nullptr, &cb, 0, 0);
	ctx_->CSSetConstantBuffers(0, 1, geoCB_.GetAddressOf());
	if (cb.count != 0) {
		ID3D11ShaderResourceView *srvs[3] = { geoInputs_.srv.Get(), geoDraws_.srv.Get(), geoTransforms_.srv.Get() };
		ctx_->CSSetShader(csGeoVertex_.Get(), nullptr, 0);
		ctx_->CSSetShaderResources(0, 3, srvs);
		ctx_->CSSetUnorderedAccessViews(0, 1, geoClip_.uav.GetAddressOf(), nullptr);
		ctx_->Dispatch((cb.count + 63) / 64, 1, 1);
		ctx_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
	}
	cb.count = (u32)nJobs;
	cb.base = 0;
	ctx_->UpdateSubresource(geoCB_.Get(), 0, nullptr, &cb, 0, 0);
	ID3D11ShaderResourceView *srvs[5] = { nullptr, geoDraws_.srv.Get(), geoTransforms_.srv.Get(), geoJobs_.srv.Get(), geoClip_.srv.Get() };
	ctx_->CSSetShader(csGeoTriangle_.Get(), nullptr, 0);
	ctx_->CSSetShaderResources(0, 5, srvs);
	ctx_->CSSetUnorderedAccessViews(1, 1, geoOut_.uav.GetAddressOf(), nullptr);
	ctx_->Dispatch((UINT)((nJobs + 63) / 64), 1, 1);
	ctx_->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
	ctx_->CSSetShaderResources(0, 5, nullSrv);
	geoJobs += nJobs;
	geoVerts += nIn - firstIn;
	return true;
}

// PSPC_GPU_TRANSFORM_VERIFY: the GPU's vertices against what transform.cpp and EmitTriangle make of the
// same jobs (GeometryBatch::expect), bit for bit.
void D3D11Backend::VerifyGeometry(const GeometryBatch &g, const RasterState *states) {
	const size_t bytes = g.jobs.size() * GEO_JOB_VERTS * sizeof(GpuVertex);
	if (bytes == 0)
		return;
	if (geoStagingBytes_ < bytes) {
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (UINT)bytes;
		bd.Usage = D3D11_USAGE_STAGING;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		geoStaging_.Reset();
		if (FAILED(dev_->CreateBuffer(&bd, nullptr, &geoStaging_)))
			return;
		geoStagingBytes_ = bytes;
	}
	D3D11_BOX box{ 0, 0, 0, (UINT)bytes, 1, 1 };
	ctx_->CopySubresourceRegion(geoStaging_.Get(), 0, 0, 0, 0, geoOut_.buf.Get(), 0, &box);
	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(ctx_->Map(geoStaging_.Get(), 0, D3D11_MAP_READ, 0, &m)))
		return;
	const GpuVertex *gpu = (const GpuVertex *)m.pData;
	// EmitTriangle appends to verts_, which holds the batch's own vertices: work past them.
	const size_t keep = verts_.size();
	static const char *const names[11] = { "x", "y", "z", "fog", "c0", "c1", "s", "t", "q", "state", "lod" };
	u64 fieldMismatches[11] = {};
	u64 bad = 0;
	for (size_t j = 0; j < g.jobs.size() && j < g.expectFirst.size(); ++j) {
		const GeJob &job = g.jobs[j];
		const GeDraw &d = g.draws[job.draw];
		const u32 e0 = g.expectFirst[j], e1 = j + 1 < g.expectFirst.size() ? g.expectFirst[j + 1] : (u32)g.expect.size();
		// The CPU's triangles, made into GPU vertices, against the GPU's in order (a triangle the GPU
		// leaves out is degenerate: every vertex at 0, 0).
		std::vector<GpuVertex> want, got;
		for (u32 e = e0; e + 3 <= e1; e += 3) {
			BatchPrim p{};
			p.type = PrimType::Triangle;
			p.stateIndex = d.state;
			for (int k = 0; k < 3; ++k)
				p.v[k] = g.expect[e + k];
			EmitTriangle(p, states[d.state]);
			want.insert(want.end(), verts_.end() - 3, verts_.end());
			verts_.resize(keep);
		}
		for (u32 t = 0; t < GEO_JOB_VERTS / 3; ++t) {
			const GpuVertex *v = &gpu[job.out + t * 3];
			bool degenerate = true;
			for (int k = 0; k < 3; ++k)
				degenerate = degenerate && FloatBits(v[k].x) == 0 && FloatBits(v[k].y) == 0;
			if (!degenerate)
				got.insert(got.end(), v, v + 3);
		}
		bool jobBad = false;
		if (want.size() != got.size()) {
			if (geoMismatches < 20)
				LOGW("d3d11", "GPU transform: job %zu: GPU made %zu triangles, CPU %zu", j, got.size() / 3, want.size() / 3);
			jobBad = true;
		}
		for (size_t k = 0; k < want.size() && k < got.size(); ++k) {
			const u32 *a = (const u32 *)&got[k], *b = (const u32 *)&want[k];
			for (int f = 0; f < 11; ++f) {
				if (a[f] != b[f]) {
					fieldMismatches[f]++;
					if (!jobBad && geoMismatches < 20)
						LOGW("d3d11", "GPU transform: job %zu vertex %zu %s: GPU %08x, CPU %08x", j, k, names[f], a[f], b[f]);
					jobBad = true;
				}
			}
		}
		if (jobBad) {
			bad++;
			geoMismatches++;
		}
		geoChecked++;
	}
	ctx_->Unmap(geoStaging_.Get(), 0);
	if (bad) {
		std::string fields;
		for (int f = 0; f < 11; ++f) {
			if (fieldMismatches[f])
				fields += std::string(fields.empty() ? "" : ", ") + names[f] + " " + std::to_string(fieldMismatches[f]);
		}
		LOGW("d3d11", "GPU transform: %llu of %zu jobs differ (%s); %llu of %llu so far", (unsigned long long)bad, g.jobs.size(), fields.c_str(),
		     (unsigned long long)geoMismatches, (unsigned long long)geoChecked);
	}
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
	BeginGpuSample(5);
	ctx_->Draw(3, 0);
	EndGpuSample();
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

D3D11TransformStats GetD3D11TransformStats(RenderBackend *backend) {
	auto *b = dynamic_cast<D3D11Backend *>(backend);
	return b ? b->TransformStats() : D3D11TransformStats{};
}

}  // namespace gpu::hw
