// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small system libraries: UtilsForUser, scePower, sceSuspendForUser, sceRtc, sceUmdUser,
// sceImpose, sceDmac, sceHprm.
#include <chrono>
#include <ctime>
#include <map>
#include <random>

#include "common/log.h"
#include "common/sha1.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/io/io.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace {

// ---------------------------------------------------------------- wall clock

// Microseconds between 0001-01-01 and 1970-01-01 (the PSP RTC epoch is year 1).
constexpr u64 kRtcUnixOffset = 62135596800000000ULL;
u64 g_bootUnixUs = 0;
u64 g_bootSchedUs = 0;

u64 NowUnixUs() {
	if (!g_bootUnixUs) {
		g_bootUnixUs = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
		                   std::chrono::system_clock::now().time_since_epoch())
		                   .count();
		g_bootSchedUs = sched::NowUs();
	}
	return g_bootUnixUs + (sched::NowUs() - g_bootSchedUs);
}
u64 NowRtcTick() { return NowUnixUs() + kRtcUnixOffset; }

s64 LocalOffsetUs() {
	time_t t = (time_t)(NowUnixUs() / 1000000);
	tm lt{}, gt{};
	localtime_s(&lt, &t);
	gmtime_s(&gt, &t);
	lt.tm_isdst = 0;
	gt.tm_isdst = 0;
	return (s64)difftime(mktime(&lt), mktime(&gt)) * 1000000;
}

// ---------------------------------------------------------------- UtilsForUser

u32 sceKernelLibcTime(u32 tPtr) {
	u32 t = (u32)(NowUnixUs() / 1000000);
	if (mem::IsValidRange(tPtr, 4)) GWrite32(tPtr, t);
	return t;
}
u32 sceKernelLibcClock() { return (u32)sched::NowUs(); }
u32 sceKernelLibcGettimeofday(u32 tvPtr, u32 tzPtr) {
	u64 now = NowUnixUs();
	if (mem::IsValidRange(tvPtr, 8)) {
		GWrite32(tvPtr, (u32)(now / 1000000));
		GWrite32(tvPtr + 4, (u32)(now % 1000000));
	}
	if (mem::IsValidRange(tzPtr, 8)) {
		GWrite32(tzPtr, (u32)(-LocalOffsetUs() / 60000000));
		GWrite32(tzPtr + 4, 0);
	}
	return 0;
}
int sceKernelDcacheNop(u32, u32) { return 0; }
int sceKernelDcacheNop0() { return 0; }

// MD5 (RFC 1321).
struct Md5 {
	u32 h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	u8 buf[64];
	u64 len = 0;
	size_t used = 0;
	static u32 Rol(u32 x, int c) { return (x << c) | (x >> (32 - c)); }
	void Block(const u8 *p) {
		static const u32 K[64] = {
			0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
			0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
			0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
			0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
			0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
			0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
			0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
			0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
		static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
		                          14, 20, 5, 9, 14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		                          4,  11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
		u32 m[16];
		for (int i = 0; i < 16; i++) m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((u32)p[i * 4 + 3] << 24);
		u32 a = h[0], b = h[1], c = h[2], d = h[3];
		for (int i = 0; i < 64; i++) {
			u32 f;
			int g;
			if (i < 16) {
				f = (b & c) | (~b & d);
				g = i;
			} else if (i < 32) {
				f = (d & b) | (~d & c);
				g = (5 * i + 1) % 16;
			} else if (i < 48) {
				f = b ^ c ^ d;
				g = (3 * i + 5) % 16;
			} else {
				f = c ^ (b | ~d);
				g = (7 * i) % 16;
			}
			u32 tmp = d;
			d = c;
			c = b;
			b = b + Rol(a + f + K[i] + m[g], R[i]);
			a = tmp;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
	}
	void Update(const u8 *p, size_t n) {
		len += n;
		while (n) {
			size_t take = std::min(n, 64 - used);
			memcpy(buf + used, p, take);
			used += take;
			p += take;
			n -= take;
			if (used == 64) {
				Block(buf);
				used = 0;
			}
		}
	}
	void Final(u8 out[16]) {
		u64 bits = len * 8;
		u8 pad = 0x80, z = 0;
		Update(&pad, 1);
		while (used != 56) Update(&z, 1);
		u8 l[8];
		for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (8 * i));
		Update(l, 8);
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++) out[i * 4 + j] = (u8)(h[i] >> (8 * j));
	}
};

int sceKernelUtilsMd5Digest(u32 data, u32 size, u32 digest) {
	if (!mem::IsValidRange(data, size) || !mem::IsValidRange(digest, 16)) return -1;
	Md5 m;
	m.Update(GPtr(data), size);
	u8 out[16];
	m.Final(out);
	mem::WriteBytes(digest, out, 16);
	return 0;
}
// Block-mode contexts live in guest memory; keep host state keyed by context address.
std::map<u32, Md5> g_md5;
std::map<u32, Sha1> g_sha1;
int sceKernelUtilsMd5BlockInit(u32 ctx) {
	g_md5[ctx] = Md5();
	return 0;
}
int sceKernelUtilsMd5BlockUpdate(u32 ctx, u32 data, u32 size) {
	if (!mem::IsValidRange(data, size)) return -1;
	g_md5[ctx].Update(GPtr(data), size);
	return 0;
}
int sceKernelUtilsMd5BlockResult(u32 ctx, u32 digest) {
	u8 out[16];
	g_md5[ctx].Final(out);
	g_md5.erase(ctx);
	mem::WriteBytes(digest, out, 16);
	return 0;
}
int sceKernelUtilsSha1Digest(u32 data, u32 size, u32 digest) {
	if (!mem::IsValidRange(data, size) || !mem::IsValidRange(digest, 20)) return -1;
	auto h = Sha1::Hash(GPtr(data), size);
	mem::WriteBytes(digest, h.data(), 20);
	return 0;
}
int sceKernelUtilsSha1BlockInit(u32 ctx) {
	g_sha1[ctx] = Sha1();
	return 0;
}
int sceKernelUtilsSha1BlockUpdate(u32 ctx, u32 data, u32 size) {
	if (!mem::IsValidRange(data, size)) return -1;
	g_sha1[ctx].Update(GPtr(data), size);
	return 0;
}
int sceKernelUtilsSha1BlockResult(u32 ctx, u32 digest) {
	auto h = g_sha1[ctx].Final();
	g_sha1.erase(ctx);
	mem::WriteBytes(digest, h.data(), 20);
	return 0;
}

// MT19937 state in the guest context buffer (624 words + index), like the firmware.
int sceKernelUtilsMt19937Init(u32 ctx, u32 seed) {
	if (!mem::IsValidRange(ctx, 625 * 4)) return -1;
	u32 *mt = (u32 *)GPtr(ctx);
	mt[0] = seed;
	for (u32 i = 1; i < 624; i++) mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
	mt[624] = 624;
	return 0;
}
u32 sceKernelUtilsMt19937UInt(u32 ctx) {
	if (!mem::IsValidRange(ctx, 625 * 4)) return 0;
	u32 *mt = (u32 *)GPtr(ctx);
	u32 &idx = mt[624];
	if (idx >= 624) {
		for (int i = 0; i < 624; i++) {
			u32 y = (mt[i] & 0x80000000u) | (mt[(i + 1) % 624] & 0x7FFFFFFFu);
			mt[i] = mt[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0);
		}
		idx = 0;
	}
	u32 y = mt[idx++];
	y ^= y >> 11;
	y ^= (y << 7) & 0x9D2C5680u;
	y ^= (y << 15) & 0xEFC60000u;
	y ^= y >> 18;
	return y;
}
u32 sceKernelGetGPI() { return 0; }
int sceKernelSetGPO(u32) { return 0; }

// ---------------------------------------------------------------- scePower / sceSuspend

int g_cpuMhz = 222, g_busMhz = 111, g_pllMhz = 222;
SceUID g_powerCallbacks[16] = {};
constexpr u32 PSP_POWER_CB_AC_POWER = 0x00001000;
constexpr u32 PSP_POWER_CB_BATTERY_EXIST = 0x00000080;
constexpr u32 kPowerState = PSP_POWER_CB_AC_POWER | PSP_POWER_CB_BATTERY_EXIST | 100;

int scePowerRegisterCallback(int slot, SceUID cb) {
	if (slot < -1 || slot >= 16) return (int)SCE_KERNEL_ERROR_INVALID_INDEX;
	if (!Get<Callback>(cb)) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	if (slot == -1) {
		for (int i = 0; i < 16; i++)
			if (!g_powerCallbacks[i]) {
				slot = i;
				break;
			}
		if (slot == -1) return (int)SCE_KERNEL_ERROR_OUT_OF_MEMORY;
	} else if (g_powerCallbacks[slot]) {
		return (int)SCE_KERNEL_ERROR_ALREADY;
	}
	g_powerCallbacks[slot] = cb;
	NotifyCallback(cb, (s32)kPowerState);
	return slot;
}
int scePowerUnregisterCallback(int slot) {
	if (slot < 0 || slot >= 16) return (int)SCE_KERNEL_ERROR_INVALID_INDEX;
	if (!g_powerCallbacks[slot]) return (int)SCE_KERNEL_ERROR_NOT_FOUND;
	g_powerCallbacks[slot] = 0;
	return 0;
}
int scePowerTick(int) { return 0; }
int scePowerIsPowerOnline() { return 1; }
int scePowerIsBatteryExist() { return 1; }
int scePowerIsBatteryCharging() { return 0; }
int scePowerGetBatteryChargingStatus() { return 0; }
int scePowerIsLowBattery() { return 0; }
int scePowerIsSuspendRequired() { return 0; }
int scePowerGetBatteryLifePercent() { return 100; }
int scePowerGetBatteryLifeTime() { return 300; }
int scePowerGetBatteryTemp() { return 28; }
int scePowerCancelRequest() { return 0; }
int scePowerRequestStandby() { return 0; }
int scePowerRequestSuspend() { return 0; }
int scePowerSetCpuClockFrequency(int mhz) {
	if (mhz < 1 || mhz > 333) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	g_cpuMhz = mhz;
	return 0;
}
int scePowerSetBusClockFrequency(int mhz) {
	if (mhz < 1 || mhz > 167) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	g_busMhz = mhz;
	return 0;
}
int scePowerSetClockFrequency(int pll, int cpu, int bus) {
	if (pll < 19 || pll > 333 || cpu < 1 || cpu > 333 || bus < 1 || bus > 167 || cpu > pll)
		return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	g_pllMhz = pll;
	g_cpuMhz = cpu;
	g_busMhz = bus;
	return 0;
}
int scePowerGetCpuClockFrequencyInt() { return g_cpuMhz; }
int scePowerGetBusClockFrequencyInt() { return g_busMhz; }
int scePowerGetPllClockFrequencyInt() { return g_pllMhz; }
float scePowerGetCpuClockFrequencyFloat() { return (float)g_cpuMhz; }
float scePowerGetBusClockFrequencyFloat() { return (float)g_busMhz; }
float scePowerGetPllClockFrequencyFloat() { return (float)g_pllMhz; }
int scePowerCheckWlanCoexistenceClock() { return 0; }

bool g_volatileLocked = false;
int VolatileLock(int type, u32 ptrAddr, u32 sizeAddr, bool tryOnly) {
	if (type != 0) return (int)SCE_KERNEL_ERROR_INVALID_MODE;
	if (g_volatileLocked) {
		if (tryOnly) return (int)SCE_KERNEL_ERROR_POWER_VMEM_IN_USE;
		// Real firmware blocks; a second lock in practice is a bug, so fail like a try.
		return (int)SCE_KERNEL_ERROR_POWER_VMEM_IN_USE;
	}
	g_volatileLocked = true;
	if (mem::IsValidRange(ptrAddr, 4)) GWrite32(ptrAddr, 0x08400000);
	if (mem::IsValidRange(sizeAddr, 4)) GWrite32(sizeAddr, 0x00400000);
	return 0;
}
int sceKernelVolatileMemLock(int type, u32 p, u32 s) { return VolatileLock(type, p, s, false); }
int sceKernelVolatileMemTryLock(int type, u32 p, u32 s) { return VolatileLock(type, p, s, true); }
int sceKernelVolatileMemUnlock(int type) {
	if (type != 0) return (int)SCE_KERNEL_ERROR_INVALID_MODE;
	if (!g_volatileLocked) return (int)SCE_KERNEL_ERROR_SEMA_OVF;
	g_volatileLocked = false;
	return 0;
}
int sceKernelPowerLock(int) { return 0; }
int sceKernelPowerUnlock(int) { return 0; }
int sceKernelPowerTick(int) { return 0; }

// ---------------------------------------------------------------- sceRtc

#pragma pack(push, 1)
struct PspTime {
	u16 year, month, day, hour, minute, second;
	u32 microsecond;
};
#pragma pack(pop)

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int DaysInMonth(int y, int m) {
	static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (m < 1 || m > 12) return 0;
	return d[m - 1] + (m == 2 && IsLeap(y) ? 1 : 0);
}
// Days since 0001-01-01 (proleptic Gregorian).
s64 DaysFromCivil(s64 y, int m, int d) {
	y -= m <= 2;
	s64 era = (y >= 0 ? y : y - 399) / 400;
	s64 yoe = y - era * 400;
	s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468 + 719162;  // shift epoch from 1970 to year 1
}
void CivilFromDays(s64 z, s64 &y, int &m, int &d) {
	z += 719468 - 719162;
	s64 era = (z >= 0 ? z : z - 146096) / 146097;
	s64 doe = z - era * 146097;
	s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	s64 mp = (5 * doy + 2) / 153;
	d = (int)(doy - (153 * mp + 2) / 5 + 1);
	m = (int)(mp < 10 ? mp + 3 : mp - 9);
	y += m <= 2;
}
u64 TimeToTick(const PspTime &t) {
	s64 days = DaysFromCivil(t.year, t.month, t.day);
	return (u64)days * 86400000000ULL + (u64)t.hour * 3600000000ULL + (u64)t.minute * 60000000ULL +
	       (u64)t.second * 1000000ULL + t.microsecond;
}
PspTime TickToTime(u64 tick) {
	PspTime t{};
	s64 days = (s64)(tick / 86400000000ULL);
	u64 rem = tick % 86400000000ULL;
	s64 y;
	int m, d;
	CivilFromDays(days, y, m, d);
	t.year = (u16)y;
	t.month = (u16)m;
	t.day = (u16)d;
	t.hour = (u16)(rem / 3600000000ULL);
	rem %= 3600000000ULL;
	t.minute = (u16)(rem / 60000000ULL);
	rem %= 60000000ULL;
	t.second = (u16)(rem / 1000000ULL);
	t.microsecond = (u32)(rem % 1000000ULL);
	return t;
}

u32 sceRtcGetTickResolution() { return 1000000; }
int sceRtcGetCurrentTick(u32 tickPtr) {
	if (mem::IsValidRange(tickPtr, 8)) GWrite<u64>(tickPtr, NowRtcTick());
	return 0;
}
u64 sceRtcGetAccumulativeTime() { return NowRtcTick(); }
int sceRtcGetCurrentClock(u32 timePtr, int tz) {
	PspTime t = TickToTime(NowRtcTick() + (s64)tz * 60000000LL);
	if (mem::IsValidRange(timePtr, sizeof(t))) mem::WriteBytes(timePtr, &t, sizeof(t));
	return 0;
}
int sceRtcGetCurrentClockLocalTime(u32 timePtr) {
	PspTime t = TickToTime(NowRtcTick() + LocalOffsetUs());
	if (mem::IsValidRange(timePtr, sizeof(t))) mem::WriteBytes(timePtr, &t, sizeof(t));
	return 0;
}
int sceRtcConvertUtcToLocalTime(u32 utcPtr, u32 localPtr) {
	if (!mem::IsValidRange(utcPtr, 8) || !mem::IsValidRange(localPtr, 8)) return -1;
	GWrite<u64>(localPtr, GRead<u64>(utcPtr) + LocalOffsetUs());
	return 0;
}
int sceRtcConvertLocalTimeToUTC(u32 localPtr, u32 utcPtr) {
	if (!mem::IsValidRange(utcPtr, 8) || !mem::IsValidRange(localPtr, 8)) return -1;
	GWrite<u64>(utcPtr, GRead<u64>(localPtr) - LocalOffsetUs());
	return 0;
}
int sceRtcIsLeapYear(int y) { return IsLeap(y) ? 1 : 0; }
int sceRtcGetDaysInMonth(int y, int m) {
	int d = DaysInMonth(y, m);
	return d ? d : (int)SCE_KERNEL_ERROR_INVALID_VALUE;
}
int sceRtcGetDayOfWeek(int y, int m, int d) {
	s64 days = DaysFromCivil(y, m, d);
	return (int)((days + 1) % 7);  // 0001-01-01 was a Monday
}
int sceRtcCheckValid(u32 timePtr) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime))) return -1;
	PspTime t;
	mem::ReadBytes(&t, timePtr, sizeof(t));
	if (t.year < 1 || t.year > 9999) return -1;
	if (t.month < 1 || t.month > 12) return -2;
	if (t.day < 1 || t.day > DaysInMonth(t.year, t.month)) return -3;
	if (t.hour > 23) return -4;
	if (t.minute > 59) return -5;
	if (t.second > 59) return -6;
	if (t.microsecond >= 1000000) return -7;
	return 0;
}
int sceRtcSetTick(u32 timePtr, u32 tickPtr) {
	if (!mem::IsValidRange(tickPtr, 8) || !mem::IsValidRange(timePtr, sizeof(PspTime))) return -1;
	PspTime t = TickToTime(GRead<u64>(tickPtr));
	mem::WriteBytes(timePtr, &t, sizeof(t));
	return 0;
}
int sceRtcGetTick(u32 timePtr, u32 tickPtr) {
	if (!mem::IsValidRange(tickPtr, 8) || !mem::IsValidRange(timePtr, sizeof(PspTime))) return -1;
	PspTime t;
	mem::ReadBytes(&t, timePtr, sizeof(t));
	if (sceRtcCheckValid(timePtr) < 0) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	GWrite<u64>(tickPtr, TimeToTick(t));
	return 0;
}
int sceRtcCompareTick(u32 aPtr, u32 bPtr) {
	if (!mem::IsValidRange(aPtr, 8) || !mem::IsValidRange(bPtr, 8)) return -1;
	u64 a = GRead<u64>(aPtr), b = GRead<u64>(bPtr);
	return a < b ? -1 : (a > b ? 1 : 0);
}
int TickAdd(u32 dst, u32 src, s64 amount) {
	if (!mem::IsValidRange(dst, 8) || !mem::IsValidRange(src, 8)) return -1;
	GWrite<u64>(dst, GRead<u64>(src) + (u64)amount);
	return 0;
}
int sceRtcTickAddTicks(u32 d, u32 s, s64 n) { return TickAdd(d, s, n); }
int sceRtcTickAddMicroseconds(u32 d, u32 s, s64 n) { return TickAdd(d, s, n); }
int sceRtcTickAddSeconds(u32 d, u32 s, s64 n) { return TickAdd(d, s, n * 1000000LL); }
int sceRtcTickAddMinutes(u32 d, u32 s, s64 n) { return TickAdd(d, s, n * 60000000LL); }
int sceRtcTickAddHours(u32 d, u32 s, int n) { return TickAdd(d, s, (s64)n * 3600000000LL); }
int sceRtcTickAddDays(u32 d, u32 s, int n) { return TickAdd(d, s, (s64)n * 86400000000LL); }
int sceRtcTickAddWeeks(u32 d, u32 s, int n) { return TickAdd(d, s, (s64)n * 7 * 86400000000LL); }
int sceRtcTickAddMonths(u32 d, u32 s, int n) {
	if (!mem::IsValidRange(d, 8) || !mem::IsValidRange(s, 8)) return -1;
	PspTime t = TickToTime(GRead<u64>(s));
	int total = t.year * 12 + (t.month - 1) + n;
	t.year = (u16)(total / 12);
	t.month = (u16)(total % 12 + 1);
	if (t.day > DaysInMonth(t.year, t.month)) t.day = (u16)DaysInMonth(t.year, t.month);
	GWrite<u64>(d, TimeToTick(t));
	return 0;
}
int sceRtcTickAddYears(u32 d, u32 s, int n) { return sceRtcTickAddMonths(d, s, n * 12); }
int sceRtcSetTime_t(u32 timePtr, u32 t) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime))) return -1;
	PspTime pt = TickToTime((u64)t * 1000000ULL + kRtcUnixOffset);
	mem::WriteBytes(timePtr, &pt, sizeof(pt));
	return 0;
}
int sceRtcGetTime_t(u32 timePtr, u32 outPtr) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime)) || !mem::IsValidRange(outPtr, 4)) return -1;
	PspTime t;
	mem::ReadBytes(&t, timePtr, sizeof(t));
	GWrite32(outPtr, (u32)((TimeToTick(t) - kRtcUnixOffset) / 1000000ULL));
	return 0;
}
int sceRtcSetTime64_t(u32 timePtr, u64 t) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime))) return -1;
	PspTime pt = TickToTime(t * 1000000ULL + kRtcUnixOffset);
	mem::WriteBytes(timePtr, &pt, sizeof(pt));
	return 0;
}
int sceRtcGetTime64_t(u32 timePtr, u32 outPtr) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime)) || !mem::IsValidRange(outPtr, 8)) return -1;
	PspTime t;
	mem::ReadBytes(&t, timePtr, sizeof(t));
	GWrite<u64>(outPtr, (TimeToTick(t) - kRtcUnixOffset) / 1000000ULL);
	return 0;
}
int sceRtcSetDosTime(u32 timePtr, u32 dos) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime))) return -1;
	PspTime t{};
	t.year = (u16)(((dos >> 25) & 0x7F) + 1980);
	t.month = (u16)((dos >> 21) & 0xF);
	t.day = (u16)((dos >> 16) & 0x1F);
	t.hour = (u16)((dos >> 11) & 0x1F);
	t.minute = (u16)((dos >> 5) & 0x3F);
	t.second = (u16)((dos & 0x1F) * 2);
	mem::WriteBytes(timePtr, &t, sizeof(t));
	return 0;
}
int sceRtcGetDosTime(u32 timePtr, u32 outPtr) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime)) || !mem::IsValidRange(outPtr, 4)) return -1;
	PspTime t;
	mem::ReadBytes(&t, timePtr, sizeof(t));
	if (t.year < 1980) {
		GWrite32(outPtr, 0);
		return -1;
	}
	u32 dos = ((u32)(t.year - 1980) << 25) | ((u32)t.month << 21) | ((u32)t.day << 16) | ((u32)t.hour << 11) |
	          ((u32)t.minute << 5) | (t.second / 2);
	GWrite32(outPtr, dos);
	return 0;
}
// FILETIME: 100ns units since 1601-01-01.
constexpr u64 kRtc1601 = 50491123200000000ULL;
int sceRtcSetWin32FileTime(u32 timePtr, u64 ft) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime))) return -1;
	PspTime t = TickToTime(ft / 10 + kRtc1601);
	mem::WriteBytes(timePtr, &t, sizeof(t));
	return 0;
}
int sceRtcGetWin32FileTime(u32 timePtr, u32 outPtr) {
	if (!mem::IsValidRange(timePtr, sizeof(PspTime)) || !mem::IsValidRange(outPtr, 8)) return -1;
	PspTime t;
	mem::ReadBytes(&t, timePtr, sizeof(t));
	u64 tick = TimeToTick(t);
	GWrite<u64>(outPtr, tick < kRtc1601 ? 0 : (tick - kRtc1601) * 10);
	return 0;
}
int FormatRfc(u32 outPtr, u32 tickPtr, int tzMinutes, bool rfc3339) {
	if (!mem::IsValidRange(outPtr, 32) || !mem::IsValidRange(tickPtr, 8)) return -1;
	PspTime t = TickToTime(GRead<u64>(tickPtr) + (s64)tzMinutes * 60000000LL);
	char buf[64];
	if (rfc3339) {
		int tzh = tzMinutes / 60, tzm = abs(tzMinutes % 60);
		if (tzMinutes == 0)
			snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%02dZ", t.year, t.month, t.day, t.hour, t.minute,
			         t.second, t.microsecond / 10000);
		else
			snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%02d%+03d:%02d", t.year, t.month, t.day, t.hour,
			         t.minute, t.second, t.microsecond / 10000, tzh, tzm);
	} else {
		static const char *days[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
		static const char *mons[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		int dow = sceRtcGetDayOfWeek(t.year, t.month, t.day);
		snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d %+05d", days[dow % 7], t.day, mons[(t.month - 1) % 12],
		         t.year, t.hour, t.minute, t.second, tzMinutes / 60 * 100 + tzMinutes % 60);
	}
	mem::WriteBytes(outPtr, buf, (u32)strlen(buf) + 1);
	return 0;
}
int sceRtcFormatRFC2822(u32 o, u32 t, int tz) { return FormatRfc(o, t, tz, false); }
int sceRtcFormatRFC3339(u32 o, u32 t, int tz) { return FormatRfc(o, t, tz, true); }
int sceRtcFormatRFC2822LocalTime(u32 o, u32 t) { return FormatRfc(o, t, (int)(LocalOffsetUs() / 60000000), false); }
int sceRtcFormatRFC3339LocalTime(u32 o, u32 t) { return FormatRfc(o, t, (int)(LocalOffsetUs() / 60000000), true); }
int sceRtcGetLastAdjustedTime(u32 tickPtr) { return sceRtcGetCurrentTick(tickPtr); }
int sceRtcGetLastReincarnatedTime(u32 tickPtr) { return sceRtcGetCurrentTick(tickPtr); }
u64 g_alarmTick = 0;
int sceRtcSetAlarmTick(u32 tickPtr) {
	g_alarmTick = mem::IsValidRange(tickPtr, 8) ? GRead<u64>(tickPtr) : 0;
	return 0;
}
int sceRtcGetAlarmTick(u32 tickPtr) {
	if (mem::IsValidRange(tickPtr, 8)) GWrite<u64>(tickPtr, g_alarmTick);
	return 0;
}
int sceRtcIsAlarmed() { return 0; }

// ---------------------------------------------------------------- sceUmdUser

constexpr u32 PSP_UMD_NOT_PRESENT = 0x01, PSP_UMD_PRESENT = 0x02, PSP_UMD_CHANGED = 0x04, PSP_UMD_INITING = 0x08,
              PSP_UMD_INITED = 0x10, PSP_UMD_READY = 0x20;
bool g_umdActivated = false;
SceUID g_umdCallback = 0;
WaitQueue g_umdWaiters;

bool DiscPresent() { return io::MountedDevice("disc0") != nullptr; }
u32 DriveStat() {
	if (!DiscPresent()) return PSP_UMD_NOT_PRESENT;
	return g_umdActivated ? (PSP_UMD_PRESENT | PSP_UMD_INITED | PSP_UMD_READY) : (PSP_UMD_PRESENT | PSP_UMD_INITED);
}
void UmdStatChanged() {
	if (g_umdCallback) NotifyCallback(g_umdCallback, (s32)DriveStat());
	g_umdWaiters.Cleanup(WAITTYPE_UMD, 0);
	std::vector<SceUID> w = g_umdWaiters.threads;
	g_umdWaiters.threads.clear();
	for (SceUID id : w) {
		PspThread *t = Get<PspThread>(id);
		if (t && (DriveStat() & t->waitValue[0])) sched::Wake(t, 0);
		else if (t) g_umdWaiters.threads.push_back(id);
	}
}
int sceUmdCheckMedium() { return DiscPresent() ? 1 : 0; }
int sceUmdGetDriveStat() { return (int)DriveStat(); }
int sceUmdActivate(int mode, const char *name) {
	if (mode < 1 || mode > 2) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	if (!name || _stricmp(name, "disc0:") != 0) return (int)SCE_KERNEL_ERROR_ERRNO_NO_SUCH_DEVICE;
	g_umdActivated = true;
	UmdStatChanged();
	return 0;
}
int sceUmdDeactivate(int mode, const char *name) {
	(void)mode;
	(void)name;
	g_umdActivated = false;
	UmdStatChanged();
	return 0;
}
static int UmdWait(u32 stat, u32 timeoutPtr, bool cb) {
	if ((stat & DriveStat()) != 0) return 0;
	PspThread *t = sched::Current();
	t->waitValue[0] = stat;
	g_umdWaiters.Add(t->uid);
	return sched::Wait(WAITTYPE_UMD, 0, timeoutPtr, cb);
}
int sceUmdWaitDriveStat(u32 stat) { return UmdWait(stat, 0, false); }
int sceUmdWaitDriveStatWithTimer(u32 stat, u32 timeout) {
	if ((stat & DriveStat()) != 0) return 0;
	u32 tmp = kernel::AllocKernelMemory(4, "umd timeout");
	GWrite32(tmp, timeout);
	int r = UmdWait(stat, timeout ? tmp : 0, false);
	g_kernelMem.Free(tmp);
	return r;
}
int sceUmdWaitDriveStatCB(u32 stat, u32 timeout) {
	(void)timeout;
	return UmdWait(stat, 0, true);
}
int sceUmdCancelWaitDriveStat() {
	g_umdWaiters.WakeAll(WAITTYPE_UMD, 0, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	return 0;
}
int sceUmdGetErrorStat() { return 0; }
int sceUmdGetDiscInfo(u32 infoPtr) {
	if (!mem::IsValidRange(infoPtr, 8)) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	if (GRead32(infoPtr) != 8) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	GWrite32(infoPtr + 4, 0x10);  // PSP_UMD_TYPE_GAME
	return 0;
}
int sceUmdRegisterUMDCallBack(SceUID cb) {
	if (!Get<Callback>(cb)) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	g_umdCallback = cb;
	return 0;
}
int sceUmdUnRegisterUMDCallBack(SceUID cb) {
	if (cb != g_umdCallback) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	g_umdCallback = 0;
	return 0;
}
int sceUmdReplaceProhibit() { return 0; }
int sceUmdReplacePermit() { return 0; }

// ---------------------------------------------------------------- sceImpose

int g_language = 1, g_buttonMode = 1;  // English, X = confirm (western default)
int g_umdPopup = 0, g_backlightOff = 0;
int sceImposeSetLanguageMode(int lang, int button) {
	g_language = lang;
	g_buttonMode = button;
	return 0;
}
int sceImposeGetLanguageMode(u32 langPtr, u32 buttonPtr) {
	if (mem::IsValidRange(langPtr, 4)) GWrite32(langPtr, (u32)g_language);
	if (mem::IsValidRange(buttonPtr, 4)) GWrite32(buttonPtr, (u32)g_buttonMode);
	return 0;
}
int sceImposeGetBatteryIconStatus(u32 chargingPtr, u32 iconPtr) {
	if (mem::IsValidRange(chargingPtr, 4)) GWrite32(chargingPtr, 0);
	if (mem::IsValidRange(iconPtr, 4)) GWrite32(iconPtr, 3);
	return 0;
}
int sceImposeSetUMDPopup(int v) {
	g_umdPopup = v;
	return 0;
}
int sceImposeGetUMDPopup() { return g_umdPopup; }
int sceImposeGetBacklightOffTime() { return g_backlightOff; }
int sceImposeSetBacklightOffTime(int v) {
	g_backlightOff = v;
	return 0;
}

// ---------------------------------------------------------------- sceDmac / sceHprm

int sceDmacMemcpy(u32 dst, u32 src, u32 size) {
	if (size == 0) return (int)SCE_KERNEL_ERROR_INVALID_SIZE;
	if (!mem::IsValidRange(dst, size) || !mem::IsValidRange(src, size)) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	mem::Memcpy(dst, src, size);
	return 0;
}
int sceHprmPeekCurrentKey(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, 0);
	return 0;
}
int sceHprmZero() { return 0; }
int sceHprmLatch(u32 ptr) {
	if (mem::IsValidRange(ptr, 16)) mem::Memset(ptr, 0, 16);
	return 0;
}
int sceHprmRegisterCallback(SceUID) { return 0; }

}  // namespace

namespace umd {
void NotifyDiscChanged() { UmdStatChanged(); }
}  // namespace umd

namespace systemparam {
int Language() { return g_language; }
int ButtonMode() { return g_buttonMode; }
}  // namespace systemparam

void Register_UtilsForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x91E4F6A7, sceKernelLibcClock),
		HLEFN(0x27CC57F0, sceKernelLibcTime),
		HLEFN(0x71EC4271, sceKernelLibcGettimeofday),
		HLEFN(0xBFA98062, sceKernelDcacheNop),
		HLEFN(0xC8186A58, sceKernelUtilsMd5Digest),
		HLEFN(0x9E5C5086, sceKernelUtilsMd5BlockInit),
		HLEFN(0x61E1E525, sceKernelUtilsMd5BlockUpdate),
		HLEFN(0xB8D24E78, sceKernelUtilsMd5BlockResult),
		HLEFN(0x840259F1, sceKernelUtilsSha1Digest),
		HLEFN(0xF8FCD5BA, sceKernelUtilsSha1BlockInit),
		HLEFN(0x346F6DA8, sceKernelUtilsSha1BlockUpdate),
		HLEFN(0x585F1C09, sceKernelUtilsSha1BlockResult),
		HLEFN(0xE860E75E, sceKernelUtilsMt19937Init),
		HLEFN(0x06FB8A63, sceKernelUtilsMt19937UInt),
		HLEFN(0x37FB5C42, sceKernelGetGPI),
		HLEFN(0x6AD345D7, sceKernelSetGPO),
		HLEFN(0x79D1C3FA, sceKernelDcacheNop0),
		HLEFN(0xB435DEC5, sceKernelDcacheNop0),
		HLEFN(0x3EE30821, sceKernelDcacheNop),
		HLEFN(0x34B9FA9E, sceKernelDcacheNop),
		HLEFN(0xC2DF770E, sceKernelDcacheNop),
		HLEFN(0x920F104A, sceKernelDcacheNop0),
	};
	hle::RegisterModule("UtilsForUser", funcs);
	hle::RegisterModule("UtilsForKernel", funcs);
}

void Register_scePower() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x04B7766E, scePowerRegisterCallback),
		HLEFN(0xEFD3C963, scePowerTick),
		HLEFN(0x87440F5E, scePowerIsPowerOnline),
		HLEFN(0x0AFD0D8B, scePowerIsBatteryExist),
		HLEFN(0x1E490401, scePowerIsBatteryCharging),
		HLEFN(0xB4432BC8, scePowerGetBatteryChargingStatus),
		HLEFN(0xD3075926, scePowerIsLowBattery),
		HLEFN(0x78A1A796, scePowerIsSuspendRequired),
		HLEFN(0x2085D15D, scePowerGetBatteryLifePercent),
		HLEFN(0x8EFB3FA2, scePowerGetBatteryLifeTime),
		HLEFN(0x28E12023, scePowerGetBatteryTemp),
		HLEFN(0xDB62C9CF, scePowerCancelRequest),
		HLEFN(0x2B7C7CF4, scePowerRequestStandby),
		HLEFN(0xAC32C9CC, scePowerRequestSuspend),
		HLEFN(0xDFA8BAF8, scePowerUnregisterCallback),
		HLEFN(0xDB9D28DD, scePowerUnregisterCallback),
		HLEFN(0x843FBF43, scePowerSetCpuClockFrequency),
		HLEFN(0xB8D7B3FB, scePowerSetBusClockFrequency),
		HLEFN(0xFEE03A2F, scePowerGetCpuClockFrequencyInt),
		HLEFN(0x478FE6F5, scePowerGetBusClockFrequencyInt),
		HLEFN(0xFDB5BFE9, scePowerGetCpuClockFrequencyInt),
		HLEFN(0xBD681969, scePowerGetBusClockFrequencyInt),
		HLEFN(0xB1A52C83, scePowerGetCpuClockFrequencyFloat),
		HLEFN(0x9BADB3EB, scePowerGetBusClockFrequencyFloat),
		HLEFN(0x737486F2, scePowerSetClockFrequency),
		HLEFN(0x34F9C463, scePowerGetPllClockFrequencyInt),
		HLEFN(0xEA382A27, scePowerGetPllClockFrequencyFloat),
		HLEFN(0xEBD177D6, scePowerSetClockFrequency),
		HLEFN(0x469989AD, scePowerSetClockFrequency),
		HLEFN(0xA85880D0, scePowerCheckWlanCoexistenceClock),
		HLEFN(0x23C31FFE, sceKernelVolatileMemLock),
		HLEFN(0xFA97A599, sceKernelVolatileMemTryLock),
		HLEFN(0xB3EDD801, sceKernelVolatileMemUnlock),
	};
	hle::RegisterModule("scePower", funcs);
	hle::RegisterModule("scePower_driver", funcs);
}

void Register_sceSuspendForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xEADB1BD7, sceKernelPowerLock),
		HLEFN(0x3AEE7261, sceKernelPowerUnlock),
		HLEFN(0x090CCB3F, sceKernelPowerTick),
		HLEFN(0xA14F40B2, sceKernelVolatileMemTryLock),
		HLEFN(0xA569E425, sceKernelVolatileMemUnlock),
		HLEFN(0x3E0271D3, sceKernelVolatileMemLock),
	};
	hle::RegisterModule("sceSuspendForUser", funcs);
}

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
		HLEFN(0xE1C93E47, sceRtcGetTime64_t),
		HLEFN(0x1909C99B, sceRtcSetTime64_t),
		HLEFN(0x62685E98, sceRtcGetLastAdjustedTime),
		HLEFN(0x203CEB0D, sceRtcGetLastReincarnatedTime),
		HLEFN(0x7D1FBED3, sceRtcSetAlarmTick),
		HLEFN(0x81FCDA34, sceRtcIsAlarmed),
		HLEFN(0xC2DDBEB5, sceRtcGetAlarmTick),
	};
	hle::RegisterModule("sceRtc", funcs);
}

void Register_sceUmdUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xC6183D47, sceUmdActivate),
		HLEFN(0x6B4A146C, sceUmdGetDriveStat),
		HLEFN(0x46EBB729, sceUmdCheckMedium),
		HLEFN(0xE83742BA, sceUmdDeactivate),
		HLEFN(0x8EF08FCE, sceUmdWaitDriveStat),
		HLEFN(0x56202973, sceUmdWaitDriveStatWithTimer),
		HLEFN(0x4A9E5E29, sceUmdWaitDriveStatCB),
		HLEFN(0x6AF9B50A, sceUmdCancelWaitDriveStat),
		HLEFN(0x20628E6F, sceUmdGetErrorStat),
		HLEFN(0x340B7686, sceUmdGetDiscInfo),
		HLEFN(0xAEE7404D, sceUmdRegisterUMDCallBack),
		HLEFN(0xBD2BDE07, sceUmdUnRegisterUMDCallBack),
		HLEFN(0x87533940, sceUmdReplaceProhibit),
		HLEFN(0xCBE9F02A, sceUmdReplacePermit),
	};
	hle::RegisterModule("sceUmdUser", funcs);
}

void Register_sceImpose() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x36AA6E91, sceImposeSetLanguageMode),
		HLEFN(0x24FD7BCF, sceImposeGetLanguageMode),
		HLEFN(0x8C943191, sceImposeGetBatteryIconStatus),
		HLEFN(0x72189C48, sceImposeSetUMDPopup),
		HLEFN(0xE0887BC8, sceImposeGetUMDPopup),
		HLEFN(0x8F6E3518, sceImposeGetBacklightOffTime),
		HLEFN(0x967F6D4A, sceImposeSetBacklightOffTime),
	};
	hle::RegisterModule("sceImpose", funcs);
}

void Register_sceDmac() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x617F3FE6, sceDmacMemcpy),
		HLEFN(0xD97F94D8, sceDmacMemcpy),
	};
	hle::RegisterModule("sceDmac", funcs);
}

void Register_sceHprm() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x1910B327, sceHprmPeekCurrentKey),
		HLEFN(0x208DB1BD, sceHprmZero),
		HLEFN(0x7E69EDA4, sceHprmZero),
		HLEFN(0x219C58F1, sceHprmZero),
		HLEFN(0xC7154136, sceHprmRegisterCallback),
		HLEFN(0xFD7DE6CD, sceHprmRegisterCallback),
		HLEFN(0x2BCEC83E, sceHprmLatch),
		HLEFN(0x40D2F9F0, sceHprmLatch),
	};
	hle::RegisterModule("sceHprm", funcs);
}
