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

#include <functional>
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
	// A list finished, or the guest waits for the GE (sceGeDrawSync, sceGeListSync): from now on the guest
	// must see everything rendered in guest memory (written back now, or at its first access to it).
	virtual void Sync() {}
	// The end of a Run (the GE stalled or went idle): submit queued work.
	virtual void EndRun() {}
	// sceDisplay shows the framebuffer at addr (format 0-3 as GE_FORMAT_*). Makes guest memory current
	// for it; returns true with the image (RGBA8888, alpha 255) when the backend has a better one than
	// guest memory (upscaled rendering): at outScale times width x height (at most the backend's own
	// scale; 0 = its own), reduced by averaging.
	virtual bool GetDisplayImage(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out, int *outW, int *outH,
	                             int outScale) {
		(void)addr; (void)stride; (void)format; (void)width; (void)height; (void)out; (void)outW; (void)outH; (void)outScale;
		return false;
	}
	// A backend that presents to the host window itself (a GPU's swap chain): sceDisplay shows the
	// framebuffer at addr. Presents the backend's own image of it, scaled to fit the window, and returns
	// true. overlay(rgba, w, h), if given, draws over it at about the window's size (RGBA8888, w x h a
	// multiple of 480x272): onto a transparent image that is laid over the picture (the dialogs and the HOME
	// menu), or, with `capture`, onto the picture itself read back (screenshots). False without a window or
	// an image of that framebuffer: the caller presents a CPU image instead.
	virtual bool PresentDisplay(u32 addr, int stride, int format, int width, int height,
	                            const std::function<void(u32 *rgba, int w, int h)> &overlay, bool capture) {
		(void)addr; (void)stride; (void)format; (void)width; (void)height; (void)overlay; (void)capture;
		return false;
	}
	// Presents a CPU image (RGBA8888, `stride` pixels a row) to the host window, scaled to fit; false when
	// the backend doesn't present to a window.
	virtual bool PresentImage(const u32 *rgba, int width, int height, int stride) {
		(void)rgba; (void)width; (void)height; (void)stride;
		return false;
	}
	// The host window is about to close: stop presenting to it.
	virtual void ReleaseWindow() {}
};

// Installs a backend (not owned; nullptr = the software rasterizer). Flushes what's queued first.
void SetRenderBackend(RenderBackend *backend);
RenderBackend *GetRenderBackend();

// sceDisplay's view of RenderBackend::GetDisplayImage and PresentDisplay (false without a backend).
inline bool GetBackendDisplayImage(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out, int *outW, int *outH,
                                   int outScale = 0) {
	RenderBackend *b = GetRenderBackend();
	return b && b->GetDisplayImage(addr, stride, format, width, height, out, outW, outH, outScale);
}
inline bool PresentBackendDisplay(u32 addr, int stride, int format, int width, int height,
                                  const std::function<void(u32 *rgba, int w, int h)> &overlay, bool capture) {
	RenderBackend *b = GetRenderBackend();
	return b && b->PresentDisplay(addr, stride, format, width, height, overlay, capture);
}

}  // namespace gpu
