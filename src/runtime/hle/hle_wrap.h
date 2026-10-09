// SPDX-License-Identifier: GPL-2.0-or-later
//
// Automatic argument/return marshalling for HLE functions following the PSP EABI:
// integer arguments in $a0-$a3,$t0-$t3 (64-bit values in an aligned register pair),
// float arguments in $f12-$f19, results in $v0 (+$v1) or $f0.
//
//   static int sceFoo(u32 a, PspPtr<SceFooParam> p, const char *name);
//   const HleFunction table[] = { HLEFN(0x12345678, sceFoo), ... };
#pragma once

#include <tuple>
#include <type_traits>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"

// A guest pointer argument. Holds the address; dereference only after checking validity.
template <typename T>
struct PspPtr {
	u32 addr = 0;
	PspPtr() = default;
	explicit PspPtr(u32 a) : addr(a) {}
	bool IsValid() const { return addr != 0 && mem::IsValidRange(addr, (u32)sizeof(T)); }
	explicit operator bool() const { return IsValid(); }
	T *get() const { return IsValid() ? (T *)GPtr(addr) : nullptr; }
	T *operator->() const { return get(); }
	T &operator*() const { return *get(); }
	T Read() const {
		T v{};
		if (IsValid()) memcpy(&v, GPtr(addr), sizeof(T));
		return v;
	}
	void Write(const T &v) const {
		if (IsValid()) memcpy(GPtr(addr), &v, sizeof(T));
	}
};

namespace hle {
namespace detail {

struct ArgState {
	Ctx *c;
	int gpr = 4;
	int fpr = 12;
};

template <typename T>
struct ArgReader {
	static T Read(ArgState &s) {
		static_assert(sizeof(T) <= 8, "unsupported HLE argument type");
		if constexpr (std::is_floating_point_v<T>) {
			return (T)s.c->f[s.fpr++];
		} else if constexpr (sizeof(T) == 8) {
			if (s.gpr & 1) s.gpr++;
			u64 lo = s.gpr <= 11 ? s.c->r[s.gpr] : 0;
			u64 hi = s.gpr + 1 <= 11 ? s.c->r[s.gpr + 1] : 0;
			s.gpr += 2;
			return (T)(lo | (hi << 32));
		} else {
			u32 v = s.gpr <= 11 ? s.c->r[s.gpr] : 0;
			s.gpr++;
			return (T)v;
		}
	}
};

template <typename T>
struct ArgReader<PspPtr<T>> {
	static PspPtr<T> Read(ArgState &s) {
		u32 v = s.gpr <= 11 ? s.c->r[s.gpr] : 0;
		s.gpr++;
		return PspPtr<T>(v);
	}
};

template <>
struct ArgReader<const char *> {
	static const char *Read(ArgState &s) {
		u32 v = s.gpr <= 11 ? s.c->r[s.gpr] : 0;
		s.gpr++;
		return v ? mem::GetCString(v) : nullptr;
	}
};

template <typename R>
inline void WriteRet(Ctx *c, R v) {
	if constexpr (std::is_floating_point_v<R>) {
		c->f[0] = (float)v;
	} else if constexpr (sizeof(R) == 8) {
		c->r[2] = (u32)(u64)v;
		c->r[3] = (u32)((u64)v >> 32);
	} else {
		c->r[2] = (u32)v;
	}
}

template <auto F, typename R, typename... A>
inline void WrapImpl(Ctx *c, R (*)(A...)) {
	ArgState st{c};
	std::tuple<std::decay_t<A>...> args{ArgReader<std::decay_t<A>>::Read(st)...};
	if constexpr (std::is_void_v<R>) {
		std::apply(F, args);
	} else {
		R r = std::apply(F, args);
		WriteRet<R>(c, r);
	}
}

}  // namespace detail

template <auto F>
void Wrap(Ctx *c) {
	detail::WrapImpl<F>(c, F);
}

}  // namespace hle

#define HLEFN(nid, func) hle::HleFunction{(u32)(nid), &hle::Wrap<&func>, #func, 0}
#define HLEFN_F(nid, func, flags) hle::HleFunction{(u32)(nid), &hle::Wrap<&func>, #func, (flags)}
// Raw function taking the context directly: void func(Ctx *c).
#define HLERAW(nid, func) hle::HleFunction{(u32)(nid), &func, #func, 0}
// NID known but deliberately unimplemented: logs and returns 0.
#define HLESTUB(nid, name) hle::HleFunction{(u32)(nid), nullptr, name, 0}
