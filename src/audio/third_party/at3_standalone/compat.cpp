// Part of the standalone ATRAC3/ATRAC3+ decoder extracted from FFmpeg by the PPSSPP project
// (ext/at3_standalone). LGPL-2.1-or-later, see LICENSE in this directory.
// Modified for PSPC: logging goes to PSPC's logger instead of PPSSPP's.
#include <cstdarg>
#include <cstdio>

#include "compat.h"
#include "common/log.h"

void av_log(int level, const char *fmt, ...) {
	char buffer[512];
	va_list vl;
	va_start(vl, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, vl);
	va_end(vl);

	switch (level) {
	case AV_LOG_DEBUG:
	case AV_LOG_TRACE:
	case AV_LOG_VERBOSE: LOGD("ATRAC", "Atrac3/3+: %s", buffer); break;
	case AV_LOG_ERROR: LOGE("ATRAC", "Atrac3/3+: %s", buffer); break;
	case AV_LOG_INFO: LOGI("ATRAC", "Atrac3/3+: %s", buffer); break;
	case AV_LOG_WARNING:
	default: LOGW("ATRAC", "Atrac3/3+: %s", buffer); break;
	}
}
