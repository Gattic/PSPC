// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// sceRtc: the wall clock (host time at boot plus guest time since, so it runs with the guest
// clock) and the date/tick conversions and arithmetic, with the firmware's quirks as PPSSPP
// measured them.
#include "runtime/hle/modules/rtc.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

namespace {

// Microseconds between 0001-01-01 and 1970-01-01 (the PSP RTC epoch is year 1).
constexpr u64 kMagicOffset = 62135596800000000ULL;
// ... and between 0001-01-01 and 1601-01-01 (Win32 FILETIME).
constexpr u64 kFiletimeOffset = 50491123200000000ULL;
// The last time the clock was adjusted, and the "reincarnated" (battery) time: fixed dates.
constexpr u64 kLastAdjustedTicks = kMagicOffset + 41ULL * 365 * 24 * 3600 * 1000000ULL;
constexpr u64 kLastReincarnatedTicks = kMagicOffset + 40ULL * 365 * 24 * 3600 * 1000000ULL;

constexpr int PSP_TIME_INVALID_YEAR = -1;
constexpr int PSP_TIME_INVALID_MONTH = -2;
constexpr int PSP_TIME_INVALID_DAY = -3;
constexpr int PSP_TIME_INVALID_HOUR = -4;
constexpr int PSP_TIME_INVALID_MINUTES = -5;
constexpr int PSP_TIME_INVALID_SECONDS = -6;
constexpr int PSP_TIME_INVALID_MICROSECONDS = -7;

struct PspDateTime {
	s16 year, month, day, hour, minute, second;
	u32 microsecond;
};
static_assert(sizeof(PspDateTime) == 16, "ScePspDateTime");

bool g_baseSet = false;
u64 g_baseUnixUs = 0;  // host time at boot, whole seconds

u64 BaseUnixUs() {
	if (!g_baseSet) {
		const s64 us = std::chrono::duration_cast<std::chrono::microseconds>(
		                   std::chrono::system_clock::now().time_since_epoch())
		                   .count();
		g_baseUnixUs = (u64)(us / 1000000) * 1000000ULL;
		g_baseSet = true;
	}
	return g_baseUnixUs;
}

u64 CurrentTick() { return rtc::NowUnixUs() + kMagicOffset; }

bool Valid(u32 p, u32 size) { return mem::IsValidRange(p, size); }
bool Valid4(u32 p, u32 size) { return (p & 3) == 0 && mem::IsValidRange(p, size); }
u64 Read64(u32 p) { return GRead<u64>(p); }
void Write64(u32 p, u64 v) { GWrite<u64>(p, v); }
PspDateTime ReadTime(u32 p) {
	PspDateTime t{};
	mem::ReadBytes(&t, p, sizeof(t));
	return t;
}
void WriteTime(u32 p, const PspDateTime &t) { mem::WriteBytes(p, &t, sizeof(t)); }

void TmToPspTime(PspDateTime &t, const tm &v) {
	t.year = (s16)(v.tm_year + 1900);
	t.month = (s16)(v.tm_mon + 1);
	t.day = (s16)v.tm_mday;
	t.hour = (s16)v.tm_hour;
	t.minute = (s16)v.tm_min;
	t.second = (s16)v.tm_sec;
	t.microsecond = 0;
}

void PspTimeToTm(tm &v, const PspDateTime &pt) {
	v = tm{};
	v.tm_year = pt.year - 1900;
	v.tm_mon = pt.month - 1;
	v.tm_mday = pt.day;
	v.tm_wday = -1;
	v.tm_yday = -1;
	v.tm_hour = pt.hour;
	v.tm_min = pt.minute;
	v.tm_sec = pt.second;
	v.tm_isdst = 0;
}

// http://howardhinnant.github.io/date_algorithms.html (with the firmware-matching unsigned math).
s64 DaysFromCivil(s64 y, u32 m, u32 d) {
	y -= m <= 2;
	const s64 era = (y >= 0 ? y : y - 399) / 400;
	const u32 yoe = (u32)(y - era * 400);
	const u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (s64)doe - 719468;
}

void CivilFromDays(s64 z, s64 &outY, u32 &outM, u32 &outD) {
	z += 719468;
	const s64 era = (z >= 0 ? z : z - 146096) / 146097;
	const u32 doe = (u32)(z - era * 146097);
	const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const s64 y = (s64)yoe + era * 400;
	const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const u32 mp = (5 * doy + 2) / 153;
	outD = doy - (153 * mp + 2) / 5 + 1;
	outM = mp < 10 ? mp + 3 : mp - 9;
	outY = y + (outM <= 2);
}

void TicksToPspTime(PspDateTime &t, u64 ticks) {
	const u64 day = 24ull * 60ull * 60ull * 1000000ull;
	t.microsecond = (u32)(ticks % 1000000ull);
	t.second = (s16)(ticks / 1000000ull % 60ull);
	t.minute = (s16)(ticks / 1000000ull / 60ull % 60ull);
	t.hour = (s16)(ticks / 1000000ull / 60ull / 60ull % 24ull);
	const s64 z = (s64)(ticks / day) - (s64)(kMagicOffset / day);
	s64 y;
	u32 m, d;
	CivilFromDays(z, y, m, d);
	t.day = (s16)d;
	t.month = (s16)m;
	t.year = (s16)y;
}

u64 PspTimeToTicks(const PspDateTime &pt) {
	const s64 z = DaysFromCivil((s64)pt.year, (u32)pt.month, (u32)pt.day);
	return kMagicOffset + pt.microsecond +
	       1000000ull * (pt.second + 60ull * (pt.minute + 60ull * (pt.hour + 24ull * (u64)z)));
}

bool ValidatePspTime(const PspDateTime &t) { return t.year > 0 && t.year <= 9999; }

bool IsLeapYear(u32 year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

int DaysInMonth(u32 year, u32 month) {
	switch (month) {
	case 4:
	case 6:
	case 9:
	case 11: return 30;
	case 2: return IsLeapYear(year) ? 29 : 28;
	default: return 31;
	}
}

time_t TimeGm(tm *t) { return _mkgmtime(t); }

// ---------------------------------------------------------------- current time

u32 sceRtcGetTickResolution() { return 1000000; }

u32 sceRtcGetCurrentTick(u32 tickPtr) {
	const u64 tick = CurrentTick();
	if (Valid4(tickPtr, 8)) Write64(tickPtr, tick);
	sched::ChargeCycles(300);
	sched::Reschedule();
	return 0;
}

u64 sceRtcGetAccumulativeTime() {
	sched::ChargeCycles(300);
	sched::Reschedule();
	return CurrentTick();
}

u32 sceRtcGetCurrentClock(u32 ptr, int tz) {
	// The UTC time with tz minutes added (normalized as timegm would).
	const u64 tick = CurrentTick() + (u64)((s64)tz * 60000000LL);
	if (Valid(ptr, sizeof(PspDateTime))) {
		PspDateTime t{};
		TicksToPspTime(t, tick);
		WriteTime(ptr, t);
	}
	sched::ChargeCycles(1900);
	sched::Reschedule();
	return 0;
}

u32 sceRtcGetCurrentClockLocalTime(u32 ptr) {
	const u64 unixUs = rtc::NowUnixUs();
	const time_t sec = (time_t)(unixUs / 1000000ULL);
	tm local{};
	if (localtime_s(&local, &sec) != 0) return 0;  // too high/low to handle: pretend it worked
	if (Valid(ptr, sizeof(PspDateTime))) {
		PspDateTime t{};
		TmToPspTime(t, local);
		t.microsecond = (u32)(unixUs % 1000000ULL);
		WriteTime(ptr, t);
	}
	sched::ChargeCycles(2000);
	sched::Reschedule();
	return 0;
}

// ---------------------------------------------------------------- ticks and dates

u32 sceRtcSetTick(u32 ptr, u32 tickPtr) {
	if (!Valid(ptr, sizeof(PspDateTime)) || !Valid(tickPtr, 8)) return 0;
	PspDateTime t{};
	TicksToPspTime(t, Read64(tickPtr));
	WriteTime(ptr, t);
	return 0;
}

u32 sceRtcGetTick(u32 ptr, u32 tickPtr) {
	if (!Valid(ptr, sizeof(PspDateTime)) || !Valid(tickPtr, 8)) return 0;
	const PspDateTime t = ReadTime(ptr);
	if (!ValidatePspTime(t)) return SCE_KERNEL_ERROR_INVALID_VALUE;
	Write64(tickPtr, PspTimeToTicks(t));
	return 0;
}

u32 sceRtcGetDayOfWeek(u32 year, u32 month, u32 day) {
	// Month 0 counts as August.
	if (month == 0) month = 8;
	// Past month 12 the months go 31/31/30/31/30 and repeat.
	if (month > 12) {
		u32 rest = month - 12;
		const u32 grp5 = rest / 5;
		rest = rest % 5;
		day += grp5 * (31 * 3 + 30 * 2);
		static const u32 t[] = {31, 31 * 2, 31 * 2 + 30, 31 * 3 + 30, 31 * 3 + 30 * 2};
		if (rest > 0) day += t[rest - 1];
		month = 12;
	}
	return (u32)(((DaysFromCivil((s64)year, month, day) % 7LL) + 11LL) % 7LL);
}

u32 sceRtcGetDaysInMonth(u32 year, u32 month) {
	if (year == 0 || month == 0 || month > 12) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
	return (u32)DaysInMonth(year, month);
}

u32 sceRtcIsLeapYear(u32 year) { return IsLeapYear(year) ? 1 : 0; }

int sceRtcConvertLocalTimeToUTC(u32 localPtr, u32 utcPtr) {
	if (!Valid(localPtr, 8) || !Valid(utcPtr, 8)) return 1;
	Write64(utcPtr, Read64(localPtr) - (u64)rtc::StandardOffsetUs());
	return 0;
}

int sceRtcConvertUtcToLocalTime(u32 utcPtr, u32 localPtr) {
	if (!Valid(localPtr, 8) || !Valid(utcPtr, 8)) return 1;
	Write64(localPtr, Read64(utcPtr) + (u64)rtc::StandardOffsetUs());
	return 0;
}

int sceRtcCheckValid(u32 ptr) {
	if (!Valid(ptr, sizeof(PspDateTime))) return -1;
	const PspDateTime pt = ReadTime(ptr);
	if (pt.year < 1 || pt.year > 9999) return PSP_TIME_INVALID_YEAR;
	if (pt.month < 1 || pt.month > 12) return PSP_TIME_INVALID_MONTH;
	if (pt.day < 1 || pt.day > 31) return PSP_TIME_INVALID_DAY;
	if (pt.day > DaysInMonth((u32)(s16)pt.year, (u32)(s16)pt.month)) return PSP_TIME_INVALID_DAY;
	if (pt.hour < 0 || pt.hour > 23) return PSP_TIME_INVALID_HOUR;
	if (pt.minute < 0 || pt.minute > 59) return PSP_TIME_INVALID_MINUTES;
	if (pt.second < 0 || pt.second > 59) return PSP_TIME_INVALID_SECONDS;
	if (pt.microsecond >= 1000000UL) return PSP_TIME_INVALID_MICROSECONDS;
	return 0;
}

int sceRtcSetTime_t(u32 ptr, u32 time) {
	if (!Valid(ptr, sizeof(PspDateTime))) return 1;
	PspDateTime t{};
	TicksToPspTime(t, time * 1000000ULL + kMagicOffset);
	WriteTime(ptr, t);
	return 0;
}

int sceRtcSetTime64_t(u32 ptr, u64 time) {
	if (!Valid(ptr, sizeof(PspDateTime))) return 1;
	PspDateTime t{};
	TicksToPspTime(t, time * 1000000ULL + kMagicOffset);
	WriteTime(ptr, t);
	return 0;
}

int sceRtcGetTime_t(u32 ptr, u32 timePtr) {
	if (!Valid(ptr, sizeof(PspDateTime)) || !Valid(timePtr, 4)) return 1;
	GWrite32(timePtr, (u32)((PspTimeToTicks(ReadTime(ptr)) - kMagicOffset) / 1000000ULL));
	return 0;
}

int sceRtcGetTime64_t(u32 ptr, u32 timePtr) {
	if (!Valid(ptr, sizeof(PspDateTime)) || !Valid(timePtr, 8)) return 1;
	Write64(timePtr, (PspTimeToTicks(ReadTime(ptr)) - kMagicOffset) / 1000000ULL);
	return 0;
}

int sceRtcSetDosTime(u32 ptr, u32 dosTime) {
	if (!Valid(ptr, sizeof(PspDateTime))) return 1;
	const int hms = dosTime & 0xFFFF;
	const int ymd = dosTime >> 16;
	PspDateTime t{};
	t.year = (s16)(1980 + (ymd >> 9));
	t.month = (s16)((ymd >> 5) & 0xF);
	t.day = (s16)(ymd & 0x1F);
	t.hour = (s16)(hms >> 11);
	t.minute = (s16)((hms >> 5) & 0x3F);
	t.second = (s16)((hms << 1) & 0x3E);
	t.microsecond = 0;
	WriteTime(ptr, t);
	return 0;
}

int sceRtcGetDosTime(u32 ptr, u32 dosPtr) {
	if (!Valid(ptr, sizeof(PspDateTime)) || !Valid(dosPtr, 4)) return -1;
	const PspDateTime d = ReadTime(ptr);
	if (d.year < 1980) {
		GWrite32(dosPtr, 0);
		return -1;
	}
	if (d.year >= 2108) {
		GWrite32(dosPtr, 0xFF9FBF7D);
		return -1;
	}
	const int year = ((d.year - 1980) & 0x7F) << 9;
	const int month = (d.month & 0xF) << 5;
	const int hour = (d.hour & 0x1F) << 11;
	const int minute = (d.minute & 0x3F) << 5;
	const int day = d.day & 0x1F;
	const int second = (d.second >> 1) & 0x1F;
	GWrite32(dosPtr, (u32)(((year | month | day) << 16) | (hour | minute | second)));
	return 0;
}

int sceRtcSetWin32FileTime(u32 ptr, u64 win32Time) {
	if (!Valid(ptr, sizeof(PspDateTime))) return -1;
	PspDateTime t{};
	TicksToPspTime(t, win32Time / 10 + kFiletimeOffset);
	WriteTime(ptr, t);
	return 0;
}

int sceRtcGetWin32FileTime(u32 ptr, u32 win32TimePtr) {
	if (!Valid(ptr, sizeof(PspDateTime))) return -1;
	if (!Valid4(win32TimePtr, 8)) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	const PspDateTime t = ReadTime(ptr);
	const u64 result = PspTimeToTicks(t);
	if (!ValidatePspTime(t) || result < kFiletimeOffset) {
		Write64(win32TimePtr, 0);
		return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	}
	Write64(win32TimePtr, (result - kFiletimeOffset) * 10);
	return 0;
}

int sceRtcCompareTick(u32 aPtr, u32 bPtr) {
	if (Valid4(aPtr, 8) && Valid4(bPtr, 8)) {
		const u64 a = Read64(aPtr), b = Read64(bPtr);
		if (a > b) return 1;
		if (a < b) return -1;
	}
	return 0;
}

int TickAdd(u32 dst, u32 src, u64 delta) {
	if (Valid4(dst, 8) && Valid4(src, 8)) Write64(dst, Read64(src) + delta);
	return 0;
}
int sceRtcTickAddTicks(u32 d, u32 s, u64 n) { return TickAdd(d, s, n); }
int sceRtcTickAddMicroseconds(u32 d, u32 s, u64 n) { return TickAdd(d, s, n); }
int sceRtcTickAddSeconds(u32 d, u32 s, u64 n) { return TickAdd(d, s, n * 1000000UL); }
int sceRtcTickAddMinutes(u32 d, u32 s, u64 n) { return TickAdd(d, s, n * 60000000UL); }
int sceRtcTickAddHours(u32 d, u32 s, int n) { return TickAdd(d, s, n * 3600ULL * 1000000ULL); }
int sceRtcTickAddDays(u32 d, u32 s, int n) { return TickAdd(d, s, n * 86400ULL * 1000000ULL); }
int sceRtcTickAddWeeks(u32 d, u32 s, int n) { return TickAdd(d, s, n * 7ULL * 86400ULL * 1000000ULL); }

int sceRtcTickAddMonths(u32 dst, u32 src, int months) {
	if (!Valid4(dst, 8) || !Valid4(src, 8)) return -1;
	PspDateTime pt{};
	TicksToPspTime(pt, Read64(src));
	pt.year = (s16)(pt.year + months / 12);
	pt.month = (s16)(pt.month + months % 12);
	if (pt.month < 1) {
		pt.month += 12;
		pt.year--;
	}
	if (pt.month > 12) {
		pt.month -= 12;
		pt.year++;
	}
	if (ValidatePspTime(pt)) {
		// Landed on a year that isn't a leap year?
		if (pt.month == 2 && pt.day == 29 && !IsLeapYear((u32)(s16)pt.year)) pt.day = 28;
		Write64(dst, PspTimeToTicks(pt));
	}
	return 0;
}

int sceRtcTickAddYears(u32 dst, u32 src, int years) {
	if (!Valid4(dst, 8) || !Valid4(src, 8)) return -1;
	PspDateTime pt{};
	TicksToPspTime(pt, Read64(src));
	pt.year = (s16)(pt.year + years);
	if (ValidatePspTime(pt)) {
		if (pt.month == 2 && pt.day == 29 && !IsLeapYear((u32)(s16)pt.year)) pt.day = 28;
		Write64(dst, PspTimeToTicks(pt));
	}
	return 0;
}

// ---------------------------------------------------------------- parsing

struct ParseResult {
	PspDateTime date;
	int tzOffsetMinutes;
};

const char *ParseMonthName(const char *p, int &month) {
	static const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	for (int i = 0; i < 12; i++) {
		if (p[0] == names[i][0] && p[1] == names[i][1] && p[2] == names[i][2]) {
			month = i + 1;
			return p + 3;
		}
	}
	return nullptr;
}

bool ParseDigits(const char *&p, int n, int &value) {
	value = 0;
	for (int i = 0; i < n; i++) {
		if (p[i] < '0' || p[i] > '9') return false;
		value = value * 10 + (p[i] - '0');
	}
	p += n;
	return true;
}

bool ParseRFC3339(const char *s, ParseResult &r) {
	r = ParseResult{};
	int year, month, day, hour, minute, second, micro = 0;
	if (!ParseDigits(s, 4, year) || *s++ != '-') return false;
	if (!ParseDigits(s, 2, month) || *s++ != '-') return false;
	if (!ParseDigits(s, 2, day)) return false;
	if (*s++ != 'T') return false;
	if (!ParseDigits(s, 2, hour) || *s++ != ':') return false;
	if (!ParseDigits(s, 2, minute) || *s++ != ':') return false;
	if (!ParseDigits(s, 2, second)) return false;
	if (*s == '.') {
		s++;
		int frac = 0, digits = 0;
		while (*s >= '0' && *s <= '9' && digits < 6) {
			frac = frac * 10 + (*s - '0');
			digits++;
			s++;
		}
		while (digits < 6) {
			frac *= 10;
			digits++;
		}
		micro = frac;
	}
	if (*s == 'Z') {
		s++;
		r.tzOffsetMinutes = 0;
	} else if (*s == '+' || *s == '-') {
		const int sign = *s == '+' ? 1 : -1;
		s++;
		int tzHour, tzMin;
		if (!ParseDigits(s, 2, tzHour)) return false;
		if (*s != ':') return false;
		s++;
		if (!ParseDigits(s, 2, tzMin)) return false;
		r.tzOffsetMinutes = sign * (tzHour * 60 + tzMin);
	} else {
		return false;
	}
	if (*s != '\0') return false;
	r.date.year = (s16)year;
	r.date.month = (s16)month;
	r.date.day = (s16)day;
	r.date.hour = (s16)hour;
	r.date.minute = (s16)minute;
	r.date.second = (s16)second;
	r.date.microsecond = (u32)micro;
	return true;
}

bool ParseRFC2822(const char *s, ParseResult &r) {
	r = ParseResult{};
	// [weekday,] - required here
	while (*s && *s != ' ' && *s != ',') s++;
	if (*s != ',') return false;
	s++;
	if (*s != ' ') return false;
	s++;
	int day, month, year, hour, minute, second;
	if (!ParseDigits(s, 2, day)) return false;
	if (*s++ != ' ') return false;
	if (!ParseMonthName(s, month)) return false;
	s += 3;
	if (*s++ != ' ') return false;
	if (!ParseDigits(s, 4, year)) return false;
	if (*s++ != ' ') return false;
	if (!ParseDigits(s, 2, hour) || *s++ != ':') return false;
	if (!ParseDigits(s, 2, minute) || *s++ != ':') return false;
	if (!ParseDigits(s, 2, second)) return false;
	if (*s++ != ' ') return false;
	if (*s != '+' && *s != '-') return false;
	const int sign = *s == '+' ? 1 : -1;
	s++;
	int tzHour, tzMin;
	if (!ParseDigits(s, 2, tzHour)) return false;
	if (!ParseDigits(s, 2, tzMin)) return false;
	r.tzOffsetMinutes = sign * (tzHour * 60 + tzMin);
	if (*s != '\0') return false;
	r.date.year = (s16)year;
	r.date.month = (s16)month;
	r.date.day = (s16)day;
	r.date.hour = (s16)hour;
	r.date.minute = (s16)minute;
	r.date.second = (s16)second;
	return true;
}

int sceRtcParseDateTime(u32 dstTickPtr, u32 strPtr) {
	if (!Valid4(dstTickPtr, 8) || !Valid(strPtr, 1)) return -1;
	std::string str;
	for (u32 a = strPtr; mem::IsValidRange(a, 1) && str.size() < 256; a++) {
		const char ch = (char)GRead8(a);
		if (!ch) break;
		str.push_back(ch);
	}
	ParseResult r;
	if (ParseRFC3339(str.c_str(), r) || ParseRFC2822(str.c_str(), r)) {
		u64 ticks = PspTimeToTicks(r.date);
		ticks -= (u64)((s64)r.tzOffsetMinutes * 60 * 1000000);
		Write64(dstTickPtr, ticks);
		return 0;
	}
	// Parse failed: -1 without touching the tick.
	return -1;
}

// ---------------------------------------------------------------- formatting

void PrepareTm(tm &local, u32 srcTickPtr, int tz, PspDateTime &pt) {
	pt = PspDateTime{};
	TicksToPspTime(pt, Read64(srcTickPtr));
	PspTimeToTm(local, pt);
	while (local.tm_year < 70) local.tm_year += 400;
	while (local.tm_year >= 470) local.tm_year -= 400;
	local.tm_min += tz;
	TimeGm(&local);
}

int FormatRFC2822(u32 outPtr, u32 srcTickPtr, int tz) {
	PspDateTime pt;
	tm local;
	PrepareTm(local, srcTickPtr, tz, pt);
	char buf[32] = {};
	char *out = buf, *end = buf + 32;
	out += strftime(out, end - out, "%a, %d %b ", &local);
	out += snprintf(out, end - out, "%04d", pt.year);
	out += strftime(out, end - out, " %H:%M:%S ", &local);
	if (tz < 0) snprintf(out, end - out, "-%02d%02d", -tz / 60, -tz % 60);
	else snprintf(out, end - out, "+%02d%02d", tz / 60, tz % 60);
	mem::WriteBytes(outPtr, buf, (u32)std::min<size_t>(strlen(buf) + 1, sizeof(buf)));
	return 0;
}

int FormatRFC3339(u32 outPtr, u32 srcTickPtr, int tz) {
	PspDateTime pt;
	tm local;
	PrepareTm(local, srcTickPtr, tz, pt);
	char buf[32] = {};
	char *out = buf, *end = buf + 32;
	out += snprintf(out, end - out, "%04d", pt.year);
	out += strftime(out, end - out, "-%m-%dT%H:%M:%S.00", &local);
	if (tz == 0) snprintf(out, end - out, "Z");
	else if (tz < 0) snprintf(out, end - out, "-%02d:%02d", -tz / 60, -tz % 60);
	else snprintf(out, end - out, "+%02d:%02d", tz / 60, tz % 60);
	mem::WriteBytes(outPtr, buf, (u32)std::min<size_t>(strlen(buf) + 1, sizeof(buf)));
	return 0;
}

bool FormatArgsValid(u32 outPtr, u32 srcTickPtr) { return Valid(outPtr, 32) && Valid4(srcTickPtr, 8); }

int sceRtcFormatRFC2822(u32 o, u32 t, int tz) { return FormatArgsValid(o, t) ? FormatRFC2822(o, t, tz) : -1; }
int sceRtcFormatRFC3339(u32 o, u32 t, int tz) { return FormatArgsValid(o, t) ? FormatRFC3339(o, t, tz) : -1; }
int sceRtcFormatRFC2822LocalTime(u32 o, u32 t) {
	return FormatArgsValid(o, t) ? FormatRFC2822(o, t, (int)(rtc::StandardOffsetUs() / 60000000)) : -1;
}
int sceRtcFormatRFC3339LocalTime(u32 o, u32 t) {
	return FormatArgsValid(o, t) ? FormatRFC3339(o, t, (int)(rtc::StandardOffsetUs() / 60000000)) : -1;
}

// ---------------------------------------------------------------- fixed times and the alarm

int sceRtcGetLastAdjustedTime(u32 tickPtr) {
	if (Valid4(tickPtr, 8)) Write64(tickPtr, kLastAdjustedTicks);
	return 0;
}
int sceRtcGetLastReincarnatedTime(u32 tickPtr) {
	if (Valid4(tickPtr, 8)) Write64(tickPtr, kLastReincarnatedTicks);
	return 0;
}
// No alarm is modelled: setting one succeeds, none ever fires, and the alarm tick reads 0.
int sceRtcSetAlarmTick(u32, u32) { return 0; }
int sceRtcIsAlarmed() { return 0; }
int sceRtcGetAlarmTick(u32 tickPtr) {
	if (Valid(tickPtr, 8)) Write64(tickPtr, 0);
	return 0;
}

// The XMB registers a callback for clock changes (alarms, time set); the clock never changes
// under it here, so it is only remembered.
SceUID g_rtcCallback = 0;
int sceRtcRegisterCallback(SceUID cb) {
	g_rtcCallback = cb;
	return 0;
}
int sceRtcUnregisterCallback(SceUID cb) {
	if (cb == g_rtcCallback) g_rtcCallback = 0;
	return 0;
}

}  // namespace

namespace rtc {

u64 NowUnixUs() { return BaseUnixUs() + sched::NowUs(); }

s64 StandardOffsetUs() {
	_tzset();
	long tz = 0;
	_get_timezone(&tz);
	return -(s64)tz * 1000000LL;
}

}  // namespace rtc

void Register_sceRtc() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xC41C2853, sceRtcGetTickResolution),
		HLEFN(0x3F7AD767, sceRtcGetCurrentTick),
		HLEFN(0x011F03C1, sceRtcGetAccumulativeTime),
		HLEFN(0x029CA3B3, sceRtcGetAccumulativeTime),
		HLEFN(0x4CFA57B0, sceRtcGetCurrentClock),
		HLEFN(0xE7C27D1B, sceRtcGetCurrentClockLocalTime),
		HLEFN(0x34885E0D, sceRtcConvertUtcToLocalTime),
		HLEFN(0x779242A2, sceRtcConvertLocalTimeToUTC),
		HLEFN(0x42307A17, sceRtcIsLeapYear),
		HLEFN(0x05EF322C, sceRtcGetDaysInMonth),
		HLEFN(0x57726BC1, sceRtcGetDayOfWeek),
		HLEFN(0x4B1B5E82, sceRtcCheckValid),
		HLEFN(0x3A807CC8, sceRtcSetTime_t),
		HLEFN(0x27C4594C, sceRtcGetTime_t),
		HLEFN(0xF006F264, sceRtcSetDosTime),
		HLEFN(0x36075567, sceRtcGetDosTime),
		HLEFN(0x7ACE4C04, sceRtcSetWin32FileTime),
		HLEFN(0xCF561893, sceRtcGetWin32FileTime),
		HLEFN(0x7ED29E40, sceRtcSetTick),
		HLEFN(0x6FF40ACC, sceRtcGetTick),
		HLEFN(0x9ED0AE87, sceRtcCompareTick),
		HLEFN(0x44F45E05, sceRtcTickAddTicks),
		HLEFN(0x26D25A5D, sceRtcTickAddMicroseconds),
		HLEFN(0xF2A4AFE5, sceRtcTickAddSeconds),
		HLEFN(0xE6605BCA, sceRtcTickAddMinutes),
		HLEFN(0x26D7A24A, sceRtcTickAddHours),
		HLEFN(0xE51B4B7A, sceRtcTickAddDays),
		HLEFN(0xCF3A2CA8, sceRtcTickAddWeeks),
		HLEFN(0xDBF74F1B, sceRtcTickAddMonths),
		HLEFN(0x42842C77, sceRtcTickAddYears),
		HLEFN(0xC663B3B9, sceRtcFormatRFC2822),
		HLEFN(0x7DE6711B, sceRtcFormatRFC2822LocalTime),
		HLEFN(0x0498FB3C, sceRtcFormatRFC3339),
		HLEFN(0x27F98543, sceRtcFormatRFC3339LocalTime),
		HLEFN(0xDFBC5F16, sceRtcParseDateTime),
		HLEFN(0xE1C93E47, sceRtcGetTime64_t),
		HLEFN(0x1909C99B, sceRtcSetTime64_t),
		HLEFN(0x62685E98, sceRtcGetLastAdjustedTime),
		HLEFN(0x203CEB0D, sceRtcGetLastReincarnatedTime),
		HLEFN(0x7D1FBED3, sceRtcSetAlarmTick),
		HLEFN(0x81FCDA34, sceRtcIsAlarmed),
		HLEFN(0xC2DDBEB5, sceRtcGetAlarmTick),
		HLEFN(0xFB3B18CD, sceRtcRegisterCallback),
		HLEFN(0x6A676D2D, sceRtcUnregisterCallback),
	};
	hle::RegisterModule("sceRtc", funcs);
	// The kernel-only alias: the same functions, plus sceRtcSetAlarmTick under the NIDs later
	// firmwares renumbered it to (3.71 ... 6.39), as PPSSPP lists them.
	static const hle::HleFunction driver[] = {
		HLEFN(0xE09880CF, sceRtcSetAlarmTick), HLEFN(0x54B9C589, sceRtcSetAlarmTick),
		HLEFN(0x68AED59A, sceRtcSetAlarmTick), HLEFN(0xADAF231F, sceRtcSetAlarmTick),
		HLEFN(0x55AC1C23, sceRtcSetAlarmTick), HLEFN(0x827BCB3F, sceRtcSetAlarmTick),
		HLEFN(0x329E8E3A, sceRtcSetAlarmTick),
	};
	hle::RegisterModule("sceRtc_driver", funcs);
	hle::RegisterModule("sceRtc_driver", driver);
}
