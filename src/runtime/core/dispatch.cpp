// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/core/dispatch.h"

#include <atomic>
#include <memory>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/interp.h"

namespace dispatch {

namespace {

// Two-level table over the 512 MB physical window: 8192 pages of 64 KB, 16384 slots each.
constexpr u32 PAGE_SHIFT = 16;
constexpr u32 NUM_PAGES = 0x20000000 >> PAGE_SHIFT;
constexpr u32 SLOTS = (1u << PAGE_SHIFT) / 4;

struct Page {
	HostFn slot[SLOTS];
};

std::unique_ptr<Page> g_pages[NUM_PAGES];
std::atomic<u32> g_count{0};

}  // namespace

void Init() {
	for (auto &p : g_pages) p.reset();
	g_count = 0;
}

void Register(u32 addr, HostFn fn) {
	u32 p = (addr & GUEST_ADDR_MASK);
	auto &page = g_pages[p >> PAGE_SHIFT];
	if (!page) {
		page = std::make_unique<Page>();
		memset(page->slot, 0, sizeof(page->slot));
	}
	HostFn &slot = page->slot[(p & 0xFFFF) >> 2];
	if (!slot && fn) g_count++;
	if (slot && !fn) g_count--;
	slot = fn;
}

void UnregisterRange(u32 addr, u32 size) {
	for (u32 a = addr & ~3u; a < addr + size; a += 4) {
		u32 p = a & GUEST_ADDR_MASK;
		auto &page = g_pages[p >> PAGE_SHIFT];
		if (!page) {
			a = (a | 0xFFFF);
			a -= 3;
			continue;
		}
		HostFn &slot = page->slot[(p & 0xFFFF) >> 2];
		if (slot) {
			slot = nullptr;
			g_count--;
		}
	}
}

HostFn Lookup(u32 addr) {
	u32 p = addr & GUEST_ADDR_MASK;
	const auto &page = g_pages[p >> PAGE_SHIFT];
	if (!page) return nullptr;
	return page->slot[(p & 0xFFFF) >> 2];
}

u32 RegisteredCount() { return g_count.load(); }

void Call(Ctx *c, u32 target) {
	if (HostFn fn = Lookup(target)) {
		c->pc = target;
		fn(c);
		return;
	}
	interp::Run(c, target, c->r[31], c->r[29]);
}

void Jump(Ctx *c, u32 target, u32 ra0, u32 sp0) {
	if (HostFn fn = Lookup(target)) {
		// The callee returns to whatever $ra holds now; continue there if it isn't our exit.
		u32 ret = c->r[31];
		c->pc = target;
		fn(c);
		if (c->unwinding) return;
		if (ret == ra0 && c->r[29] >= sp0) return;
		interp::Run(c, ret, ra0, sp0);
		return;
	}
	interp::Run(c, target, ra0, sp0);
}

void NonlocalReturn(Ctx *c, u32 target) {
	c->pc = target;
	c->unwinding = 1;
}

}  // namespace dispatch
