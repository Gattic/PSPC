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

struct Prim {
	PrimType type;
	u32 stateIndex;
	Range range;
	RVertex v[3];
};

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
bool batchUsesDepth = false;
bool batchSingleThread = false;
int batchMaxRow = -1;
int batchMinRow = 1 << 30;
u64 batchPixels = 0;

// CLUT snapshots (2 KB: the 1 KB palette twice, for index wrapping).
std::vector<std::unique_ptr<u8[]>> clutPool;
int clutCount = 0;
bool clutInUse = false;  // the latest snapshot is referenced by a queued state

u8 *LatestClut() {
	if (clutCount == 0) {
		if (clutPool.empty())
			clutPool.emplace_back(new u8[2048]());
		clutCount = 1;
	}
	return clutPool[clutCount - 1].get();
}

void EnsureCurrent() {
	if (!currentValid) {
		ComputeRasterState(&current, LatestClut());
		currentValid = true;
		currentIndex = -1;
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

u32 PushCurrent() {
	EnsureCurrent();
	if (currentIndex < 0) {
		states.push_back(current);
		currentIndex = (int)states.size() - 1;
		clutInUse = true;
		AddTextureReads(current);
		const PixelState &ps = current.pix;
		if (ps.depthWrite || ps.depthTestFunc != GE_COMP_ALWAYS || (ps.clearMode && ps.depthClear))
			batchUsesDepth = true;
	}
	return (u32)currentIndex;
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
	const u32 idx = PushCurrent();
	const Range range = Intersect(rawRange, current.scissor);
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
	const PixelState &ps = current.pix;
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
	currentIndex = -1;
	batchUsesDepth = false;
	batchSingleThread = false;
	batchMaxRow = -1;
	batchMinRow = 1 << 30;
	batchPixels = 0;
	// Keep only the latest CLUT snapshot.
	if (clutCount > 1) {
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
	if (!prims.empty())
		Flush();
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
	return !prims.empty();
}

void Flush() {
	if (prims.empty()) {
		ResetBatch();
		return;
	}
	g_stats.flushes++;
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
	if (prims.empty() || size == 0)
		return;
	const u32 start = FoldAddress(addr);
	const AddrRange r{ start, start + size };
	if (Overlaps(r, FramebufferWriteRange()) || Overlaps(r, DepthWriteRange())) {
		Flush();
		return;
	}
	if (modifying && AnyReadOverlap(r))
		Flush();
}

}  // namespace binner

}  // namespace gpu
