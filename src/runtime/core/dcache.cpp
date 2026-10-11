// SPDX-License-Identifier: GPL-2.0-or-later
// The data cache model (dcache.h). Traced against pspautotests cpu/lsu/cacheop.
#include "runtime/core/dcache.h"

#include <cstring>

#include "allegrex/guestmem.h"
#include "runtime/core/memory.h"

namespace dcache {

bool g_enabled = false;

namespace {

constexpr u32 kLineBytes = 64, kSets = 128, kWays = 2;

struct Line {
	bool valid = false, dirty = false;
	u32 tag = 0;
	u8 memory[kLineBytes];  // what memory holds; the host has the cached view
};

struct Set {
	Line way[kWays];
	u8 victim = 0;  // the least recently used way
};

Set g_sets[kSets];

u32 Physical(u32 addr) { return addr & GUEST_ADDR_MASK; }
u32 SetOf(u32 p) { return (p / kLineBytes) % kSets; }
u32 TagOf(u32 p) { return p / (kLineBytes * kSets); }
u8 *HostLine(u32 setIndex, u32 tag) { return GPtr((tag * kSets + setIndex) * kLineBytes); }

Line *Find(u32 p, int *wayOut = nullptr) {
	Set &s = g_sets[SetOf(p)];
	for (u32 w = 0; w < kWays; w++) {
		if (s.way[w].valid && s.way[w].tag == TagOf(p)) {
			if (wayOut) *wayOut = (int)w;
			return &s.way[w];
		}
	}
	return nullptr;
}

void Use(u32 setIndex, int way) { g_sets[setIndex].victim = (u8)(way ^ 1); }

// Out of the cache: a dirty line was written back (memory is the cached view the host has), a clean
// one leaves memory as it is, which an uncached store may have changed.
void Drop(u32 setIndex, Line &line) {
	if (!line.valid) return;
	if (!line.dirty) memcpy(HostLine(setIndex, line.tag), line.memory, kLineBytes);
	line.valid = false;
	line.dirty = false;
}

// A miss: the victim goes and the line comes in from memory.
Line &Fill(u32 p) {
	const u32 setIndex = SetOf(p);
	Set &s = g_sets[setIndex];
	const int way = s.victim;
	Line &line = s.way[way];
	Drop(setIndex, line);
	line.valid = true;
	line.dirty = false;
	line.tag = TagOf(p);
	memcpy(line.memory, HostLine(setIndex, line.tag), kLineBytes);
	Use(setIndex, way);
	return line;
}

void Writeback(u32 setIndex, Line &line) {
	if (line.valid && line.dirty) {
		memcpy(line.memory, HostLine(setIndex, line.tag), kLineBytes);
		line.dirty = false;
	}
}

// A dirty line that's dropped without a writeback loses its stores: the host gets memory back.
void Discard(u32 setIndex, Line &line) {
	if (!line.valid) return;
	memcpy(HostLine(setIndex, line.tag), line.memory, kLineBytes);
	line.valid = false;
	line.dirty = false;
}

}  // namespace

void Reset() {
	for (Set &s : g_sets) s = Set{};
}

bool IsCached(u32 addr) {
	const u32 seg = addr >> 28;
	if ((seg >= 0x4 && seg <= 0x7) || seg == 0xA || seg == 0xB) return false;
	return mem::IsRam(addr);
}

bool IsUncachedRam(u32 addr) {
	const u32 seg = addr >> 28;
	return ((seg >= 0x4 && seg <= 0x7) || seg == 0xA || seg == 0xB) && mem::IsRam(addr);
}

void Access(u32 addr, bool store) {
	const u32 p = Physical(addr);
	int way;
	Line *line = Find(p, &way);
	if (line)
		Use(SetOf(p), way);
	else
		line = &Fill(p);
	if (store) line->dirty = true;
}

void UncachedAccess(u32 addr, const std::function<void()> &access) {
	const u32 p = Physical(addr);
	Line *line = Find(p);
	if (!line) {
		access();
		return;
	}
	u8 *host = HostLine(SetOf(p), line->tag);
	u8 cached[kLineBytes];
	memcpy(cached, host, kLineBytes);
	memcpy(host, line->memory, kLineBytes);
	access();
	memcpy(line->memory, host, kLineBytes);
	memcpy(host, cached, kLineBytes);
}

void CacheOp(u32 op, u32 addr) {
	const u32 p = Physical(addr);
	const u32 setIndex = SetOf(p);
	switch (op) {
	case 0x14: {  // index writeback invalidate: the way is address bit 13
		Line &line = g_sets[setIndex].way[(p / (kLineBytes * kSets)) & 1];
		Drop(setIndex, line);
		break;
	}
	case 0x18: {  // create dirty exclusive: resident and dirty, without a fill
		if (!mem::IsRam(addr)) break;
		Line *line = Find(p);
		if (!line) line = &Fill(p);
		line->dirty = true;
		break;
	}
	case 0x19:  // hit invalidate: dirty data is lost
		if (Line *line = Find(p)) Discard(setIndex, *line);
		break;
	case 0x1a:  // hit writeback
		if (Line *line = Find(p)) Writeback(setIndex, *line);
		break;
	case 0x1b:  // hit writeback invalidate
		if (Line *line = Find(p)) {
			Writeback(setIndex, *line);
			Drop(setIndex, *line);
		}
		break;
	case 0x1e:  // fill
		if (mem::IsRam(addr) && !Find(p)) Fill(p);
		break;
	default:  // instruction cache operations
		break;
	}
}

void WritebackRange(u32 addr, u32 size, bool invalidate) {
	for (u32 a = addr & ~(kLineBytes - 1); a < addr + size; a += kLineBytes) CacheOp(invalidate ? 0x1b : 0x1a, a);
}

void InvalidateRange(u32 addr, u32 size) {
	for (u32 a = addr & ~(kLineBytes - 1); a < addr + size; a += kLineBytes) CacheOp(0x19, a);
}

void WritebackAll(bool invalidate) {
	for (u32 s = 0; s < kSets; s++) {
		for (Line &line : g_sets[s].way) {
			Writeback(s, line);
			if (invalidate) Drop(s, line);
		}
	}
}

}  // namespace dcache
