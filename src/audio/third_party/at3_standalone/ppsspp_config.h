// PSPC shim for the one PPSSPP config macro the standalone ATRAC decoder uses (PPSSPP_ARCH).
#pragma once

#define PPSSPP_ARCH(FEATURE) (PSPC_AT3_ARCH_##FEATURE)

#if defined(_M_X64) || defined(__x86_64__)
#define PSPC_AT3_ARCH_AMD64 1
#define PSPC_AT3_ARCH_X86 0
#define PSPC_AT3_ARCH_SSE2 1
#elif defined(_M_IX86) || defined(__i386__)
#define PSPC_AT3_ARCH_AMD64 0
#define PSPC_AT3_ARCH_X86 1
#define PSPC_AT3_ARCH_SSE2 1
#else
#define PSPC_AT3_ARCH_AMD64 0
#define PSPC_AT3_ARCH_X86 0
#define PSPC_AT3_ARCH_SSE2 0
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
#define PSPC_AT3_ARCH_ARM64 1
#define PSPC_AT3_ARCH_ARM_NEON 1
#else
#define PSPC_AT3_ARCH_ARM64 0
#define PSPC_AT3_ARCH_ARM_NEON 0
#endif
