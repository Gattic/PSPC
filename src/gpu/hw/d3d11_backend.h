// SPDX-License-Identifier: GPL-2.0-or-later
//
// The optional Direct3D 11 renderer backend (gpu/backend.h). The software rasterizer stays the default
// and the accuracy reference; this one draws on the GPU, at 1x or upscaled, and keeps guest VRAM
// coherent with what it renders.
#pragma once

#include <memory>
#include <string>

#include "gpu/backend.h"

namespace gpu::hw {

struct D3D11Options {
	int scale = 1;      // internal render scale, 1-16 (8: 3840 x 2176, 4K; 16: 7680 x 4352, 8K)
	bool warp = false;  // the WARP software device instead of a GPU
	int adapter = -1;   // adapter index by GPU preference (high performance first); -1 = the first
	// A private host view of VRAM (mem::PrivateVramView). With it, guest VRAM pages holding rendering
	// not yet written back are made inaccessible and written back when touched; without it, everything
	// rendered is written back at each list end and sceGeDrawSync/ListSync.
	u8 *privateVram = nullptr;
	// The host window (HWND) to present to (RenderBackend::PresentDisplay / PresentImage): the rendering
	// goes from the GPU to the window without a round trip through the CPU. nullptr: headless.
	void *window = nullptr;
	// Texture filtering: 0 = as the game sets it, 1 = always nearest, 2 = always linear.
	int textureFilter = 0;
	// Texture upscaling, 1 (off) - 4: decoded textures are scaled up that many times with xBR.
	int textureUpscale = 1;
};

// Creates the backend; nullptr (with the reason in *error) when this machine can't run it (no D3D11
// device with rasterizer-ordered views).
std::unique_ptr<RenderBackend> CreateD3D11Backend(const D3D11Options &opts, std::string *error);

}  // namespace gpu::hw
