// PSPC - PSP 4.01 M33 as a native Windows program.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#if defined(_MSC_VER) && !defined(__clang__)
#define PSPC_FORCEINLINE __forceinline
#define PSPC_NOINLINE __declspec(noinline)
#define PSPC_LIKELY(x) (x)
#define PSPC_UNLIKELY(x) (x)
#define PSPC_RESTRICT __restrict
#else
#define PSPC_FORCEINLINE inline __attribute__((always_inline))
#define PSPC_NOINLINE __attribute__((noinline))
#define PSPC_LIKELY(x) __builtin_expect(!!(x), 1)
#define PSPC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PSPC_RESTRICT __restrict
#endif

#ifdef _WIN32
#define PSPC_EXPORT __declspec(dllexport)
#else
#define PSPC_EXPORT __attribute__((visibility("default")))
#endif

template <typename T, size_t N>
constexpr size_t ArraySize(const T (&)[N]) { return N; }

inline u32 AlignUp(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
inline u32 AlignDown(u32 v, u32 a) { return v & ~(a - 1); }
