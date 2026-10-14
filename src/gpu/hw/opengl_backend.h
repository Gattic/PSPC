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
};
std::unique_ptr<RenderBackend> CreateOpenGLBackend(const OpenGLOptions &, std::string *error);
} // namespace gpu::hw
