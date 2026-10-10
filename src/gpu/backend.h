// SPDX-License-Identifier: GPL-2.0-or-later
//
// The renderer backend seam, at the post-transform level. The GE core decodes display lists, the
// transform pipeline turns vertices into screen-space primitives (RVertex), and the binner queues
// them with a snapshot of the raster state. When the queue is flushed it goes to the installed
// backend; with none installed (the default), the built-in software rasterizer draws it into guest
// VRAM (binner.cpp), which stays the accuracy reference.
//
// A backend that keeps rendering results outside guest memory (a GPU) must keep guest memory
// coherent: it is told when the GE reads or writes memory on its own (block transfers, CLUT
// loads), when the guest synchronizes with the GE (list end, sceGeDrawSync/ListSync), and when
// sceDisplay shows a framebuffer.
#pragma once

#include <vector>

#include "common/common.h"
#include "gpu/render.h"

namespace gpu {

// One queued primitive. Clears are primitives too: PrimType::ClearRect (and any primitive whose
// state has pix.clearMode) writes color, stencil and depth as the clear flags say.
struct BatchPrim {
	PrimType type;
	u32 stateIndex;  // into the batch's states
	Range range;     // screen subpixels, already limited to the scissor
	RVertex v[3];
};

class RenderBackend {
public:
	virtual ~RenderBackend() = default;
	virtual const char *Name() const = 0;

	// The GE starts processing lists: guest memory may have been changed by the CPU since it last ran.
	virtual void BeginRun() {}
	// Draws the primitives in order. All of them have the same render target (the binner flushes when
	// the target changes); the states (and the CLUT snapshots they point to) are valid during the call.
	virtual void DrawBatch(const BatchPrim *prims, size_t primCount, const RasterState *states, size_t stateCount) = 0;
	// The GE is about to read (CLUT load, block transfer source) or write (block transfer destination)
	// guest memory [addr, addr + size) itself: guest memory there must hold the latest rendering.
	virtual void SyncRange(u32 addr, u32 size) { (void)addr; (void)size; }
	// The GE wrote guest memory [addr, addr + size) itself (block transfer).
	virtual void Invalidate(u32 addr, u32 size) { (void)addr; (void)size; }
	// A list finished, or the guest waits for the GE (sceGeDrawSync, sceGeListSync): write everything
	// rendered back to guest memory.
	virtual void Sync() {}
	// The end of a Run (the GE stalled or went idle): submit queued work.
	virtual void EndRun() {}
	// sceDisplay shows the framebuffer at addr (format 0-3 as GE_FORMAT_*). Makes guest memory current
	// for it; returns true with the image (RGBA8888, alpha 255, at the backend's own resolution) when the
	// backend has a better one than guest memory (upscaled rendering).
	virtual bool GetDisplayImage(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out, int *outW, int *outH) {
		(void)addr; (void)stride; (void)format; (void)width; (void)height; (void)out; (void)outW; (void)outH;
		return false;
	}
};

// Installs a backend (not owned; nullptr = the software rasterizer). Flushes what's queued first.
void SetRenderBackend(RenderBackend *backend);
RenderBackend *GetRenderBackend();

}  // namespace gpu

namespace gpu {

// sceDisplay's view of RenderBackend::GetDisplayImage (false without a backend).
inline bool GetBackendDisplayImage(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out, int *outW, int *outH) {
	RenderBackend *b = GetRenderBackend();
	return b && b->GetDisplayImage(addr, stride, format, width, height, out, outW, outH);
}

}  // namespace gpu
