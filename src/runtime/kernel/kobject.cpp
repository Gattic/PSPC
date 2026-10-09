// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/kernel/kobject.h"

#include <map>
#include <memory>
#include <vector>

namespace kernel {

namespace {
// UIDs are never reused within a session, so stale IDs fail cleanly.
std::map<SceUID, std::unique_ptr<KObject>> g_objects;
SceUID g_next = 0x00100001;
}  // namespace

SceUID Register(KObject *obj) {
	SceUID id = g_next;
	g_next += 2;
	obj->uid = id;
	g_objects[id].reset(obj);
	return id;
}

KObject *GetAny(SceUID uid) {
	auto it = g_objects.find(uid);
	return it == g_objects.end() ? nullptr : it->second.get();
}

void Destroy(SceUID uid) { g_objects.erase(uid); }

void DestroyAll() {
	g_objects.clear();
}

void ForEachObject(const std::function<void(KObject *)> &fn) {
	// Copy the IDs first: callbacks may destroy objects.
	std::vector<SceUID> ids;
	ids.reserve(g_objects.size());
	for (auto &kv : g_objects) ids.push_back(kv.first);
	for (SceUID id : ids) {
		auto it = g_objects.find(id);
		if (it != g_objects.end()) fn(it->second.get());
	}
}

}  // namespace kernel
