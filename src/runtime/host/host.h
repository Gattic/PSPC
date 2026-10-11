// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host platform: window + presentation, controller input and audio output. The emulation runs on
// its own thread; these functions are thread-safe. In headless mode everything is a no-op and
// input reads as "nothing pressed".
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/common.h"

namespace host {

struct Config {
	bool headless = false;
	int windowScale = 2;
	std::string title = "PSPC";
	bool vsync = true;
	bool fullscreen = false;  // borderless desktop fullscreen; F11 toggles
	int volume = 100;         // 0-100, applied to everything AudioSubmit queues
	// Input bindings, one comma-separated list per settings::kControls entry (SDL key names /
	// game controller names). Empty vectors mean the defaults.
	std::vector<std::string> keys, pads;
	std::string padStick = "left";  // controller stick driving the PSP analog: left, right, none
	// F12 saves the presented frame as <screenshotDir>/<gameName>_<timestamp>.bmp.
	std::string screenshotDir;
	std::string gameName = "pspc";
	// A GPU renderer may present to the window itself (SetFramePresenter): the host makes its own SDL
	// renderer only once a frame comes to it, so the two never share the window.
	bool gpuPresent = false;
};

struct CtrlState {
	u32 buttons = 0;  // PSP_CTRL_* bits
	u8 lx = 128, ly = 128;
};

// Runs `emulation` on a new thread and pumps the host event loop on the calling thread until it
// returns (or the window is closed, which calls onQuit). Returns the emulation's result.
int Run(const Config &cfg, const std::function<int()> &emulation, const std::function<void()> &onQuit);

// Emulation thread -> host.
void PresentFrame(const u32 *rgba, int width, int height, int stride);
CtrlState GetCtrl();
bool IsHeadless();

// The window's native handle (HWND) once Run has made it; nullptr headless.
void *NativeWindow();
// The smallest whole multiple of 480x272 that covers the picture in the window (at least 1); 0 headless.
int FitScale();
// A GPU renderer presenting to the window itself: PresentFrame hands it the frames (it returns false
// when it can't show one; the host presents that frame itself). Set from the emulation thread.
void SetFramePresenter(std::function<bool(const u32 *rgba, int width, int height, int stride)> presenter);
// With a frame presenter, F12 asks for the next presented frame: whoever has it saves it here.
bool ScreenshotRequested();
void SaveScreenshotIfRequested(const u32 *rgba, int width, int height);

// Audio: interleaved stereo s16 at 44100 Hz. Blocks (sleeps) while more than `maxQueuedMs` is
// already queued, which paces audio-driven games.
void AudioSubmit(const s16 *samples, int frames);
int AudioQueuedFrames();

}  // namespace host
