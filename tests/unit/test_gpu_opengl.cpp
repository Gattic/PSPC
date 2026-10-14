// SPDX-License-Identifier: GPL-2.0-or-later
#if PSPC_TEST_GPU_GL
#include "gpu/hw/opengl_backend.h"
#include "testing.h"
#include <SDL.h>
#include <cstdlib>
#include <thread>

TEST(gpu_opengl_context_presentation_and_release) {
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		if (getenv("PSPC_RENDERER_REQUIRE"))
			CHECK(false);
		return;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_Window *window =
		SDL_CreateWindow("PSPC synthetic GPU test", 0, 0, 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if (!window) {
		if (getenv("PSPC_RENDERER_REQUIRE"))
			CHECK(false);
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
		return;
	}
	SDL_GLContext peer = SDL_GL_CreateContext(window);
	gpu::hw::OpenGLOptions options;
	options.window = window;
	options.vsync = false;
	std::string error;
	auto backend = gpu::hw::CreateOpenGLBackend(options, &error);
	CHECK(SDL_GL_GetCurrentContext() == peer);
	if (!backend) {
		printf("OpenGL unavailable: %s\n", error.c_str());
		if (getenv("PSPC_RENDERER_REQUIRE"))
			CHECK(false);
	} else {
		std::vector<u32> pixels(480 * 272, 0xFF123456);
		CHECK(backend->PresentImage(pixels.data(), 480, 272, 480));
		CHECK(SDL_GL_GetCurrentContext() == peer);
		// Runtime drawing and presentation occur on a worker; events/resizing stay on the host thread.
		SDL_SetWindowSize(window, 960, 544);
		CHECK(SDL_GL_MakeCurrent(window, nullptr) == 0);
		bool presented = false;
		std::thread worker([&] {
			try {
				presented = backend->PresentImage(pixels.data(), 480, 272, 480);
			} catch (const std::exception &e) {
				error = e.what();
			}
		});
		worker.join();
		if (!presented)
			printf("Worker presentation failed: %s\n", error.c_str());
		CHECK(presented);
		CHECK(SDL_GL_GetCurrentContext() == nullptr);
		CHECK(SDL_GL_MakeCurrent(window, peer) == 0);
		backend->ReleaseWindow();
		CHECK(SDL_GL_GetCurrentContext() == peer);
		CHECK(!backend->PresentImage(pixels.data(), 480, 272, 480));
		backend->ReleaseWindow();
	}
	SDL_GL_DeleteContext(peer);
	SDL_DestroyWindow(window);
	backend.reset();
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
#endif
