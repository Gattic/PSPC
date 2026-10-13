// SPDX-License-Identifier: GPL-2.0-or-later
// Execute synthetic recompiled code through the same module-loading boundary as the runtime.
#include "allegrex/aot_abi.h"
#include "common/fileutil.h"
#include "common/host.h"
#include "pspc_sem_version.h"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
constexpr u32 kEntry = 0x08804000;
constexpr u32 kReturn = 0xDEADBEEF;

void Require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

struct State {
	int syscalls = 0;
	bool syscallLocation = false;
	bool interpreted = false;
};

void Syscall(Ctx *c, u32 insn, u32 pc, u32 tag) {
	auto &state = *static_cast<State *>(c->thread);
	++state.syscalls;
	state.syscallLocation = insn == 0x0000000C && pc == kEntry + 12;
	c->r[3] = tag;
}
void SyscallA(Ctx *c, u32 insn, u32 pc) { Syscall(c, insn, pc, 1); }
void SyscallB(Ctx *c, u32 insn, u32 pc) { Syscall(c, insn, pc, 2); }
void Interpret(Ctx *c, u32 pc, u32 ra0, u32 sp0) {
	auto &state = *static_cast<State *>(c->thread);
	state.interpreted = pc == kEntry && ra0 == kReturn && sp0 == 0x100;
	c->r[2] = 0xBEEF;
}

const AotModuleDesc *Bind(void *module, const char *path, PspcRuntimeApi &api) {
	auto entry = reinterpret_cast<PspcAotModuleFn>(hostsys::ModuleSymbol(module, PSPC_AOT_ENTRY_NAME));
	Require(entry != nullptr, "missing AOT export");
	Require(entry(nullptr) == nullptr, "null API accepted");
	auto invalid = api;
	++invalid.abiVersion;
	Require(entry(&invalid) == nullptr, "incompatible ABI accepted");
	invalid = api;
	--invalid.structSize;
	Require(entry(&invalid) == nullptr, "truncated API accepted");

	const auto *desc = entry(&api);
	Require(desc && desc->abiVersion == PSPC_AOT_ABI_VERSION, "incorrect descriptor ABI");
	Require(desc->semVersion == PSPC_SEM_VERSION, "incorrect semantics version");
	Require(HexString(desc->sha1, sizeof(desc->sha1)) == std::filesystem::path(path).stem().string(), "incorrect ELF hash");
	Require(desc->recompileBase == kEntry && !desc->relocatable, "incorrect fixed-image descriptor");
	Require(desc->delta && *desc->delta == 0 && desc->patched, "missing module state");
	Require(desc->numFunctions > 0 && desc->functions, "empty function table");
	return desc;
}

u32 EntryIndex(const AotModuleDesc &desc) {
	for (u32 i = 0; i < desc.numFunctions; ++i)
		if (desc.functions[i].offset == 0) return i;
	throw std::runtime_error("guest entry not compiled");
}

void Execute(const AotFunctionEntry &fn, std::array<u32, 64> &memory, u32 input, u32 addend, u32 tag, bool body) {
	State state;
	Ctx c{};
	c.r[4] = 0x40;
	c.r[29] = 0x100;
	c.r[31] = kReturn;
	c.budget = 100;
	c.thread = &state;
	memory[16] = input;
	memory[17] = 0;
	Require(fn.fn && fn.body, "missing call/body entry");
	if (body) fn.body(&c, c.r[31], c.r[29]);
	else fn.fn(&c);
	Require(c.r[2] == input + addend + 1, "incorrect load/arithmetic/return delay slot");
	Require(memory[17] == input + addend, "incorrect guest memory store");
	Require(c.r[3] == tag && state.syscalls == 1 && state.syscallLocation, "incorrect module API binding");
	Require(c.budget == 94 && !c.unwinding, "incorrect budget or return state");
}
void ExecuteRoundUp(const AotFunctionEntry &fn) {
	Ctx c{};
	c.r[29] = 0x100;
	c.r[31] = kReturn;
	c.budget = 100;
	fn.fn(&c);
	std::printf("Guest round-up: result=%08x expected=3f800001\n", c.r[2]);
	Require(c.r[2] == 0x3F800001 && c.fi[2] == 0x3F800001,
	        "guest round-up was lost: 1.0f + 2^-24 must round to the next float");
	Require(c.fcr31 == 0 && c.budget == 88 && !c.unwinding, "incorrect rounding fixture return state");
}
}  // namespace

int main(int argc, char **argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: pspc_aot_probe <first.so> <second.so> <rounding.so>\n");
		return 2;
	}
	try {
		std::array<u32, 64> memory[2]{};
		u32 cop0[32]{}, interrupts = 1;
		PspcRuntimeApi api[2]{};
		for (int i = 0; i < 2; ++i) {
			api[i].abiVersion = PSPC_AOT_ABI_VERSION;
			api[i].structSize = sizeof(PspcRuntimeApi);
			api[i].membase = reinterpret_cast<u8 *>(memory[i].data());
			api[i].cop0 = cop0;
			api[i].intrEnabled = &interrupts;
			api[i].syscall = i == 0 ? SyscallA : SyscallB;
			api[i].interpretFrom = Interpret;
		}
		using Module = std::unique_ptr<void, decltype(&hostsys::CloseModule)>;
		Module first(hostsys::OpenModule(argv[1]), hostsys::CloseModule);
		Module second(hostsys::OpenModule(argv[2]), hostsys::CloseModule);
		Module rounding(hostsys::OpenModule(argv[3]), hostsys::CloseModule);
		Require(first && second && rounding, "cannot load generated modules");
		const auto *a = Bind(first.get(), argv[1], api[0]);
		const auto *b = Bind(second.get(), argv[2], api[1]);
		const auto *fpu = Bind(rounding.get(), argv[3], api[0]);
		u32 ai = EntryIndex(*a), bi = EntryIndex(*b);
		// Bind both first, then interleave execution: each .so must retain its own API and memory.
		Execute(a->functions[ai], memory[0], 20, 7, 1, false);
		Execute(b->functions[bi], memory[1], 50, 11, 2, true);
		Execute(a->functions[ai], memory[0], 90, 7, 1, true);
		Require(memory[1][17] == 61, "one module overwrote another's memory");
		ExecuteRoundUp(fpu->functions[EntryIndex(*fpu)]);

		State state;
		Ctx c{};
		c.r[29] = 0x100;
		c.r[31] = kReturn;
		c.thread = &state;
		a->patched[ai] = 1;
		a->functions[ai].fn(&c);
		Require(state.interpreted && c.r[2] == 0xBEEF && state.syscalls == 0, "patched entry did not hand off");
		Require(b->patched[bi] == 0, "module patch flags are not isolated");
		puts("AOT ABI, execution, module isolation and patched-entry handoff passed");
		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "AOT probe: %s\n", e.what());
		return 1;
	}
}
