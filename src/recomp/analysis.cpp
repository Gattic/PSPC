// SPDX-License-Identifier: GPL-2.0-or-later
#include "recomp/analysis.h"

#include <algorithm>

#include "allegrex/decode.h"
#include "allegrex/insn.h"

namespace recomp {

using ag::Kind;
using ag::Op;
using namespace insn;

namespace {

// Backward search for the last definition of `reg` before `pc` in straight-line code, resolving
// it to a constant through lui/addiu/ori/addu-with-zero chains.
bool ResolveConst(const ModuleImage &img, u32 pc, u32 reg, u32 &value, int depth = 0) {
	if (reg == 0) {
		value = 0;
		return true;
	}
	if (depth > 4) return false;
	for (int i = 1; i <= 24; i++) {
		u32 a = pc - 4 * i;
		if (!img.InCode(a)) return false;
		u32 w = img.Read32(a);
		Op op = ag::Decode(w);
		Kind k = ag::OpKind(op);
		if (k != Kind::ALU) {
			// Crossing control flow: only the delay-slot-free straight line is trusted.
			if (k == Kind::JAL || k == Kind::JALR || k == Kind::JR || k == Kind::J) return false;
			continue;
		}
		switch (op) {
		case Op::LUI:
			if (Rt(w) == reg) {
				value = Imm16(w) << 16;
				return true;
			}
			break;
		case Op::ADDIU:
		case Op::ADDI:
			if (Rt(w) == reg) {
				u32 b;
				if (!ResolveConst(img, a, Rs(w), b, depth + 1)) return false;
				value = b + (u32)Simm16(w);
				return true;
			}
			break;
		case Op::ORI:
			if (Rt(w) == reg) {
				u32 b;
				if (!ResolveConst(img, a, Rs(w), b, depth + 1)) return false;
				value = b | Imm16(w);
				return true;
			}
			break;
		case Op::ADDU:
		case Op::OR:
			if (Rd(w) == reg) {
				if (Rs(w) == 0) return ResolveConst(img, a, Rt(w), value, depth + 1);
				if (Rt(w) == 0) return ResolveConst(img, a, Rs(w), value, depth + 1);
				return false;
			}
			break;
		default: {
			// Any other write to reg ends the search.
			bool writesRd = Opc(w) == 0 || Opc(w) == 28 || Opc(w) == 31;
			bool writesRt = !writesRd && Opc(w) >= 8 && Opc(w) <= 15;
			bool isLoad = Opc(w) >= 32 && Opc(w) <= 38;
			if ((writesRd && Rd(w) == reg) || ((writesRt || isLoad) && Rt(w) == reg)) return false;
			break;
		}
		}
	}
	return false;
}

// Finds the table behind "jr $rs" in the usual GCC/SN pattern:
//   sltiu at, idx, N ; beqz at, default ; sll idx, idx, 2 ; lui at, hi ; addu at, at, idx ;
//   lw rs, lo(at) ; jr rs
bool FindJumpTable(const ModuleImage &img, u32 jrPc, JumpTable &jt) {
	u32 rs = Rs(img.Read32(jrPc));
	u32 lwPc = 0, lwWord = 0;
	for (int i = 1; i <= 16; i++) {
		u32 a = jrPc - 4 * i;
		if (!img.InCode(a)) return false;
		u32 w = img.Read32(a);
		Op op = ag::Decode(w);
		if (op == Op::LW && Rt(w) == rs) {
			lwPc = a;
			lwWord = w;
			break;
		}
		Kind k = ag::OpKind(op);
		if (k == Kind::JR || k == Kind::JAL || k == Kind::JALR || k == Kind::J) return false;
	}
	if (!lwPc) return false;
	u32 baseReg = Rs(lwWord);
	u32 table = 0;
	bool found = false;
	for (int i = 1; i <= 16 && !found; i++) {
		u32 a = lwPc - 4 * i;
		if (!img.InCode(a)) break;
		u32 w = img.Read32(a);
		Op op = ag::Decode(w);
		if (op == Op::ADDU && Rd(w) == baseReg) {
			u32 c;
			if (ResolveConst(img, a, Rs(w), c) || ResolveConst(img, a, Rt(w), c)) {
				table = c + (u32)Simm16(lwWord);
				found = true;
			}
			break;
		}
		if (ag::OpKind(op) != Kind::ALU && ag::OpKind(op) != Kind::BR && ag::OpKind(op) != Kind::BRL) break;
	}
	if (!found) return false;
	// Bound from a preceding sltiu, if any.
	u32 count = 0;
	for (int i = 1; i <= 24; i++) {
		u32 a = lwPc - 4 * i;
		if (!img.InCode(a)) break;
		u32 w = img.Read32(a);
		if (ag::Decode(w) == Op::SLTIU) {
			count = Imm16(w);
			break;
		}
	}
	u32 maxCount = count ? count : 1024;
	for (u32 i = 0; i < maxCount; i++) {
		u32 ea = table + 4 * i;
		if (!img.InImage(ea)) break;
		if (img.relocatable && !img.relocWords.count(ea)) break;
		u32 t = img.Read32(ea);
		if (!img.InCode(t) || (t & 3)) break;
		jt.targets.push_back(t);
	}
	if (jt.targets.empty() || (count && jt.targets.size() != count)) return false;
	jt.table = table;
	return true;
}

struct Walker {
	const ModuleImage &img;
	const std::set<u32> &strong;
	std::set<u32> &newEntries;

	bool IsTailCall(const Function &f, u32 target) const {
		if (target == f.entry) return false;
		if (target > f.entry && target < f.rangeEnd) return false;
		return true;
	}

	void Walk(Function &f) {
		std::vector<u32> work{f.entry};
		while (!work.empty()) {
			u32 pc = work.back();
			work.pop_back();
			if (f.body.count(pc)) continue;
			if (!img.InCode(pc) || img.IsStub(pc)) continue;
			u32 w = img.Read32(pc);
			Op op = ag::Decode(w);
			Kind k = ag::OpKind(op);
			f.body.insert(pc);
			switch (k) {
			case Kind::ALU:
			case Kind::SYSCALL:
			case Kind::BREAK: work.push_back(pc + 4); break;
			case Kind::BR:
			case Kind::BRL: {
				u32 t = BranchTarget(pc, w);
				if (img.InCode(t)) {
					f.labels.insert(t);
					work.push_back(t);
				}
				work.push_back(pc + 8);
				break;
			}
			case Kind::BRAL:
			case Kind::BRALL:
			case Kind::JAL:
			case Kind::JALR: work.push_back(pc + 8); break;
			case Kind::J: {
				u32 t = JumpTarget(pc, w);
				if (IsTailCall(f, t)) {
					f.tailCalls.insert(pc);
					if (img.InCode(t) && !strong.count(t) && !img.IsStub(t)) newEntries.insert(t);
				} else {
					f.labels.insert(t);
					work.push_back(t);
				}
				break;
			}
			case Kind::JR: {
				if (Rs(w) == 31) break;
				JumpTable jt;
				if (FindJumpTable(img, pc, jt)) {
					for (u32 t : jt.targets) {
						f.labels.insert(t);
						work.push_back(t);
					}
					f.jumpTables[pc] = std::move(jt);
				} else {
					f.unresolvedJumps++;
				}
				break;
			}
			default: break;
			}
		}
	}
};

void ScanCodePointers(const ModuleImage &img, std::set<u32> &strong, std::set<u32> &weak) {
	for (const auto &r : img.code) {
		for (u32 pc = r.first; pc + 4 <= r.second; pc += 4) {
			if (img.IsStub(pc)) continue;
			u32 w = img.Read32(pc);
			Op op = ag::Decode(w);
			Kind k = ag::OpKind(op);
			if (k == Kind::JAL) {
				u32 t = JumpTarget(pc, w);
				if (img.InCode(t) && !img.IsStub(t)) strong.insert(t);
			} else if (k == Kind::BRAL || k == Kind::BRALL) {
				u32 t = BranchTarget(pc, w);
				if (img.InCode(t) && !img.IsStub(t)) strong.insert(t);
			} else if (op == Op::LUI) {
				// lui rX, hi ; ... addiu/ori rY, rX, lo  => possible function pointer
				u32 rt = Rt(w);
				for (int i = 1; i <= 8; i++) {
					u32 a = pc + 4 * i;
					if (!img.InCode(a)) break;
					u32 w2 = img.Read32(a);
					Op op2 = ag::Decode(w2);
					if ((op2 == Op::ADDIU || op2 == Op::ORI) && Rs(w2) == rt) {
						u32 v = op2 == Op::ADDIU ? (Imm16(w) << 16) + (u32)Simm16(w2) : (Imm16(w) << 16) | Imm16(w2);
						if (img.InCode(v) && !(v & 3) && !img.IsStub(v)) weak.insert(v);
						break;
					}
					if (ag::OpKind(op2) != Kind::ALU) break;
				}
			}
		}
	}
}

void ScanDataPointers(const ModuleImage &img, std::set<u32> &weak) {
	if (img.relocatable) {
		for (u32 a : img.relocWords) {
			if (img.InCode(a)) continue;
			u32 v = img.Read32(a);
			if (img.InCode(v) && !(v & 3) && !img.IsStub(v)) weak.insert(v);
		}
		return;
	}
	// Fixed-address executables have no relocations: accept data words that point at something
	// that looks like a function start (stack adjust, or just after a return).
	for (u32 a = img.low; a + 4 <= img.high; a += 4) {
		if (img.InCode(a)) continue;
		u32 v = img.Read32(a);
		if (!img.InCode(v) || (v & 3) || img.IsStub(v)) continue;
		if (LooksLikeFunctionStart(img, v)) weak.insert(v);
	}
}

// Walks every entry in `entries` that has no function yet, to a fixpoint over the tail-call
// targets the walks discover (those become strong entries).
void WalkEntries(const ModuleImage &img, std::set<u32> &strong, const std::set<u32> &weak,
                 std::map<u32, Function> &funcs) {
	for (int round = 0; round < 8; round++) {
		std::set<u32> all = strong;
		all.insert(weak.begin(), weak.end());
		std::set<u32> newEntries;
		for (u32 e : all) {
			if (funcs.count(e)) continue;
			Function f;
			f.entry = e;
			auto next = strong.upper_bound(e);
			f.rangeEnd = next == strong.end() ? 0xFFFFFFFF : *next;
			Walker w{img, strong, newEntries};
			w.Walk(f);
			funcs[e] = std::move(f);
		}
		size_t before = strong.size();
		for (u32 e : newEntries) strong.insert(e);
		if (strong.size() == before) break;
		// Range ends changed for functions preceding new entries: re-walk those.
		for (u32 e : newEntries) {
			auto it = funcs.lower_bound(e);
			if (it != funcs.begin()) funcs.erase(std::prev(it));
		}
	}
}

}  // namespace

bool HasDelaySlot(u32 w) {
	switch (ag::OpKind(ag::Decode(w))) {
	case Kind::BR:
	case Kind::BRL:
	case Kind::BRAL:
	case Kind::BRALL:
	case Kind::J:
	case Kind::JAL:
	case Kind::JR:
	case Kind::JALR: return true;
	default: return false;
	}
}

void MarkCovered(const ModuleImage &img, const Function &f, std::vector<bool> &cov) {
	for (u32 pc : f.body) {
		cov[(pc - img.low) / 4] = true;
		if (HasDelaySlot(img.Read32(pc)) && img.InImage(pc + 4)) cov[(pc + 4 - img.low) / 4] = true;
	}
}

bool LooksLikeFunctionStart(const ModuleImage &img, u32 v) {
	u32 first = img.Read32(v);
	if (ag::Decode(first) == Op::ADDIU && Rs(first) == 29 && Rt(first) == 29 && Simm16(first) < 0) return true;
	// Two words after a "jr ra" (return + delay slot).
	return img.InCode(v - 8) && img.Read32(v - 8) == 0x03E00008;
}

Analysis Analyze(const ModuleImage &img, const AnalysisOptions &opts) {
	Analysis an;
	std::set<u32> &strong = an.strongEntries;
	std::set<u32> &weak = an.weakEntries;
	if (img.InCode(img.entry)) strong.insert(img.entry);
	for (const auto &lib : img.exports)
		for (const auto &f : lib.funcs)
			if (img.InCode(f.second)) strong.insert(f.second);
	for (u32 h : opts.hints)
		if (img.InCode(h) && !(h & 3)) strong.insert(h);
	ScanCodePointers(img, strong, weak);
	if (opts.scanDataPointers) ScanDataPointers(img, weak);
	for (u32 e : strong) weak.erase(e);

	std::map<u32, Function> funcs;
	WalkEntries(img, strong, {}, funcs);

	// A weak entry inside code the strong functions already cover is almost always a switch case
	// or a local label whose address was taken; give it a function only if it looks like a start.
	std::vector<bool> covered((img.high - img.low) / 4 + 1);
	for (const auto &kv : funcs) MarkCovered(img, kv.second, covered);
	for (auto it = weak.begin(); it != weak.end();) {
		if (covered[(*it - img.low) / 4] && !LooksLikeFunctionStart(img, *it)) {
			it = weak.erase(it);
			an.droppedWeak++;
		} else {
			++it;
		}
	}
	WalkEntries(img, strong, weak, funcs);

	// Code nothing reaches is usually still code: functions called only through pointers the
	// scans miss (vtables in fixed-address executables, computed addresses). An uncovered word
	// becomes an entry when it sits on a function boundary (range start, prologue, or the first
	// word after a return/tail jump and its padding) and the next few words decode.
	if (opts.gapEntries) {
		std::vector<bool> cov((img.high - img.low) / 4 + 1);
		for (const auto &kv : funcs) MarkCovered(img, kv.second, cov);
		auto onBoundary = [&](u32 pc, u32 rangeStart) {
			if (pc == rangeStart || LooksLikeFunctionStart(img, pc)) return true;
			u32 q = pc - 4;
			while (q >= rangeStart && img.Read32(q) == 0) q -= 4;
			if (q < rangeStart) return true;
			auto ends = [&](u32 a) {
				if (a < rangeStart) return false;
				u32 w = img.Read32(a);
				return w == 0x03E00008 || ag::OpKind(ag::Decode(w)) == Kind::J;
			};
			return ends(q) || ends(q - 4);  // q is the jump (nop delay slot skipped) or its delay slot
		};
		std::set<u32> newEntries;
		for (const auto &r : img.code) {
			for (u32 pc = r.first; pc + 4 <= r.second; pc += 4) {
				if (cov[(pc - img.low) / 4] || img.IsStub(pc) || img.Read32(pc) == 0) continue;
				if (funcs.count(pc) || !onBoundary(pc, r.first)) continue;
				bool plausible = true;
				for (u32 a = pc; a < pc + 16 && a + 4 <= r.second; a += 4)
					if (ag::OpKind(ag::Decode(img.Read32(a))) == Kind::INVALID) plausible = false;
				if (!plausible) continue;
				Function f;
				f.entry = pc;
				auto next = strong.upper_bound(pc);
				f.rangeEnd = next == strong.end() ? 0xFFFFFFFF : *next;
				std::set<u32> tails;
				Walker w{img, strong, tails};
				w.Walk(f);
				// Data decoded as code branches into other functions or hits invalid words; real
				// unreached functions do neither.
				bool garbage = false;
				for (u32 a : f.body) {
					if (cov[(a - img.low) / 4] || ag::OpKind(ag::Decode(img.Read32(a))) == Kind::INVALID) {
						garbage = true;
						break;
					}
				}
				if (garbage) continue;
				MarkCovered(img, f, cov);
				newEntries.insert(tails.begin(), tails.end());
				weak.insert(pc);
				an.gapEntries++;
				funcs[pc] = std::move(f);
			}
		}
		for (u32 e : newEntries) {
			if (!strong.insert(e).second) continue;
			auto it = funcs.lower_bound(e);
			if (it != funcs.begin()) funcs.erase(std::prev(it));
		}
		WalkEntries(img, strong, weak, funcs);
	}

	for (auto &kv : funcs) {
		an.instructions += kv.second.body.size();
		an.unresolvedIndirect += kv.second.unresolvedJumps;
		an.functions.push_back(std::move(kv.second));
	}
	return an;
}

}  // namespace recomp
