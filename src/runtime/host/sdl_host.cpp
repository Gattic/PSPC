// SPDX-License-Identifier: GPL-2.0-or-later
// SDL2 host: window/presentation on the main thread, emulation on a worker thread.
//
// Input goes through the bindings in Config (pspc.ini, defaults in common/settings.cpp).
// Hotkeys: F11 toggles fullscreen, F12 saves a screenshot, Ctrl+Q quits like closing the window.
//
// Frames come from the emulation thread (PresentFrame) and are shown with an SDL renderer, unless a GPU
// renderer presents them to the window itself (SetFramePresenter, Config::gpuPresent).
#include <SDL.h>
#ifdef _WIN32
#include <SDL_syswm.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "common/fileutil.h"
#include "common/app_paths.h"
#include "common/log.h"
#include "common/host.h"
#include "common/settings.h"
#include "runtime/host/host.h"

namespace host {

namespace {

bool g_headless = true;
Config g_cfg;
void *g_hwnd = nullptr;  // Windows: HWND; Linux: SDL_Window for the OpenGL context.
std::atomic<int> g_fitScale{0};
std::mutex g_frameMutex;
std::vector<u32> g_frame;
int g_frameW = 0, g_frameH = 0;
bool g_frameDirty = false;
// A GPU renderer showing the frames itself (emulation thread only), and F12 waiting for one.
std::function<bool(const u32 *, int, int, int)> g_presenter;
std::atomic<bool> g_hasPresenter{false};
std::atomic<bool> g_screenshotRequested{false};

// Input state read by the emulation thread: buttons from keys and controller buttons, the
// analog stick from bound keys/buttons ("digital", 128 = centred) or the controller stick.
std::atomic<u32> g_buttons{0};
std::atomic<int> g_digitalLx{128}, g_digitalLy{128};
// Controller sticks (0 = left, 1 = right), 0-255 with 128 centred.
std::atomic<int> g_stickPos[2][2] = {{128, 128}, {128, 128}};
// Which stick drives the PSP analog stick: both (the one pushed further), left, right or none.
enum class StickMode { Both, Left, Right, None };
std::atomic<StickMode> g_stickMode{StickMode::Both};
SDL_AudioDeviceID g_audio = 0;
std::atomic<int> g_volume{100};

// Bindings resolved from the Config, one per settings::kControls entry (host thread only).
struct Binding {
	std::vector<SDL_Keycode> keys;
	std::vector<SDL_GameControllerButton> buttons;
	std::vector<SDL_GameControllerAxis> triggers;
};
std::vector<Binding> g_bind;

// What is held right now (host thread only).
std::vector<SDL_Keycode> g_heldKeys;
bool g_heldButtons[SDL_CONTROLLER_BUTTON_MAX] = {};
bool g_heldTriggers[SDL_CONTROLLER_AXIS_MAX] = {};

void ResolveBindings(const Config &cfg) {
	settings::Settings defaults;
	g_bind.assign(settings::kNumControls, Binding{});
	for (size_t i = 0; i < settings::kNumControls; i++) {
		const char *control = settings::kControls[i].name;
		const std::string &keys = i < cfg.keys.size() ? cfg.keys[i] : defaults.keys[i];
		for (const std::string &name : settings::SplitList(keys)) {
			SDL_Keycode k = ToLower(name) == "comma" ? SDLK_COMMA : SDL_GetKeyFromName(name.c_str());
			if (k == SDLK_UNKNOWN) LOGW("HOST", "key.%s: unknown key name \"%s\"", control, name.c_str());
			else g_bind[i].keys.push_back(k);
		}
		const std::string &pads = i < cfg.pads.size() ? cfg.pads[i] : defaults.pads[i];
		for (const std::string &name : settings::SplitList(pads)) {
			SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.c_str());
			SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name.c_str());
			if (b != SDL_CONTROLLER_BUTTON_INVALID) g_bind[i].buttons.push_back(b);
			else if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) g_bind[i].triggers.push_back(a);
			else LOGW("HOST", "pad.%s: unknown controller button \"%s\"", control, name.c_str());
		}
	}
	g_stickMode = cfg.padStick == "left"    ? StickMode::Left
	              : cfg.padStick == "right" ? StickMode::Right
	              : cfg.padStick == "none"  ? StickMode::None
	                                        : StickMode::Both;
}

// Recomputes the PSP buttons and the digital analog directions from what is held. For opposite
// analog directions held together, right/down wins.
void UpdateInput() {
	u32 buttons = 0;
	int axis[2] = {128, 128};
	for (size_t i = 0; i < g_bind.size(); i++) {
		const Binding &b = g_bind[i];
		bool held = false;
		for (SDL_Keycode k : b.keys) held |= std::find(g_heldKeys.begin(), g_heldKeys.end(), k) != g_heldKeys.end();
		for (SDL_GameControllerButton btn : b.buttons) held |= g_heldButtons[btn];
		for (SDL_GameControllerAxis t : b.triggers) held |= g_heldTriggers[t];
		if (!held) continue;
		const settings::Control &c = settings::kControls[i];
		if (c.button) buttons |= c.button;
		else axis[c.axis] = c.dir < 0 ? 0 : 255;
	}
	g_buttons = buttons;
	g_digitalLx = axis[0];
	g_digitalLy = axis[1];
}

void SetFullscreen(SDL_Window *win, bool on) {
	if (SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
		LOGW("HOST", "Fullscreen switch failed: %s", SDL_GetError());
	SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
}

// F12: the presented frame (at the resolution it was presented) as a 24-bit BMP.
void SaveScreenshot(const Config &cfg, const u32 *px, int w, int h) {
	if (!px || w <= 0 || h <= 0) {
		LOGW("HOST", "Screenshot: nothing has been displayed yet");
		return;
	}
	fs::path dir = cfg.screenshotDir.empty() ? apppaths::DataRoot() / "screenshots" : fs::u8path(cfg.screenshotDir);
	std::error_code ec;
	fs::create_directories(dir, ec);
	time_t now = time(nullptr);
	tm local{};
	hostsys::LocalTime(now, local);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
	std::string base = cfg.gameName + "_" + stamp;
	fs::path file = dir / fs::u8path(base + ".bmp");
	for (int n = 2; fs::exists(file, ec); n++) file = dir / fs::u8path(base + "_" + std::to_string(n) + ".bmp");
	// XBGR8888 has no alpha mask, so SDL writes a plain 24-bit BMP (the PSP's alpha byte is not
	// opacity).
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)px, w, h, 32, w * 4, SDL_PIXELFORMAT_XBGR8888);
	if (s && SDL_SaveBMP(s, PathToUtf8(file).c_str()) == 0) LOGI("HOST", "Screenshot saved to %s", PathToUtf8(file).c_str());
	else LOGE("HOST", "Screenshot %s failed: %s", PathToUtf8(file).c_str(), SDL_GetError());
	if (s) SDL_FreeSurface(s);
}

}  // namespace

bool IsHeadless() { return g_headless; }

int Run(const Config &cfg, const std::function<int()> &emulation, const std::function<void()> &onQuit) {
	g_headless = cfg.headless;
	g_cfg = cfg;
	if (cfg.headless) return emulation();

#ifdef _WIN32
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		LOGE("HOST", "SDL_Init failed: %s; running headless", SDL_GetError());
		g_headless = true;
		return emulation();
	}
	ResolveBindings(cfg);
	g_volume = std::clamp(cfg.volume, 0, 100);
	Uint32 winFlags = SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#ifndef _WIN32
	if (cfg.gpuPresent) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		winFlags |= SDL_WINDOW_OPENGL;
	}
#endif
	SDL_Window *win = SDL_CreateWindow(cfg.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                   480 * cfg.windowScale, 272 * cfg.windowScale, winFlags);
#ifndef _WIN32
	if (!win && cfg.gpuPresent) {
		LOGW("HOST", "OpenGL window unavailable (%s); creating a software window", SDL_GetError());
		win = SDL_CreateWindow(cfg.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		                       480 * cfg.windowScale, 272 * cfg.windowScale, winFlags & ~SDL_WINDOW_OPENGL);
	}
#endif
	if (cfg.fullscreen) SDL_ShowCursor(SDL_DISABLE);
	// FitScale is read by the emulation thread; keep all SDL window queries on this thread.
	auto updateFitScale = [&]() {
		int w = 0, h = 0;
		if (win) SDL_GetWindowSize(win, &w, &h);
		const int pictureW = std::min(w, (int)((long long)h * 480 / 272));
		g_fitScale = win ? std::max(1, (pictureW + 479) / 480) : 0;
	};
	updateFitScale();
#ifdef _WIN32
	SDL_SysWMinfo wm;
	SDL_VERSION(&wm.version);
	if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_WINDOWS) g_hwnd = wm.info.win.window;
#else
	g_hwnd = win;
#endif
	// The SDL renderer, unless a GPU renderer will present instead (then made once a frame comes here).
	SDL_Renderer *ren = nullptr;
	SDL_Texture *tex = nullptr;
	auto makeRenderer = [&]() {
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
		SDL_RenderSetLogicalSize(ren, 480, 272);
	};
	if (!cfg.gpuPresent || !g_hwnd) makeRenderer();

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

	// The frame on screen (swapped out of g_frame), kept for F12.
	std::vector<u32> shown;
	int shownW = 0, shownH = 0;
	bool quitSent = false;
	auto quit = [&]() {
		if (!quitSent) {
			quitSent = true;
			onQuit();
		}
	};
	while (!done) {
		SDL_Event ev;
		bool screenshot = false;
		bool present = false;
		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT: quit(); break;
			case SDL_RENDER_DEVICE_RESET:
				// Fullscreen/resize can reset SDL's device. The texture handle survives, but its
				// driver resources do not; even a same-size frame must use a new texture.
				SDL_DestroyTexture(tex);
				tex = nullptr;
				present = !shown.empty();  // Restore the last frame even when emulation is paused.
				LOGI("HOST", "SDL render device reset; recreating the frame texture");
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				SDL_Keycode k = ev.key.keysym.sym;
				bool down = ev.type == SDL_KEYDOWN;
				if (k == SDLK_F11 || k == SDLK_F12 || (k == SDLK_q && down && (ev.key.keysym.mod & KMOD_CTRL))) {
					if (down && !ev.key.repeat) {
						if (k == SDLK_F11) SetFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP));
						else if (k == SDLK_F12 && g_hasPresenter) g_screenshotRequested = true;
						else if (k == SDLK_F12) screenshot = true;
						else quit();
					}
					break;
				}
				auto it = std::find(g_heldKeys.begin(), g_heldKeys.end(), k);
				if (down && it == g_heldKeys.end()) g_heldKeys.push_back(k);
				if (!down && it != g_heldKeys.end()) g_heldKeys.erase(it);
				UpdateInput();
				break;
			}
			case SDL_CONTROLLERDEVICEADDED:
				if (SDL_GameController *gc = SDL_GameControllerOpen(ev.cdevice.which)) pads.push_back(gc);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				for (auto it = pads.begin(); it != pads.end(); ++it)
					if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(*it)) == ev.cdevice.which) {
						SDL_GameControllerClose(*it);
						pads.erase(it);
						break;
					}
				memset(g_heldButtons, 0, sizeof(g_heldButtons));
				memset(g_heldTriggers, 0, sizeof(g_heldTriggers));
				for (auto &s : g_stickPos) s[0] = s[1] = 128;
				UpdateInput();
				break;
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
				if (ev.cbutton.button < SDL_CONTROLLER_BUTTON_MAX) {
					g_heldButtons[ev.cbutton.button] = ev.type == SDL_CONTROLLERBUTTONDOWN;
					UpdateInput();
				}
				break;
			case SDL_CONTROLLERAXISMOTION: {
				int v = ev.caxis.value;
				int m = (v + 32768) >> 8;
				if (m < 0) m = 0;
				if (m > 255) m = 255;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_stickPos[0][0] = m;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_stickPos[0][1] = m;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX) g_stickPos[1][0] = m;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY) g_stickPos[1][1] = m;
				if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
					bool on = v > 16000;
					if (g_heldTriggers[ev.caxis.axis] != on) {
						g_heldTriggers[ev.caxis.axis] = on;
						UpdateInput();
					}
				}
				break;
			}
			default: break;
			}
		}
		updateFitScale();
		{
			std::lock_guard<std::mutex> lock(g_frameMutex);
			if (g_frameDirty) {
				shown.swap(g_frame);
				shownW = g_frameW;
				shownH = g_frameH;
				g_frameDirty = false;
				present = true;
			}
		}
		if (screenshot) SaveScreenshot(cfg, shown.data(), shownW, shownH);
		if (present && !ren) makeRenderer();
		if (present && ren) {
			// A GPU renderer at a render scale above 1 presents larger frames: the texture follows.
			int texW = 0, texH = 0;
			if (tex) SDL_QueryTexture(tex, nullptr, nullptr, &texW, &texH);
			if ((!tex || shownW != texW || shownH != texH) && shownW > 0 && shownH > 0) {
				SDL_DestroyTexture(tex);
				tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, shownW, shownH);
				SDL_SetTextureScaleMode(tex, shownW > 480 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
			}
			SDL_UpdateTexture(tex, nullptr, shown.data(), shownW * 4);
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, tex, nullptr, nullptr);
			// Diagnostics: PSPC_HOST_SNAPSHOT=<file.bmp> saves what the renderer drew for the 600th
			// presented frame, or the PSPC_HOST_SNAPSHOT_AT=<n>th (screen captures fail while the
			// desktop is locked).
			static int presents = 0;
			static const char *snap = getenv("PSPC_HOST_SNAPSHOT");
			static const int snapAt = [] {
				const char *at = getenv("PSPC_HOST_SNAPSHOT_AT");
				return at && atoi(at) > 0 ? atoi(at) : 600;
			}();
			if (snap && ++presents == snapAt) {
				int w = 0, h = 0;
				SDL_GetRendererOutputSize(ren, &w, &h);
				if (SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888)) {
					if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
						SDL_SaveBMP(s, snap);
					SDL_FreeSurface(s);
				}
			}
			SDL_RenderPresent(ren);
		} else {
			SDL_Delay(1);
		}
	}
	emu.join();
	for (auto *p : pads) SDL_GameControllerClose(p);
	if (g_audio) SDL_CloseAudioDevice(g_audio);
	if (tex) SDL_DestroyTexture(tex);
	if (ren) SDL_DestroyRenderer(ren);
	g_hwnd = nullptr;
	g_fitScale = 0;
	SDL_DestroyWindow(win);
	SDL_Quit();
	return result;
}

void PresentFrame(const u32 *rgba, int width, int height, int stride) {
	if (g_headless) return;
	if (g_presenter) {
		if (stride == width) SaveScreenshotIfRequested(rgba, width, height);
		if (g_presenter(rgba, width, height, stride)) return;
	}
	std::lock_guard<std::mutex> lock(g_frameMutex);
	g_frame.resize((size_t)width * height);
	for (int y = 0; y < height; y++) memcpy(&g_frame[(size_t)y * width], rgba + (size_t)y * stride, (size_t)width * 4);
	g_frameW = width;
	g_frameH = height;
	g_frameDirty = true;
}

void *NativeWindow() { return g_hwnd; }

int FitScale() { return g_fitScale.load(); }

void SetFramePresenter(std::function<bool(const u32 *, int, int, int)> presenter) {
	g_presenter = std::move(presenter);
	g_hasPresenter = (bool)g_presenter;
}

bool ScreenshotRequested() { return g_screenshotRequested; }

void SaveScreenshotIfRequested(const u32 *rgba, int width, int height) {
	if (g_screenshotRequested.exchange(false)) SaveScreenshot(g_cfg, rgba, width, height);
}

CtrlState GetCtrl() {
	CtrlState s;
	s.buttons = g_buttons;
	int lx = 128, ly = 128;
	auto deflection = [](int s) {
		int dx = std::abs(g_stickPos[s][0] - 128), dy = std::abs(g_stickPos[s][1] - 128);
		return std::max(dx, dy);
	};
	int stick = -1;
	switch (g_stickMode.load()) {
	case StickMode::Left: stick = 0; break;
	case StickMode::Right: stick = 1; break;
	case StickMode::Both: stick = deflection(1) > deflection(0) ? 1 : 0; break;
	case StickMode::None: break;
	}
	if (stick >= 0) {
		lx = g_stickPos[stick][0];
		ly = g_stickPos[stick][1];
	}
	if (g_digitalLx != 128) lx = g_digitalLx;
	if (g_digitalLy != 128) ly = g_digitalLy;
	s.lx = (u8)lx;
	s.ly = (u8)ly;
	return s;
}

bool AudioSubmit(const s16 *samples, int frames, bool waitForSpace) {
	if (g_headless || !g_audio) return false;
	const Uint32 maxBytes = 44100 / 10 * 4;
	while (SDL_GetQueuedAudioSize(g_audio) > maxBytes) {
		if (!waitForSpace) return false;
		SDL_Delay(1);
	}
	int volume = g_volume;
	if (volume >= 100) return SDL_QueueAudio(g_audio, samples, (Uint32)frames * 4) == 0;
	// Muted audio is still queued so virtual-time pacing is unchanged.
	thread_local std::vector<s16> scaled;
	scaled.resize((size_t)frames * 2);
	for (size_t i = 0; i < scaled.size(); i++) scaled[i] = (s16)(samples[i] * volume / 100);
	return SDL_QueueAudio(g_audio, scaled.data(), (Uint32)frames * 4) == 0;
}

int AudioQueuedFrames() { return g_audio ? (int)(SDL_GetQueuedAudioSize(g_audio) / 4) : 0; }

}  // namespace host
