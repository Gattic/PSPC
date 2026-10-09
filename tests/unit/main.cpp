// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>

#include "common/common.h"
#include "testing.h"

u8 *g_membase = nullptr;
static u32 s_cop0[32];
static u32 s_intr = 1;
u32 *g_cop0 = s_cop0;
u32 *g_intrEnabled = &s_intr;

std::vector<TestCase> &TestRegistry() {
	static std::vector<TestCase> tests;
	return tests;
}
int g_testFailures = 0;

int main(int argc, char **argv) {
	static u8 fakeMem[0x10000];
	// Unit tests use a small window: callers keep addresses below 0x10000.
	g_membase = fakeMem;
	int run = 0;
	for (auto &t : TestRegistry()) {
		if (argc > 1 && !strstr(t.name, argv[1])) continue;
		int before = g_testFailures;
		t.fn();
		printf("%s %s\n", g_testFailures == before ? "[ OK ]" : "[FAIL]", t.name);
		run++;
	}
	printf("%d tests, %d failures\n", run, g_testFailures);
	return g_testFailures ? 1 : 0;
}
