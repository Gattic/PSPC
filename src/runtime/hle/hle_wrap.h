// SPDX-License-Identifier: GPL-2.0-or-later
//
// Automatic argument/return marshalling for HLE functions following the PSP EABI:
// integer arguments in $a0-$a3,$t0-$t3 (64-bit values in an aligned register pair), then words
// on the stack from $sp, float arguments in $f12-$f19, results in $v0 (+$v1) or $f0.
//
//   static int sceFoo(u32 a, PspPtr<SceFooParam> p, const char *name);
//   const HleFunction table[] = { HLEFN(0x12345678, sceFoo), ... };
#pragma once

#include <array>
#include <tuple>
#include <type_traits>
#include <utility>

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

// Stack argument `index` (0 = the first word at $sp). Out of line: keeps the wrappers small.
u32 StackArgWord(const Ctx *c, int index);

// Every argument's register (or stack word) is fixed at compile time, and each read is a pure
// function of that constant. (An earlier version threaded a mutable cursor through a tuple
// initializer; MSVC 19.44 /O2 miscompiled one instance of it.)
template <typename T>
struct IsPspPtr : std::false_type {};
template <typename T>
struct IsPspPtr<PspPtr<T>> : std::true_type {};

template <typename T>
constexpr bool kFloatArg = std::is_floating_point_v<T>;
template <typename T>
constexpr bool kWideArg = !kFloatArg<T> && !std::is_pointer_v<T> && !IsPspPtr<T>::value && sizeof(T) == 8;

// Slot of each argument: 4..11 = $a0-$t3 (then stack words from 12), 12.. = $f12-.
template <typename... A>
constexpr std::array<int, sizeof...(A) + 1> ArgSlots() {
	std::array<int, sizeof...(A) + 1> out{};
	int gpr = 4, fpr = 12, i = 0;
	auto place = [&](bool isFloat, bool isWide) {
		if (isFloat) return fpr++;
		if (isWide) {
			gpr += gpr & 1;  // 64-bit values take an aligned register pair
			gpr += 2;
			return gpr - 2;
		}
		return gpr++;
	};
	((out[i++] = place(kFloatArg<A>, kWideArg<A>)), ...);
	return out;
}

template <int Slot>
inline u32 WordAt(Ctx *c) {
	if constexpr (Slot <= 11) return c->r[Slot];
	else return StackArgWord(c, Slot - 12);
}

template <typename T, int Slot>
inline T ReadArg(Ctx *c) {
	if constexpr (kFloatArg<T>) {
		return (T)c->f[Slot];
	} else if constexpr (IsPspPtr<T>::value) {
		return T(WordAt<Slot>(c));
	} else if constexpr (std::is_same_v<T, const char *>) {
		u32 v = WordAt<Slot>(c);
		return v ? mem::GetCString(v) : nullptr;
	} else if constexpr (kWideArg<T>) {
		return (T)((u64)WordAt<Slot>(c) | ((u64)WordAt<Slot + 1>(c) << 32));
	} else {
		static_assert(sizeof(T) <= 4 && !std::is_pointer_v<T>, "unsupported HLE argument type");
		return (T)WordAt<Slot>(c);
	}
}

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

template <auto F, typename R, typename Args, size_t... I>
inline void WrapIndexed(Ctx *c, std::index_sequence<I...>) {
	constexpr auto slots = ArgSlots<std::tuple_element_t<I, Args>...>();
	if constexpr (std::is_void_v<R>) {
		F(ReadArg<std::tuple_element_t<I, Args>, slots[I]>(c)...);
	} else {
		R r = F(ReadArg<std::tuple_element_t<I, Args>, slots[I]>(c)...);
		WriteRet<R>(c, r);
	}
}

template <auto F, typename R, typename... A>
inline void WrapImpl(Ctx *c, R (*)(A...)) {
	WrapIndexed<F, R, std::tuple<std::decay_t<A>...>>(c, std::index_sequence_for<A...>{});
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
