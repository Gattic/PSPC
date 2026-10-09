// SPDX-License-Identifier: GPL-2.0-or-later
// Kernel object table (SceUID -> object).
#pragma once

#include <functional>
#include <string>

#include "common/common.h"
#include "runtime/kernel/errors.h"

typedef s32 SceUID;

namespace kernel {

enum class KType : u8 {
	Thread = 1,
	Semaphore,
	EventFlag,
	Mutex,
	LwMutex,
	Mbx,
	MsgPipe,
	Vpl,
	Fpl,
	Callback,
	Alarm,
	VTimer,
	Module,
	MemBlock,
	ThreadEventHandler,
	Tls,
	Other,
};

struct KObject {
	SceUID uid = 0;
	std::string name;
	virtual ~KObject() = default;
	virtual KType Type() const = 0;
};

SceUID Register(KObject *obj);
KObject *GetAny(SceUID uid);
void Destroy(SceUID uid);
void DestroyAll();
void ForEachObject(const std::function<void(KObject *)> &fn);

template <typename T>
T *Get(SceUID uid) {
	KObject *o = GetAny(uid);
	if (!o || o->Type() != T::kType) return nullptr;
	return static_cast<T *>(o);
}

template <typename T>
void ForEach(const std::function<void(T *)> &fn) {
	ForEachObject([&](KObject *o) {
		if (o->Type() == T::kType) fn(static_cast<T *>(o));
	});
}

}  // namespace kernel
