// SPDX-License-Identifier: GPL-2.0-or-later
//
// Queues primitives with a snapshot of the raster state, and draws them in order when flushed. A large
// batch is split into horizontal bands drawn by worker threads: each band draws every primitive of the
// batch that reaches it, in order, so every pixel sees the primitives in order and the result doesn't
// depend on the thread count. Batches where that wouldn't hold (texturing from the target, depth and
// color buffers sharing memory, pixels past the stride) draw on one thread.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <xmmintrin.h>

#include "gpu/backend.h"
#include "gpu/ge_state.h"
#include "gpu/render.h"

namespace gpu {

InternalStats g_stats;

namespace {

// --- Worker pool ---

class Pool {
public:
	void Start(int workers) {
		Stop();
		quit_ = false;
		for (int i = 0; i < workers; ++i)
			threads_.emplace_back([this] { WorkerLoop(); });
	}

	void Stop() {
		{
			std::lock_guard<std::mutex> lk(mu_);
			quit_ = true;
		}
		cv_.notify_all();
		for (auto &t : threads_)
			t.join();
		threads_.clear();
	}

	int Workers() const { return (int)threads_.size(); }

	// Runs fn(0..count-1), on the calling thread too; returns when all are done.
	void ParallelFor(int count, const std::function<void(int)> &fn) {
		auto job = std::make_shared<Job>();
		job->fn = &fn;
		job->count = count;
		{
			std::lock_guard<std::mutex> lk(mu_);
			current_ = job;
			gen_.fetch_add(1, std::memory_order_release);
		}
		cv_.notify_all();
		RunTasks(*job);
		int spins = 0;
		while (job->done.load(std::memory_order_acquire) < count) {
			if (++spins < 20000) {
				Pause();
				continue;
			}
			std::unique_lock<std::mutex> lk(mu_);
			doneCv_.wait_for(lk, std::chrono::microseconds(200), [&] { return job->done.load() >= count; });
		}
	}

private:
	struct Job {
		const std::function<void(int)> *fn = nullptr;
		int count = 0;
		std::atomic<int> next{ 0 };
		std::atomic<int> done{ 0 };
	};

	static void Pause() {
#if defined(_MSC_VER)
		_mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
		__builtin_ia32_pause();
#endif
	}

	void RunTasks(Job &job) {
		for (;;) {
			const int i = job.next.fetch_add(1, std::memory_order_relaxed);
			if (i >= job.count)
				break;
			(*job.fn)(i);
			if (job.done.fetch_add(1, std::memory_order_acq_rel) + 1 == job.count) {
				std::lock_guard<std::mutex> lk(mu_);
				doneCv_.notify_all();
			}
		}
	}

	void WorkerLoop() {
		u64 seen = gen_.load();
		for (;;) {
			// Spin a little first: flushes come in quick succession.
			for (int i = 0; i < 4000 && gen_.load(std::memory_order_acquire) == seen && !quit_; ++i)
				Pause();
			std::shared_ptr<Job> job;
			{
				std::unique_lock<std::mutex> lk(mu_);
				cv_.wait(lk, [&] { return quit_ || gen_.load() != seen; });
				if (quit_)
					return;
				seen = gen_.load();
				job = current_;
			}
			if (job)
				RunTasks(*job);
		}
	}

	std::vector<std::thread> threads_;
	std::mutex mu_;
	std::condition_variable cv_, doneCv_;
	std::shared_ptr<Job> current_;
	std::atomic<u64> gen_{ 0 };
	std::atomic<bool> quit_{ false };
};

// Never destroyed: a std::thread still joinable at static destruction calls std::terminate, and
// the CRT may tear down while workers wait. binner::Shutdown (gpu::Shutdown) joins them; without it
// ExitProcess just ends the idle workers.
Pool &pool = *new Pool;
int requestedThreads = 0;

using Prim = BatchPrim;

// The installed hardware backend (backend.h); nullptr draws with the software rasterizer below.
RenderBackend *g_backend = nullptr;

struct AddrRange {
	u32 start, end;  // folded guest addresses, [start, end)
};

constexpr size_t MAX_PRIMS = 16384;
constexpr int BAND_ROWS = 16;
constexpr u64 THREAD_MIN_PIXELS = 24 * 1024;

std::vector<Prim> prims;
std::vector<RasterState> states;
std::vector<AddrRange> readRanges;
RasterState current;
bool currentValid = false;   // current is up to date with gstate
int currentIndex = -1;       // current's index in states, -1 if not queued yet
u64 currentGen = 0;          // counts the recomputations of current
u32 batchEpoch = 0;          // counts the batches (ResetBatch)
// The texture-cache copy current samples (UseTexCache), if any.
std::shared_ptr<std::vector<u8>> currentTexCopy;

// Snapshots of current for recorded draws (SnapshotState), and where each went in states.
struct PendingState {
	RasterState st;
	std::shared_ptr<std::vector<u8>> texCopy;
	u64 gen;
	int index;   // in states, valid in batch epoch
	u32 epoch;
};
std::vector<PendingState> pendingStates;
bool resolving = false;      // pushes come from pendingStates[pushSnapshot]
u32 pushSnapshot = 0;
// Where the recorded draws can draw: their target and the rows of their scissors.
struct PendingTarget {
	bool mixed;              // more than one target
	u32 fbOffset, zbOffset;
	int fbStride, zbStride;
	u8 fbFormat;
	int minRow, maxRow;
	bool depth;
} pendingTarget;
bool batchUsesDepth = false;
bool batchSingleThread = false;
int batchMaxRow = -1;
int batchMinRow = 1 << 30;
u64 batchPixels = 0;

// CLUT snapshots (2 KB: the 1 KB palette twice, for index wrapping).
std::vector<std::unique_ptr<u8[]>> clutPool;
int clutCount = 0;
bool clutInUse = false;  // the latest snapshot is referenced by a queued state

// --- Texture cache ---
//
// The GE loads a texture into its 8 KB cache when a primitive first samples it and serves it from
// there until TEXFLUSH. That only shows when a primitive textures from what the batch draws
// (gpu/exact/pixel exp174): such a texture, if it fits, is drawn from a copy taken when it was first
// sampled. Bigger ones, and textures elsewhere, are read from memory as they are drawn.
constexpr u32 TEX_CACHE_BYTES = 8192;

struct TexCacheEntry {
	u32 addr[8] = {}, bytes[8] = {};
	int levels = 0;
	u32 flushGen = 0;
	std::shared_ptr<std::vector<u8>> data;
};

u32 texFlushGen = 0;
TexCacheEntry texCache;  // valid while data is set
std::vector<std::shared_ptr<std::vector<u8>>> batchTexCopies;  // kept until the batch is drawn

u8 *LatestClut() {
	if (clutCount == 0) {
		if (clutPool.empty())
			clutPool.emplace_back(new u8[2048]());
		clutCount = 1;
	}
	return clutPool[clutCount - 1].get();
}

void UseTexCache(RasterState *st);

void EnsureCurrent() {
	if (!currentValid) {
		ComputeRasterState(&current, LatestClut());
		currentValid = true;
		currentIndex = -1;
		currentGen++;
		UseTexCache(&current);
	}
}

void AddTextureReads(const RasterState &st) {
	if (!st.enableTextures)
		return;
	static const u8 bits[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };
	const SamplerState &ss = st.samp;
	for (int i = 0; i <= ss.maxLevel; ++i) {
		const TexLevel &lv = ss.levels[i];
		if (!lv.ptr)
			continue;
		const u32 start = FoldAddress(lv.addr);
		const u32 h = std::min<u32>(lv.h, 512);
		const u32 rows = ss.swizzle ? ((h + 7) & ~7u) : h;
		const u32 bytes = std::min<u32>((u32)((u64)rows * lv.bufw * bits[ss.texfmt] / 8), lv.validBytes);
		AddrRange r{ start, start + bytes };
		if (!readRanges.empty() && readRanges.back().start == r.start && readRanges.back().end == r.end)
			continue;
		readRanges.push_back(r);
	}
}

// Adds st to the batch's states (a copy of the texture it samples from the cache, texCopy, stays until
// the batch is drawn).
int AddState(const RasterState &st, const std::shared_ptr<std::vector<u8>> &texCopy) {
	states.push_back(st);
	clutInUse = true;
	if (texCopy)
		batchTexCopies.push_back(texCopy);
	AddTextureReads(st);
	const PixelState &ps = st.pix;
	if (ps.depthWrite || ps.depthTestFunc != GE_COMP_ALWAYS || (ps.clearMode && ps.depthClear))
		batchUsesDepth = true;
	return (int)states.size() - 1;
}

u32 PushCurrent() {
	EnsureCurrent();
	if (currentIndex < 0)
		currentIndex = AddState(current, currentTexCopy);
	return (u32)currentIndex;
}

u32 PushSnapshot(u32 id) {
	PendingState &p = pendingStates[id];
	if (p.index < 0 || p.epoch != batchEpoch) {
		p.index = AddState(p.st, p.texCopy);
		p.epoch = batchEpoch;
	}
	return (u32)p.index;
}

inline bool Overlaps(const AddrRange &a, const AddrRange &b);

// The texture's bytes per level, as AddTextureReads counts them.
u32 TextureLevelBytes(const SamplerState &ss, const TexLevel &lv) {
	static const u8 bits[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };
	const u32 h = std::min<u32>(lv.h, 512);
	const u32 rows = ss.swizzle ? ((h + 7) & ~7u) : h;
	return std::min<u32>((u32)((u64)rows * lv.bufw * bits[ss.texfmt] / 8) + 16, lv.validBytes);
}

// Whether st textures from the color or depth buffer it draws to (rows up to the scissor).
bool TexturesFromTarget(const RasterState &st) {
	const PixelState &ps = st.pix;
	const u32 rows = (u32)std::max(st.scissor.y2 / SUBPIX + 1, 0);
	const u32 bpp = ps.fbFormat == GE_FORMAT_8888 ? 4 : 2;
	const AddrRange color{ VRAM_BASE + ps.fbOffset, VRAM_BASE + ps.fbOffset + rows * ps.fbStride * bpp };
	const bool depth = ps.depthWrite || ps.depthTestFunc != GE_COMP_ALWAYS;
	const AddrRange zbuf{ VRAM_BASE + ps.zbOffset, VRAM_BASE + ps.zbOffset + rows * ps.zbStride * 2 };
	const SamplerState &ss = st.samp;
	for (int i = 0; i <= ss.maxLevel; ++i) {
		const TexLevel &lv = ss.levels[i];
		if (!lv.ptr)
			continue;
		const u32 start = FoldAddress(lv.addr);
		const AddrRange r{ start, start + TextureLevelBytes(ss, lv) };
		if (Overlaps(r, color) || (depth && Overlaps(r, zbuf)))
			return true;
	}
	return false;
}

void UseTexCache(RasterState *st) {
	currentTexCopy.reset();
	// The D3D11 renderer keeps textures its own way.
	if (g_backend)
		return;
	if (!st->enableTextures || !TexturesFromTarget(*st)) {
		texCache.data.reset();
		return;
	}
	SamplerState &ss = st->samp;
	TexCacheEntry key;
	key.levels = ss.maxLevel + 1;
	u32 total = 0;
	for (int i = 0; i < key.levels; ++i) {
		const TexLevel &lv = ss.levels[i];
		key.addr[i] = lv.addr;
		key.bytes[i] = lv.ptr ? TextureLevelBytes(ss, lv) : 0;
		total += key.bytes[i];
	}
	if (total > TEX_CACHE_BYTES) {
		texCache.data.reset();
		return;
	}
	const bool hit = texCache.data && texCache.flushGen == texFlushGen && texCache.levels == key.levels &&
		std::equal(key.addr, key.addr + key.levels, texCache.addr) && std::equal(key.bytes, key.bytes + key.levels, texCache.bytes);
	if (!hit) {
		// The copy must see everything drawn before it.
		ResolvePendingDraws();
		if (!prims.empty()) {
			binner::Flush();
			currentValid = true;
			currentIndex = -1;
		}
		key.flushGen = texFlushGen;
		key.data = std::make_shared<std::vector<u8>>(total);
		u32 off = 0;
		for (int i = 0; i < key.levels; ++i) {
			if (key.bytes[i])
				memcpy(key.data->data() + off, ss.levels[i].ptr, key.bytes[i]);
			off += key.bytes[i];
		}
		texCache = key;
	}
	batchTexCopies.push_back(texCache.data);
	currentTexCopy = texCache.data;
	u32 off = 0;
	for (int i = 0; i < key.levels; ++i) {
		TexLevel &lv = ss.levels[i];
		if (lv.ptr) {
			lv.ptr = texCache.data->data() + off;
			lv.validBytes = texCache.bytes[i];
			lv.bounded = true;
		}
		off += texCache.bytes[i];
	}
}

// The VRAM byte ranges the batch draws to.
AddrRange FramebufferWriteRange() {
	if (states.empty() || batchMaxRow < 0)
		return { 0, 0 };
	const PixelState &ps = states[0].pix;
	const u32 bpp = ps.fbFormat == GE_FORMAT_8888 ? 4 : 2;
	const u32 start = VRAM_BASE + ps.fbOffset + (u32)batchMinRow * ps.fbStride * bpp;
	// Pixels past the stride make the batch single-threaded, so rows are exact.
	const u32 end = VRAM_BASE + ps.fbOffset + (u32)(batchMaxRow + 1) * ps.fbStride * bpp;
	return { start, end };
}

AddrRange DepthWriteRange() {
	if (states.empty() || batchMaxRow < 0 || !batchUsesDepth)
		return { 0, 0 };
	const PixelState &ps = states[0].pix;
	const u32 start = VRAM_BASE + ps.zbOffset + (u32)batchMinRow * ps.zbStride * 2;
	const u32 end = VRAM_BASE + ps.zbOffset + (u32)(batchMaxRow + 1) * ps.zbStride * 2;
	return { start, end };
}

inline bool Overlaps(const AddrRange &a, const AddrRange &b) {
	return a.start < b.end && b.start < a.end && a.end > a.start && b.end > b.start;
}

inline bool UsesDepth(const PixelState &ps) {
	return ps.depthWrite || ps.depthTestFunc != GE_COMP_ALWAYS || (ps.clearMode && ps.depthClear);
}

inline bool SameTarget(const PixelState &ps, const PendingTarget &t) {
	return ps.fbOffset == t.fbOffset && ps.zbOffset == t.zbOffset && ps.fbStride == t.fbStride && ps.zbStride == t.zbStride &&
	       ps.fbFormat == t.fbFormat;
}

void AddPendingTarget(const RasterState &st) {
	const PixelState &ps = st.pix;
	PendingTarget &t = pendingTarget;
	const int row1 = std::max(st.scissor.y1 / SUBPIX, 0), row2 = st.scissor.y2 / SUBPIX;
	if (pendingStates.empty()) {
		t.mixed = false;
		t.fbOffset = ps.fbOffset;
		t.zbOffset = ps.zbOffset;
		t.fbStride = ps.fbStride;
		t.zbStride = ps.zbStride;
		t.fbFormat = ps.fbFormat;
		t.minRow = row1;
		t.maxRow = row2;
		t.depth = false;
	} else if (!SameTarget(ps, t)) {
		t.mixed = true;
	}
	t.minRow = std::min(t.minRow, row1);
	t.maxRow = std::max(t.maxRow, row2);
	t.depth = t.depth || UsesDepth(ps);
}

// Whether assembling the recorded draws could make the queue write [r.start, r.end): a superset of the
// rows FramebufferWriteRange and DepthWriteRange will have.
bool PendingMayWrite(const AddrRange &r) {
	if (pendingStates.empty())
		return false;
	const PendingTarget &t = pendingTarget;
	if (t.mixed || (!states.empty() && !SameTarget(states[0].pix, t)))
		return true;
	int minRow = t.minRow, maxRow = t.maxRow;
	if (batchMaxRow >= 0) {
		minRow = std::min(minRow, batchMinRow);
		maxRow = std::max(maxRow, batchMaxRow);
	}
	if (maxRow < minRow)
		return false;
	const u32 bpp = t.fbFormat == GE_FORMAT_8888 ? 4 : 2;
	const AddrRange fb{ VRAM_BASE + t.fbOffset + (u32)minRow * t.fbStride * bpp, VRAM_BASE + t.fbOffset + (u32)(maxRow + 1) * t.fbStride * bpp };
	if (Overlaps(r, fb))
		return true;
	const AddrRange zb{ VRAM_BASE + t.zbOffset + (u32)minRow * t.zbStride * 2, VRAM_BASE + t.zbOffset + (u32)(maxRow + 1) * t.zbStride * 2 };
	return (batchUsesDepth || t.depth) && Overlaps(r, zb);
}

bool AnyReadOverlap(const AddrRange &w) {
	for (const AddrRange &r : readRanges) {
		if (Overlaps(r, w))
			return true;
	}
	return false;
}

inline Range Intersect(const Range &a, const Range &b) {
	return { std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2) };
}

void Push(PrimType type, const Range &rawRange, const RVertex *v, int nv) {
	const u32 idx = resolving ? PushSnapshot(pushSnapshot) : PushCurrent();
	const RasterState &st = states[idx];
	const Range range = Intersect(rawRange, st.scissor);
	if (range.Invalid())
		return;
	Prim p;
	p.type = type;
	p.stateIndex = idx;
	p.range = range;
	for (int i = 0; i < nv; ++i)
		p.v[i] = v[i];
	prims.push_back(p);
	const int row1 = range.y1 / SUBPIX, row2 = range.y2 / SUBPIX;
	batchMinRow = std::min(batchMinRow, row1);
	batchMaxRow = std::max(batchMaxRow, row2);
	const u64 area = (u64)(range.x2 - range.x1 + 1) / SUBPIX * (u64)(range.y2 - range.y1 + 1) / SUBPIX;
	batchPixels += area;
	g_stats.prims++;
	g_stats.pixels += area;
	// Pixels past the stride land in the next row, which another band may be drawing.
	const PixelState &ps = st.pix;
	const bool usesDepth = ps.depthWrite || ps.depthTestFunc != GE_COMP_ALWAYS || (ps.clearMode && ps.depthClear);
	if (range.x2 / SUBPIX >= ps.fbStride || (usesDepth && range.x2 / SUBPIX >= ps.zbStride))
		batchSingleThread = true;
	if (prims.size() >= MAX_PRIMS)
		binner::Flush();
}

Range RangeOf(const RVertex *v, int n) {
	Range r{ v[0].x, v[0].y, v[0].x, v[0].y };
	for (int i = 1; i < n; ++i) {
		r.x1 = std::min(r.x1, v[i].x);
		r.y1 = std::min(r.y1, v[i].y);
		r.x2 = std::max(r.x2, v[i].x);
		r.y2 = std::max(r.y2, v[i].y);
	}
	r.x1 &= ~(SUBPIX - 1);
	r.y1 &= ~(SUBPIX - 1);
	r.x2 |= SUBPIX - 1;
	r.y2 |= SUBPIX - 1;
	return r;
}

void DrawPrim(const Prim &p, const Range &r, const RasterState &st) {
	switch (p.type) {
	case PrimType::Triangle: DrawTriangle(p.v[0], p.v[1], p.v[2], r, st); break;
	case PrimType::Rect: DrawRectangle(p.v[0], p.v[1], r, st); break;
	case PrimType::ClearRect: ClearRectangle(p.v[0], p.v[1], r, st); break;
	case PrimType::Line: DrawLine(p.v[0], p.v[1], r, st); break;
	case PrimType::Point: DrawPoint(p.v[0], r, st); break;
	}
}

void DrawBand(int band) {
	const int row1 = batchMinRow + band * BAND_ROWS;
	const int row2 = std::min(row1 + BAND_ROWS - 1, batchMaxRow);
	const int y1 = row1 * SUBPIX, y2 = row2 * SUBPIX + SUBPIX - 1;
	for (const Prim &p : prims) {
		if (p.range.y2 < y1 || p.range.y1 > y2)
			continue;
		Range r = p.range;
		r.y1 = std::max(r.y1, y1);
		r.y2 = std::min(r.y2, y2);
		DrawPrim(p, r, states[p.stateIndex]);
	}
}

void ResetBatch() {
	prims.clear();
	states.clear();
	readRanges.clear();
	batchTexCopies.clear();
	currentIndex = -1;
	batchUsesDepth = false;
	batchSingleThread = false;
	batchMaxRow = -1;
	batchMinRow = 1 << 30;
	batchPixels = 0;
	batchEpoch++;
	// Keep only the latest CLUT snapshot (not while recorded draws still point at the others).
	if (clutCount > 1 && !resolving) {
		std::swap(clutPool[0], clutPool[clutCount - 1]);
		clutCount = 1;
		currentValid = false;
	}
	clutInUse = false;
}

}  // namespace

namespace binner {

void Init() {
	prims.reserve(1024);
	states.reserve(256);
	clutPool.clear();
	clutCount = 0;
	currentValid = false;
	ResetBatch();
	SetThreads(requestedThreads);
}

void Shutdown() {
	pool.Stop();
	prims.clear();
	states.clear();
	pendingStates.clear();
}

void SetThreads(int count) {
	requestedThreads = count;
	int n = count;
	if (n <= 0) {
		const int hw = (int)std::thread::hardware_concurrency();
		n = std::clamp(hw - 2, 1, 12);
	}
	if (pool.Workers() != n - 1)
		pool.Start(n - 1);
}

void DirtyState() {
	currentValid = false;
}

void TargetChanged() {
	ResolvePendingDraws();
	if (!prims.empty())
		Flush();
	currentValid = false;
}

void TexFlush() {
	texFlushGen++;
	currentValid = false;
}

void UpdateClut(const u8 *src) {
	u8 *latest = LatestClut();
	if (memcmp(latest, src, 1024) == 0)
		return;
	if (clutInUse) {
		if ((int)clutPool.size() <= clutCount)
			clutPool.emplace_back(new u8[2048]());
		latest = clutPool[clutCount++].get();
		clutInUse = false;
	}
	memcpy(latest, src, 1024);
	memcpy(latest + 1024, src, 1024);
	currentValid = false;
}

const RasterState &CurrentState() {
	EnsureCurrent();
	return current;
}

void AddTriangle(const RVertex &v0, const RVertex &v1, const RVertex &v2) {
	// Drop triangles not in CCW order, and ones with zero area (gpu/probe exp118, exp132).
	const int64_t d01x = (int64_t)v0.x - v1.x, d01y = (int64_t)v0.y - v1.y;
	const int64_t d02x = (int64_t)v0.x - v2.x, d02y = (int64_t)v0.y - v2.y;
	if (d01x * d02y - d01y * d02x <= 0)
		return;
	const RVertex v[3] = { v0, v1, v2 };
	Push(PrimType::Triangle, RangeOf(v, 3), v, 3);
}

void AddRect(const RVertex &v0, const RVertex &v1) {
	const RVertex v[2] = { v0, v1 };
	Push(PrimType::Rect, RangeOf(v, 2), v, 2);
}

void AddClearRect(const RVertex &v0, const RVertex &v1) {
	const RVertex v[2] = { v0, v1 };
	Push(PrimType::ClearRect, RangeOf(v, 2), v, 2);
}

void AddLine(const RVertex &v0, const RVertex &v1) {
	const RVertex v[2] = { v0, v1 };
	Push(PrimType::Line, RangeOf(v, 2), v, 2);
}

void AddPoint(const RVertex &v0) {
	Push(PrimType::Point, RangeOf(&v0, 1), &v0, 1);
}

bool HasQueued() {
	ResolvePendingDraws();
	return !prims.empty();
}

void Flush() {
	ResolvePendingDraws();
	if (prims.empty()) {
		ResetBatch();
		return;
	}
	g_stats.flushes++;
	if (g_backend) {
		g_backend->DrawBatch(prims.data(), prims.size(), states.data(), states.size());
		ResetBatch();
		return;
	}
	bool single = batchSingleThread || pool.Workers() == 0 || batchPixels < THREAD_MIN_PIXELS;
	if (!single) {
		// Texturing from what the batch draws, or depth sharing memory with color: keep it in order.
		const AddrRange fbw = FramebufferWriteRange();
		const AddrRange zbw = DepthWriteRange();
		if (AnyReadOverlap(fbw) || AnyReadOverlap(zbw) || Overlaps(fbw, zbw))
			single = true;
		// One target per batch (TargetChanged flushes), but check anyway.
		const PixelState &p0 = states[0].pix;
		for (const RasterState &st : states) {
			if (st.pix.fbOffset != p0.fbOffset || st.pix.zbOffset != p0.zbOffset || st.pix.fbStride != p0.fbStride ||
				st.pix.zbStride != p0.zbStride || st.pix.fbFormat != p0.fbFormat) {
				single = true;
				break;
			}
		}
	}
	if (single) {
		for (const Prim &p : prims)
			DrawPrim(p, p.range, states[p.stateIndex]);
	} else {
		g_stats.threadedFlushes++;
		const int bands = (batchMaxRow - batchMinRow) / BAND_ROWS + 1;
		static const std::function<void(int)> fn = [](int band) { DrawBand(band); };
		pool.ParallelFor(bands, fn);
	}
	ResetBatch();
}

void FlushIfOverlap(u32 addr, u32 size, bool modifying) {
	if (size == 0)
		return;
	const u32 start = FoldAddress(addr);
	const AddrRange r{ start, start + size };
	// Recorded draws wait unless they could change the answer (a write may also change their vertices).
	if (modifying || PendingMayWrite(r))
		ResolvePendingDraws();
	if (prims.empty())
		return;
	if (Overlaps(r, FramebufferWriteRange()) || Overlaps(r, DepthWriteRange())) {
		Flush();
		return;
	}
	if (modifying && AnyReadOverlap(r))
		Flush();
}

u32 SnapshotState() {
	EnsureCurrent();
	if (pendingStates.empty() || pendingStates.back().gen != currentGen) {
		AddPendingTarget(current);
		// A copy that's queued already stays the one the draw's primitives use.
		pendingStates.push_back({ current, currentTexCopy, currentGen, currentIndex, batchEpoch });
		// Later CLUT loads must not overwrite the snapshot the copy points to.
		clutInUse = true;
	}
	return (u32)pendingStates.size() - 1;
}

const RasterState &Snapshot(u32 id) {
	return pendingStates[id].st;
}

void BeginResolve() {
	resolving = true;
}

void UseSnapshot(u32 id) {
	pushSnapshot = id;
}

void EndResolve() {
	resolving = false;
	// current is queued where its last snapshot went, as if the draws had pushed it.
	if (!pendingStates.empty()) {
		const PendingState &last = pendingStates.back();
		if (currentValid && last.gen == currentGen)
			currentIndex = last.epoch == batchEpoch ? last.index : -1;
	}
	pendingStates.clear();
}

}  // namespace binner

void ParallelFor(int count, const std::function<void(int)> &fn) {
	if (count <= 0)
		return;
	if (count == 1 || pool.Workers() == 0) {
		for (int i = 0; i < count; ++i)
			fn(i);
		return;
	}
	// The same floating-point mode on every thread, so results don't depend on where they ran.
	const unsigned csr = _mm_getcsr();
	const std::function<void(int)> task = [&](int i) {
		const unsigned saved = _mm_getcsr();
		_mm_setcsr(csr);
		fn(i);
		_mm_setcsr(saved);
	};
	pool.ParallelFor(count, task);
}

int ParallelWorkers() {
	return pool.Workers();
}

void SetRenderBackend(RenderBackend *backend) {
	binner::Flush();
	g_backend = backend;
	binner::DirtyState();
}

RenderBackend *GetRenderBackend() {
	return g_backend;
}

}  // namespace gpu
