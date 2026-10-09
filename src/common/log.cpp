// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace logging {

LogLevel g_level = LogLevel::Info;
static FILE *g_file = nullptr;
static std::mutex g_mutex;
void (*g_fatalHook)() = nullptr;

void SetFatalHook(void (*hook)()) { g_fatalHook = hook; }

void Init(LogLevel level, const char *filePath) {
	g_level = level;
	if (filePath && *filePath) {
		g_file = fopen(filePath, "w");
	}
}

void Shutdown() {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file) {
		fclose(g_file);
		g_file = nullptr;
	}
	fflush(stdout);
	fflush(stderr);
}

static const char *LevelName(LogLevel l) {
	switch (l) {
	case LogLevel::Trace: return "T";
	case LogLevel::Debug: return "D";
	case LogLevel::Info: return "I";
	case LogLevel::Warn: return "W";
	case LogLevel::Error: return "E";
	default: return "?";
	}
}

void Write(LogLevel level, const char *category, const char *fmt, ...) {
	char buf[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	std::lock_guard<std::mutex> lock(g_mutex);
	FILE *out = g_file ? g_file : stderr;
	fprintf(out, "[%s][%s] %s\n", LevelName(level), category, buf);
	if (g_file && level >= LogLevel::Warn) {
		fprintf(stderr, "[%s][%s] %s\n", LevelName(level), category, buf);
	}
}

void GuestOutput(const char *text, size_t len) {
	std::lock_guard<std::mutex> lock(g_mutex);
	fwrite(text, 1, len, stdout);
	fflush(stdout);
}

}  // namespace logging

void Fatal(const char *fmt, ...) {
	char buf[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	fprintf(stderr, "[FATAL] %s\n", buf);
	logging::Write(LogLevel::Error, "FATAL", "%s", buf);
	if (logging::g_fatalHook) logging::g_fatalHook();
	logging::Shutdown();
	std::_Exit(3);
}
