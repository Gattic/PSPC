// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/common.h"

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, None };

namespace logging {

extern LogLevel g_level;

void Init(LogLevel level, const char *filePath = nullptr);
void Shutdown();
void Write(LogLevel level, const char *category, const char *fmt, ...);
// Raw text written to stdout without decoration (used for guest program output).
void GuestOutput(const char *text, size_t len);
// Called by Fatal() after the message, before exiting (the runtime prints a host backtrace).
void SetFatalHook(void (*hook)());

}  // namespace logging

[[noreturn]] void Fatal(const char *fmt, ...);

#define LOG_AT(lvl, cat, ...)                                         \
	do {                                                              \
		if ((int)(lvl) >= (int)logging::g_level)                      \
			logging::Write((lvl), (cat), __VA_ARGS__);                \
	} while (0)

#define LOGT(cat, ...) LOG_AT(LogLevel::Trace, cat, __VA_ARGS__)
#define LOGD(cat, ...) LOG_AT(LogLevel::Debug, cat, __VA_ARGS__)
#define LOGI(cat, ...) LOG_AT(LogLevel::Info, cat, __VA_ARGS__)
#define LOGW(cat, ...) LOG_AT(LogLevel::Warn, cat, __VA_ARGS__)
#define LOGE(cat, ...) LOG_AT(LogLevel::Error, cat, __VA_ARGS__)
