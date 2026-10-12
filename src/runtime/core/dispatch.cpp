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
	BodyFn body[SLOTS];
};

// No function's frame is this big: a jump whose $sp lands further below the frame's start than
// this has switched to another stack.
constexpr u32 kMaxFrameBytes = 1u << 20;

std::unique_ptr<Page> g_pages[NUM_PAGES];
std::atomic<u32> g_count{0};
void (*g_missHook)(u32) = nullptr;

void Miss(u32 target) {
	if (g_missHook) g_missHook(target);
}

Page &PageFor(u32 p) {
	auto &page = g_pages[p >> PAGE_SHIFT];
	if (!page) {
		page = std::make_unique<Page>();
		memset(page->slot, 0, sizeof(page->slot));
		memset(page->body, 0, sizeof(page->body));
	}
	return *page;
}

}  // namespace

void Init() {
	for (auto &p : g_pages) p.reset();
	g_count = 0;
}

void RegisterNative(u32 addr, HostFn fn, BodyFn body) {
	u32 p = (addr & GUEST_ADDR_MASK);
	Page &page = PageFor(p);
	HostFn &slot = page.slot[(p & 0xFFFF) >> 2];
	if (!slot && fn) g_count++;
	if (slot && !fn) g_count--;
	slot = fn;
	page.body[(p & 0xFFFF) >> 2] = fn ? body : nullptr;
}

void Register(u32 addr, HostFn fn) { RegisterNative(addr, fn, nullptr); }

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
		page->body[(p & 0xFFFF) >> 2] = nullptr;
	}
}

HostFn Lookup(u32 addr) {
	u32 p = addr & GUEST_ADDR_MASK;
	const auto &page = g_pages[p >> PAGE_SHIFT];
	if (!page) return nullptr;
	return page->slot[(p & 0xFFFF) >> 2];
}

BodyFn LookupBody(u32 addr) {
	u32 p = addr & GUEST_ADDR_MASK;
	const auto &page = g_pages[p >> PAGE_SHIFT];
	if (!page) return nullptr;
	return page->body[(p & 0xFFFF) >> 2];
}

u32 RegisteredCount() { return g_count.load(); }

u32 GuestFunctionAt(const void *hostAddr, size_t *offset) {
	uintptr_t a = (uintptr_t)hostAddr, best = 0;
	u32 guest = 0;
	for (u32 pg = 0; pg < NUM_PAGES; pg++) {
		if (!g_pages[pg]) continue;
		for (u32 s = 0; s < SLOTS; s++) {
			const uintptr_t fns[2] = {(uintptr_t)g_pages[pg]->slot[s], (uintptr_t)g_pages[pg]->body[s]};
			for (uintptr_t f : fns) {
				if (f && f <= a && f > best) {
					best = f;
					guest = (pg << PAGE_SHIFT) | (s << 2);
				}
			}
		}
	}
	if (offset) *offset = (size_t)(a - best);
	return guest;
}

void Call(Ctx *c, u32 target) {
	if (HostFn fn = Lookup(target)) {
		c->pc = target;
		fn(c);
		return;
	}
	Miss(target);
	interp::Run(c, target, c->r[31], c->r[29]);
}

void Jump(Ctx *c, u32 target, u32 ra0, u32 sp0) {
	// A jump that leaves the jumper's stack doesn't continue its frame: the guest switched stacks,
	// as a coroutine or task switch does (Daxter's scheduler saves its registers, swaps $sp and
	// jumps to the other context's saved $ra). Running the target inside this host frame would
	// grow the host stack with every switch until it overflows, so unwind instead: to a frame
	// waiting for that address if one is on the host stack, else to the thread's top, which
	// carries on from there (Resume). sp0 == 0: the thread's top frame, nothing to unwind.
	if (sp0 && (c->r[29] > sp0 || sp0 - c->r[29] > kMaxFrameBytes)) {
		NonlocalReturn(c, target);
		return;
	}
	// Recompiled code continues the jumper's frame: it returns to ra0 like the jumper would.
	if (BodyFn body = LookupBody(target)) {
		c->pc = target;
		body(c, ra0, sp0);
		return;
	}
	if (HostFn fn = Lookup(target)) {
		// A thunk with call semantics returns to whatever $ra holds now; continue there if it
		// isn't our exit.
		u32 ret = c->r[31];
		c->pc = target;
		fn(c);
		if (c->unwinding) return;
		if (ret == ra0 && c->r[29] >= sp0) return;
		interp::Run(c, ret, ra0, sp0);
		return;
	}
	Miss(target);
	interp::Run(c, target, ra0, sp0);
}

void Resume(Ctx *c, u32 target, u32 ra0) {
	if (BodyFn body = LookupBody(target)) {
		c->pc = target;
		body(c, ra0, 0);
		return;
	}
	Miss(target);
	interp::Run(c, target, ra0, 0);
}

void SetMissHook(void (*hook)(u32)) { g_missHook = hook; }

void NonlocalReturn(Ctx *c, u32 target) {
	c->pc = target;
	c->unwinding = 1;
}

}  // namespace dispatch
