// SPDX-License-Identifier: GPL-2.0-or-later
// Real host event loop/SDL dummy presentation, with the native restore hazards injected:
// renderer recreation in fullscreen loses the windowed geometry, and a size request during
// restoration races the compositor's decoration/scale update. No display server or saves needed.
#include "runtime/host/host.h"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
SDL_Window *window = nullptr;
bool fullscreen = false, restoring = false, initialFullscreen = false, doubleSize = false;
bool resized = false;
int savedW = 960, savedH = 544, expectedW = 960, expectedH = 544;
int transitions = 0, failures = 0, presentations = 0;
std::atomic<bool> finished{false};
}

extern "C" {
SDL_Window *__real_SDL_CreateWindow(const char *, int, int, int, int, Uint32);
SDL_Renderer *__real_SDL_CreateRenderer(SDL_Window *, int, Uint32);
Uint32 __real_SDL_GetWindowFlags(SDL_Window *);
void __real_SDL_SetWindowSize(SDL_Window *, int, int);
int __real_SDL_PollEvent(SDL_Event *);
void __real_SDL_RenderPresent(SDL_Renderer *);

SDL_Window *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
	window = __real_SDL_CreateWindow(title, x, y, w, h, flags);
	return window;
}
SDL_Renderer *__wrap_SDL_CreateRenderer(SDL_Window *win, int, Uint32) {
	// Older SDL recreates a non-OpenGL native window for its OpenGL presenter. Creating it
	// while fullscreen can replace SDL's saved windowed size with the current desktop size.
	if (fullscreen) SDL_GetWindowSize(win, &savedW, &savedH);
	return __real_SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
}
Uint32 __wrap_SDL_GetWindowFlags(SDL_Window *win) {
	Uint32 flags = __real_SDL_GetWindowFlags(win) & ~SDL_WINDOW_FULLSCREEN_DESKTOP;
	return flags | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
int __wrap_SDL_SetWindowFullscreen(SDL_Window *win, Uint32 flags) {
	if (flags) {
		SDL_GetWindowSize(win, &savedW, &savedH);
		__real_SDL_SetWindowSize(win, 1920, 1080);
		fullscreen = true;
	} else {
		__real_SDL_SetWindowSize(win, savedW, savedH);
		fullscreen = false;
		restoring = true;
	}
	return 0;
}
void __wrap_SDL_SetWindowSize(SDL_Window *win, int w, int h) {
	// State flags can change before the native decoration/scale transition has settled.
	// An extra resize in that interval is not the same operation as restoring the window.
	if (restoring) {
		if (doubleSize) { w *= 2; h *= 2; }
		else h -= 74;
	}
	__real_SDL_SetWindowSize(win, w, h);
}
void __wrap_SDL_RenderPresent(SDL_Renderer *renderer) {
	__real_SDL_RenderPresent(renderer);
	++presentations;
}
int __wrap_SDL_PollEvent(SDL_Event *event) {
	if (__real_SDL_PollEvent(event)) return 1;
	if (!window || !presentations || finished) return 0;
	const bool expectedFullscreen = initialFullscreen != ((transitions % 2) != 0);
	int w = 0, h = 0;
	SDL_GetWindowSize(window, &w, &h);
	if (fullscreen != expectedFullscreen || (!fullscreen && (w != expectedW || h != expectedH))) {
		std::fprintf(stderr, "Restore %d: fullscreen=%d, %dx%d; expected fullscreen=%d, %dx%d\n",
		             transitions, fullscreen, w, h, expectedFullscreen, expectedW, expectedH);
		++failures;
	}
	restoring = false;
	if (transitions == 8) {
		finished = true;
		return 0;
	}
	// A user resize after the first round trip must become the size restored on later trips,
	// rather than pinning restoration to the configured startup dimensions.
	if (!fullscreen && transitions && !resized) {
		expectedW = 800;
		expectedH = 600;
		__real_SDL_SetWindowSize(window, expectedW, expectedH);
		resized = true;
		return 0;
	}
	*event = {};
	event->type = SDL_KEYDOWN;
	event->key.windowID = SDL_GetWindowID(window);
	event->key.keysym.sym = SDLK_F11;
	++transitions;
	return 1;
}
}

int main(int argc, char **argv) {
	if (argc != 3) return 2;
	initialFullscreen = !std::strcmp(argv[1], "fullscreen");
	doubleSize = !std::strcmp(argv[2], "scale");
	host::Config config;
	config.windowScale = 2;
	config.fullscreen = initialFullscreen;
	config.vsync = false;
	int result = host::Run(config, [&] {
		if (host::IsHeadless()) return 1;
		std::vector<u32> pixels(480 * 272, 0xFF336699);
		host::PresentFrame(pixels.data(), 480, 272, 480);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!finished && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return finished ? 0 : 1;
	}, [] { finished = true; ++failures; });
	if (transitions != 8 || !resized || !presentations || result != 0) ++failures;
	std::printf("%d fullscreen transitions with user resize, %d failures\n", transitions, failures);
	return failures ? 1 : 0;
}
