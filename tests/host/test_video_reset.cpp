// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise host::Run with real SDL dummy video and software rendering. Link wrappers inject the
// Windows SDL D3D11 reset contract: texture handles survive, but their driver resources do not.
// An invalid update returns an error; copying that texture would dereference null in SDL D3D11.
#include "runtime/host/host.h"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_set>

namespace {
constexpr u32 kPixel = 0xFF785634;
std::atomic<bool> requestReset{false};
std::atomic<int> resets{0}, presentedReset{-1}, invalidUses{0};
// Renderer state is accessed only by the host thread, including the wrapped event pump.
SDL_Renderer *renderer = nullptr;
std::unordered_set<SDL_Texture *> textures, invalid;
bool uploaded = false;

bool WaitFor(const std::atomic<int> &counter, int value) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (counter.load() < value && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	return counter.load() >= value;
}
}

extern "C" {
SDL_Renderer *__real_SDL_CreateRenderer(SDL_Window *, int, Uint32);
SDL_Texture *__real_SDL_CreateTexture(SDL_Renderer *, Uint32, int, int, int);
void __real_SDL_DestroyTexture(SDL_Texture *);
int __real_SDL_UpdateTexture(SDL_Texture *, const SDL_Rect *, const void *, int);
int __real_SDL_RenderCopy(SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *);
void __real_SDL_RenderPresent(SDL_Renderer *);
int __real_SDL_PollEvent(SDL_Event *);

SDL_Renderer *__wrap_SDL_CreateRenderer(SDL_Window *window, int, Uint32) {
	// No display server/GPU is needed to exercise the real host event loop and texture lifecycle.
	renderer = __real_SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	return renderer;
}
SDL_Texture *__wrap_SDL_CreateTexture(SDL_Renderer *ren, Uint32 format, int access, int w, int h) {
	SDL_Texture *texture = __real_SDL_CreateTexture(ren, format, access, w, h);
	if (texture) textures.insert(texture);
	return texture;
}
void __wrap_SDL_DestroyTexture(SDL_Texture *texture) {
	textures.erase(texture);
	invalid.erase(texture);
	__real_SDL_DestroyTexture(texture);
}
int __wrap_SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch) {
	if (invalid.count(texture)) {
		++invalidUses;
		return SDL_SetError("Injected device reset: texture is not currently available");
	}
	int result = __real_SDL_UpdateTexture(texture, rect, pixels, pitch);
	uploaded = result == 0;
	return result;
}
int __wrap_SDL_RenderCopy(SDL_Renderer *ren, SDL_Texture *texture, const SDL_Rect *src, const SDL_Rect *dst) {
	if (invalid.count(texture)) {
		++invalidUses;
		return SDL_SetError("Would dereference invalid SDL D3D11 texture resources");
	}
	return __real_SDL_RenderCopy(ren, texture, src, dst);
}
void __wrap_SDL_RenderPresent(SDL_Renderer *ren) {
	int w = 0, h = 0;
	u32 pixel = 0;
	SDL_GetRendererOutputSize(ren, &w, &h);
	SDL_Rect center{w / 2, h / 2, 1, 1};
	bool correct = uploaded && SDL_RenderReadPixels(ren, &center, SDL_PIXELFORMAT_ABGR8888, &pixel, 4) == 0 && pixel == kPixel;
	__real_SDL_RenderPresent(ren);
	// A late pre-reset frame must not acknowledge the next reset.
	if (correct) presentedReset = resets.load();
}
int __wrap_SDL_PollEvent(SDL_Event *event) {
	if (requestReset.exchange(false)) {
		invalid = textures;
		uploaded = false;
		if (renderer) {
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
			SDL_RenderClear(renderer);
		}
		*event = {};
		event->type = SDL_RENDER_DEVICE_RESET;
		++resets;
		return 1;
	}
	return __real_SDL_PollEvent(event);
}
}

int main(int argc, char **argv) {
	const bool paused = argc > 1 && !std::strcmp(argv[1], "paused");
	const bool scaled = argc > 1 && !std::strcmp(argv[1], "scaled");
	host::Config cfg;
	cfg.windowScale = 1;
	cfg.vsync = false;
	int result = host::Run(cfg, [&] {
		if (host::IsHeadless()) return 1;
		const int w = scaled ? 960 : 480, h = scaled ? 544 : 272;
		std::vector<u32> pixels((size_t)w * h, kPixel);
		host::PresentFrame(pixels.data(), w, h, w);
		if (!WaitFor(presentedReset, 0)) {
			std::fprintf(stderr, "Initial frame not presented: %s\n", SDL_GetError());
			return 1;
		}
		for (int i = 1; i <= 2; ++i) {
			requestReset = true;
			if (!WaitFor(resets, i)) return 1;
			if (!paused) host::PresentFrame(pixels.data(), w, h, w);
			if (!WaitFor(presentedReset, i) || invalidUses.load()) {
				std::fprintf(stderr, "Device reset %d: frame not restored or invalid texture reused (%d uses)\n", i, invalidUses.load());
				return 1;
			}
		}
		return 0;
	}, [] {});
	// The host can finish an iteration after the worker returns; check through teardown too.
	if (!textures.empty() || invalidUses.load()) result = 1;
	if (!result) std::puts("Device resets restored the frame without reusing invalid textures");
	return result;
}
