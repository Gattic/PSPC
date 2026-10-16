// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gpu/backend.h"
#include <memory>
#include <string>

namespace gpu::hw {
struct OpenGLOptions {
	int scale = 1;
	int textureFilter = 0;
	int textureUpscale = 1;
	void *window = nullptr; // SDL_Window; null creates a hidden rendering context.
	bool vsync = true;
	// Vertex transform on the GPU (gpu/geometry.h; PSPC_GPU_TRANSFORM=0 turns it off). While the driver
	// compiles its shaders on threads of its own, the CPU transforms, unless waitForShaders
	// (PSPC_GPU_TRANSFORM_WAIT). verifyTransform (PSPC_GPU_TRANSFORM_VERIFY): the CPU transforms too,
	// and each job is compared.
	bool gpuTransform = true;
	bool waitForShaders = false;
	bool verifyTransform = false;
};
std::unique_ptr<RenderBackend> CreateOpenGLBackend(const OpenGLOptions &, std::string *error);
} // namespace gpu::hw
