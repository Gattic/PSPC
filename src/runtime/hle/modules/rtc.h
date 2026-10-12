// SPDX-License-Identifier: GPL-2.0-or-later
// The guest's wall clock (hle/modules/rtc.cpp), shared by sceRtc and UtilsForUser's libc time.
#pragma once

#include "common/common.h"

namespace rtc {

// Microseconds since 1970-01-01 UTC: the host time at boot (whole seconds) plus guest time since.
u64 NowUnixUs();
// The host's standard (non-DST) offset from UTC, in microseconds.
s64 StandardOffsetUs();
// --rtc-base: boot at this time (seconds since 1970 UTC) instead of the host clock's, so runs with
// the same input repeat exactly (games seed their random numbers from the clock).
void SetBootTime(u64 unixSeconds);

}  // namespace rtc
