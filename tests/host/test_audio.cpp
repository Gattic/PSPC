// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/host/host.h"

#include <chrono>
#include <cstdio>
#include <vector>

int main() {
	host::Config cfg;
	cfg.windowScale = 1;
	cfg.vsync = false;
	return host::Run(cfg, [] {
		if (host::IsHeadless()) {
			std::fprintf(stderr, "SDL dummy video/audio initialization failed\n");
			return 1;
		}
		std::vector<s16> samples(512 * 2, 1);
		const auto start = std::chrono::steady_clock::now();
		// More than eleven seconds of audio must not block a real-time scheduler.
		int dropped = 0;
		for (int i = 0; i < 1000; ++i)
			if (!host::AudioSubmit(samples.data(), 512, false)) ++dropped;
		const auto elapsed = std::chrono::steady_clock::now() - start;
		if (!dropped || elapsed > std::chrono::seconds(2) || host::AudioQueuedFrames() > 4410 + 512) {
			std::fprintf(stderr, "real-time audio submission blocked or exceeded the queue bound\n");
			return 1;
		}
		const auto pacedStart = std::chrono::steady_clock::now();
		for (int i = 0; i < 20; ++i) {
			if (!host::AudioSubmit(samples.data(), 512, true)) {
				std::fprintf(stderr, "paced audio submission lost a block or has no device\n");
				return 1;
			}
		}
		if (std::chrono::steady_clock::now() - pacedStart < std::chrono::milliseconds(50)) {
			std::fprintf(stderr, "virtual-time audio no longer paces against the device\n");
			return 1;
		}
		std::puts("nonblocking bounded audio and virtual-time pacing passed");
		return 0;
	}, [] {});
}
