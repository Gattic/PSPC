// SPDX-License-Identifier: GPL-2.0-or-later
// SDL2 host: window/presentation on the main thread, emulation on a worker thread.
#include <SDL.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "common/log.h"
#include "runtime/host/host.h"

namespace host {

namespace {

enum : u32 {
	CTRL_SELECT = 0x000001,
	CTRL_START = 0x000008,
	CTRL_UP = 0x000010,
	CTRL_RIGHT = 0x000020,
	CTRL_DOWN = 0x000040,
	CTRL_LEFT = 0x000080,
	CTRL_LTRIGGER = 0x000100,
	CTRL_RTRIGGER = 0x000200,
	CTRL_TRIANGLE = 0x001000,
	CTRL_CIRCLE = 0x002000,
	CTRL_CROSS = 0x004000,
	CTRL_SQUARE = 0x008000,
	CTRL_HOME = 0x010000,
};

bool g_headless = true;
std::mutex g_frameMutex;
std::vector<u32> g_frame;
int g_frameW = 0, g_frameH = 0;
bool g_frameDirty = false;

std::atomic<u32> g_keyButtons{0}, g_padButtons{0};
std::atomic<int> g_keyLx{128}, g_keyLy{128}, g_padLx{128}, g_padLy{128};
SDL_AudioDeviceID g_audio = 0;

u32 KeyToButton(SDL_Keycode k) {
	switch (k) {
	case SDLK_UP: return CTRL_UP;
	case SDLK_DOWN: return CTRL_DOWN;
	case SDLK_LEFT: return CTRL_LEFT;
	case SDLK_RIGHT: return CTRL_RIGHT;
	case SDLK_z: return CTRL_CROSS;
	case SDLK_x: return CTRL_CIRCLE;
	case SDLK_a: return CTRL_SQUARE;
	case SDLK_s: return CTRL_TRIANGLE;
	case SDLK_q: return CTRL_LTRIGGER;
	case SDLK_w: return CTRL_RTRIGGER;
	case SDLK_RETURN: return CTRL_START;
	case SDLK_SPACE:
	case SDLK_RSHIFT: return CTRL_SELECT;
	case SDLK_ESCAPE: return CTRL_HOME;
	default: return 0;
	}
}

u32 PadToButton(Uint8 b) {
	switch (b) {
	case SDL_CONTROLLER_BUTTON_A: return CTRL_CROSS;
	case SDL_CONTROLLER_BUTTON_B: return CTRL_CIRCLE;
	case SDL_CONTROLLER_BUTTON_X: return CTRL_SQUARE;
	case SDL_CONTROLLER_BUTTON_Y: return CTRL_TRIANGLE;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return CTRL_LTRIGGER;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return CTRL_RTRIGGER;
	case SDL_CONTROLLER_BUTTON_START: return CTRL_START;
	case SDL_CONTROLLER_BUTTON_BACK: return CTRL_SELECT;
	case SDL_CONTROLLER_BUTTON_GUIDE: return CTRL_HOME;
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return CTRL_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return CTRL_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return CTRL_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return CTRL_RIGHT;
	default: return 0;
	}
}

void UpdateKeyAnalog(const Uint8 *ks) {
	int x = 128, y = 128;
	if (ks[SDL_SCANCODE_J]) x = 0;
	if (ks[SDL_SCANCODE_L]) x = 255;
	if (ks[SDL_SCANCODE_I]) y = 0;
	if (ks[SDL_SCANCODE_K]) y = 255;
	g_keyLx = x;
	g_keyLy = y;
}

}  // namespace

bool IsHeadless() { return g_headless; }

int Run(const Config &cfg, const std::function<int()> &emulation, const std::function<void()> &onQuit) {
	g_headless = cfg.headless;
	if (cfg.headless) return emulation();

	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		LOGE("HOST", "SDL_Init failed: %s; running headless", SDL_GetError());
		g_headless = true;
		return emulation();
	}
	SDL_Window *win = SDL_CreateWindow(cfg.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                   480 * cfg.windowScale, 272 * cfg.windowScale, SDL_WINDOW_RESIZABLE);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
	SDL_RenderSetLogicalSize(ren, 480, 272);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);

	SDL_AudioSpec want{}, have{};
	want.freq = 44100;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (g_audio) SDL_PauseAudioDevice(g_audio, 0);

	std::vector<SDL_GameController *> pads;
	std::atomic<bool> done{false};
	int result = 0;
	std::thread emu([&]() {
		result = emulation();
		done = true;
	});

	bool quitSent = false;
	while (!done) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT:
				if (!quitSent) {
					quitSent = true;
					onQuit();
				}
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				u32 b = KeyToButton(ev.key.keysym.sym);
				if (b) {
					if (ev.type == SDL_KEYDOWN) g_keyButtons |= b;
					else g_keyButtons &= ~b;
				}
				UpdateKeyAnalog(SDL_GetKeyboardState(nullptr));
				break;
			}
			case SDL_CONTROLLERDEVICEADDED:
				if (SDL_GameController *gc = SDL_GameControllerOpen(ev.cdevice.which)) pads.push_back(gc);
				break;
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP: {
				u32 b = PadToButton(ev.cbutton.button);
				if (ev.type == SDL_CONTROLLERBUTTONDOWN) g_padButtons |= b;
				else g_padButtons &= ~b;
				break;
			}
			case SDL_CONTROLLERAXISMOTION: {
				int v = ev.caxis.value;
				int m = (v + 32768) >> 8;
				if (m < 0) m = 0;
				if (m > 255) m = 255;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_padLx = m;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_padLy = m;
				u32 trig = 0;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) trig = CTRL_LTRIGGER;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) trig = CTRL_RTRIGGER;
				if (trig) {
					if (v > 16000) g_padButtons |= trig;
					else g_padButtons &= ~trig;
				}
				break;
			}
			default: break;
			}
		}
		bool present = false;
		{
			std::lock_guard<std::mutex> lock(g_frameMutex);
			if (g_frameDirty) {
				SDL_UpdateTexture(tex, nullptr, g_frame.data(), g_frameW * 4);
				g_frameDirty = false;
				present = true;
			}
		}
		if (present) {
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, tex, nullptr, nullptr);
			SDL_RenderPresent(ren);
		} else {
			SDL_Delay(1);
		}
	}
	emu.join();
	for (auto *p : pads) SDL_GameControllerClose(p);
	if (g_audio) SDL_CloseAudioDevice(g_audio);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return result;
}

void PresentFrame(const u32 *rgba, int width, int height, int stride) {
	if (g_headless) return;
	std::lock_guard<std::mutex> lock(g_frameMutex);
	g_frame.resize((size_t)width * height);
	for (int y = 0; y < height; y++) memcpy(&g_frame[(size_t)y * width], rgba + (size_t)y * stride, (size_t)width * 4);
	g_frameW = width;
	g_frameH = height;
	g_frameDirty = true;
}

CtrlState GetCtrl() {
	CtrlState s;
	s.buttons = g_keyButtons | g_padButtons;
	int lx = g_padLx, ly = g_padLy;
	if (g_keyLx != 128) lx = g_keyLx;
	if (g_keyLy != 128) ly = g_keyLy;
	s.lx = (u8)lx;
	s.ly = (u8)ly;
	return s;
}

void AudioSubmit(const s16 *samples, int frames) {
	if (g_headless || !g_audio) return;
	// Keep roughly 4 PSP audio blocks queued; beyond that, wait for the device to drain.
	const Uint32 maxBytes = 44100 / 10 * 4;
	while (SDL_GetQueuedAudioSize(g_audio) > maxBytes) SDL_Delay(1);
	SDL_QueueAudio(g_audio, samples, (Uint32)frames * 4);
}

int AudioQueuedFrames() { return g_audio ? (int)(SDL_GetQueuedAudioSize(g_audio) / 4) : 0; }

}  // namespace host
