// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real SDL host loop and native fullscreen changes. Only event delivery is
// substituted, so this also runs when desktop automation cannot inject physical keys.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <chrono>
#include <cstdio>
#include "runtime/core/memory.h"
#ifdef _WIN32
#include "gpu/hw/d3d11_backend.h"
#else
#include "gpu/hw/opengl_backend.h"
#endif

static int SDLCALL TestPollEvent(SDL_Event *event);
#define SDL_PollEvent TestPollEvent
#include "../../src/runtime/host/sdl_host.cpp"
#undef SDL_PollEvent

namespace {
SDL_Window *testWindow = nullptr;
std::atomic<bool> testDone{false};
std::atomic<bool> firstFrame{false};
std::atomic<int> presentFailures{0};
bool initialFullscreen = false;
int transitions = 0;
int failures = 0;
auto changedAt = std::chrono::steady_clock::now();
}

static int SDLCALL TestPollEvent(SDL_Event *event) {
	int result = SDL_PollEvent(event);
	if (result) {
		if (event->type == SDL_WINDOWEVENT)
			testWindow = SDL_GetWindowFromID(event->window.windowID);
		return result;
	}
	if (!testWindow || testDone || !firstFrame) return 0;
	if (std::chrono::steady_clock::now() - changedAt < std::chrono::milliseconds(100)) return 0;
	bool expected = initialFullscreen != ((transitions % 2) != 0);
	bool actual = (SDL_GetWindowFlags(testWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
	int width = 0, height = 0;
	SDL_GetWindowSize(testWindow, &width, &height);
	// X11/Wayland window managers acknowledge fullscreen/restore asynchronously.
	// Keep pumping native events while waiting for the requested geometry to settle.
	if ((actual != expected || (!actual && (width != 960 || height != 544))) &&
	    std::chrono::steady_clock::now() - changedAt < std::chrono::seconds(2)) return 0;
	if (actual != expected || (!actual && (width != 960 || height != 544))) {
		printf("FAIL: transition %d, fullscreen=%d (expected %d), size=%dx%d\n",
		       transitions, actual, expected, width, height);
		++failures;
	}
	printf("transition %d: fullscreen=%d, size=%dx%d\n", transitions, actual, width, height);
	if (transitions == 8) { testDone = true; return 0; }
	*event = {};
	event->type = SDL_KEYDOWN;
	event->key.windowID = SDL_GetWindowID(testWindow);
	event->key.keysym.sym = SDLK_F11;
	++transitions;
	changedAt = std::chrono::steady_clock::now();
	return 1;
}

int main(int argc, char **argv) {
	SDL_SetMainReady();
	initialFullscreen = argc > 1 && std::string(argv[1]) == "fullscreen";
	const int scale = argc > 2 ? atoi(argv[2]) : 1;
	if (scale != 1 && scale != 4 && scale != 8 && scale != 16) return 2;
	host::Config config;
	config.headless = false;
	config.windowScale = 2;
	config.fullscreen = initialFullscreen;
	config.vsync = false;
	config.title = "PSPC fullscreen regression";
	const std::string presenter = argc > 3 ? argv[3] : "sdl";
	config.gpuPresent = presenter != "sdl";
#ifdef _WIN32
	if (config.gpuPresent && presenter != "d3d11") return 2;
#else
	if (config.gpuPresent && presenter != "opengl") return 2;
#endif
	const auto started = std::chrono::steady_clock::now();
	if (!mem::Init(32 * 1024 * 1024)) return 1;
	int result = host::Run(config, [&] {
		std::unique_ptr<gpu::RenderBackend> backend;
		if (config.gpuPresent) {
#ifdef _WIN32
			gpu::hw::D3D11Options options;
#else
			gpu::hw::OpenGLOptions options;
			options.vsync = false;
#endif
			options.scale = scale;
			options.window = host::NativeWindow();
			std::string error;
#ifdef _WIN32
			backend = gpu::hw::CreateD3D11Backend(options, &error);
#else
			backend = gpu::hw::CreateOpenGLBackend(options, &error);
#endif
			if (!backend) { printf("%s unavailable: %s\n", presenter.c_str(), error.c_str()); return 1; }
			host::SetFramePresenter([&](const u32 *pixels, int width, int height, int stride) {
				if (!backend->PresentImage(pixels, width, height, stride)) {
					++presentFailures;
					testDone = true;
				}
				return true;
			});
		}
		std::vector<u32> image((size_t)480 * 272 * scale * scale, 0xFF336699);
		while (!testDone && std::chrono::steady_clock::now() - started < std::chrono::seconds(30)) {
			host::PresentFrame(image.data(), 480 * scale, 272 * scale, 480 * scale);
			firstFrame = true;
			SDL_Delay(10);
		}
		host::SetFramePresenter(nullptr);
		if (backend) backend->ReleaseWindow();
		return testDone ? 0 : 1;
	}, [] { testDone = true; ++failures; });
	mem::Shutdown();
	if (transitions != 8 || result != 0 || presentFailures != 0) ++failures;
	printf("%d fullscreen transitions, scale %d, %d failures\n", transitions, scale, failures);
	return failures ? 1 : 0;
}
