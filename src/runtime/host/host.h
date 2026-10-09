// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host platform: window + presentation, controller input and audio output. The emulation runs on
// its own thread; these functions are thread-safe. In headless mode everything is a no-op and
// input reads as "nothing pressed".
#pragma once

#include <functional>
#include <string>

#include "common/common.h"

namespace host {

struct Config {
	bool headless = false;
	int windowScale = 2;
	std::string title = "PSPC";
	bool vsync = true;
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

// Audio: interleaved stereo s16 at 44100 Hz. Blocks (sleeps) while more than `maxQueuedMs` is
// already queued, which paces audio-driven games.
void AudioSubmit(const s16 *samples, int frames);
int AudioQueuedFrames();

}  // namespace host
