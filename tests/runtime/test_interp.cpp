// SPDX-License-Identifier: GPL-2.0-or-later
#include "allegrex/sem_tables.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/interp.h"
#include "runtime/core/memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
constexpr u32 kCode = 0x08804000, kData = 0x08808000;
void Require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}
void Put(const std::vector<u32> &words) {
	for (size_t i = 0; i < words.size(); ++i) GWrite32(kCode + (u32)i * 4, words[i]);
}
Ctx Context() {
	Ctx c{};
	c.r[29] = 0x08900000;
	c.budget = 1000000000;
	return c;
}
void Run(Ctx &c) {
	cpu::SetCurrentCtx(&c);
	cpu::RunFunction(&c, kCode);
	cpu::SetCurrentCtx(nullptr);
}
void CheckSemantics() {
	// Mixed scalar ALU and memory operations, with dependencies and writes to $zero.
	const std::vector<u32> body = {
		0x2400007B, 0x2408FFFD, 0x24090007, 0x01095021, 0x01285823,
		0x00096080, 0x00086843, 0x01097024, 0x01097825, 0x01091026,
		0x0109182A, 0x2D030008, 0x310400FF, 0x35050080, 0x390600AA,
		0x3C070880, 0x34E78000, 0xACEA0000, 0x8CE20000, 0xA4E80004, 0x84E30004,
		0x01090018, 0x00002012, 0x00002810,
	};
	Ctx reference = Context();
	for (u32 w : body) {
		const auto op = ag::Decode(w);
		Require(ag::OpKind(op) == ag::Kind::ALU, "invalid reference instruction");
		ag::g_semTable[(size_t)op](&reference, w);
	}
	const u32 expectedMemory = GRead32(kData);
	GWrite32(kData, 0);
	auto code = body;
	code.insert(code.end(), {0x03E00008, 0}); // jr ra; nop
	Put(code);
	Ctx actual = Context();
	const u64 before = interp::InstructionCount();
	Run(actual);
	for (int i = 0; i < 31; ++i) Require(actual.r[i] == reference.r[i], "ALU/register mismatch");
	Require(actual.hi == reference.hi && actual.lo == reference.lo, "multiply result mismatch");
	Require(GRead32(kData) == expectedMemory, "memory result mismatch");
	Require(interp::InstructionCount() - before == code.size(), "instruction accounting mismatch");
	Require(actual.budget == reference.budget - (s32)code.size(), "cycle accounting mismatch");

	// A not-taken likely branch annuls its slot; a taken branch executes it.
	Put({0x24020001, 0x50020001, 0x24420040, 0x10000001, 0x24420002, 0x03E00008, 0x24420004});
	actual = Context();
	Run(actual);
	Require(actual.r[2] == 7, "branch/delay-slot semantics changed");
	// Reusing a PC after a guest code write must immediately execute the new word.
	GWrite32(kCode, 0x24020003);
	actual = Context();
	Run(actual);
	Require(actual.r[2] == 9, "stale instruction after guest code write");
}
void Benchmark(const char *name, const std::vector<u32> &body) {
	auto code = body;
	code.push_back(0x2484FFFF); // addiu a0,a0,-1
	code.push_back(0x1C800000 | ((u32)(-(int)code.size() - 1) & 0xFFFF)); // bgtz a0,start
	code.insert(code.end(), {0, 0x03E00008, 0});
	Put(code);
	constexpr u32 iterations = 1000000;
	std::vector<double> times;
	for (int repeat = 0; repeat < 7; ++repeat) {
		Ctx c = Context();
		c.r[4] = iterations;
		c.r[5] = kData;
		GWrite32(kData, 0);
		const u64 before = interp::InstructionCount();
		const auto start = std::chrono::steady_clock::now();
		Run(c);
		times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		Require(c.r[4] == 0, "benchmark loop did not finish");
		Require(interp::InstructionCount() - before == iterations * (body.size() + 3) + 2,
		        "benchmark instruction count mismatch");
	}
	std::sort(times.begin(), times.end());
	printf("%s: median %.3f ms, %.2f M instructions/s\n", name, times[3],
	       (iterations * (body.size() + 3) + 2) / (times[3] * 1000.0));
}
}
int main(int argc, char **argv) {
	try {
		Require(mem::Init(32 * 1024 * 1024), "guest memory initialization failed");
		cpu::Init();
		dispatch::Init();
		CheckSemantics();
		if (argc == 2 && !strcmp(argv[1], "--bench")) {
			Benchmark("scalar", {0x25080001, 0x01084821, 0x01285026, 0x000A5840, 0x01696025, 0x01896823, 0x31AE00FF, 0x01C87821});
			Benchmark("memory", {0x8CA80000, 0x25080001, 0xACA80000, 0x01084821});
		}
		Require(mem::BadAccessCount() == 0, "bad guest memory access");
		mem::Shutdown();
		puts("interpreter semantics and instruction accounting passed");
		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
