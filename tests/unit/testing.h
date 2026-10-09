// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal test registry: TEST(name) { CHECK(...); }
#pragma once

#include <cstdio>
#include <functional>
#include <vector>

struct TestCase {
	const char *name;
	std::function<void()> fn;
};

std::vector<TestCase> &TestRegistry();
extern int g_testFailures;

struct TestRegistrar {
	TestRegistrar(const char *name, std::function<void()> fn) { TestRegistry().push_back({name, fn}); }
};

#define TEST(name)                                                   \
	static void test_##name();                                       \
	static TestRegistrar registrar_##name(#name, test_##name);       \
	static void test_##name()

#define CHECK(cond)                                                                       \
	do {                                                                                  \
		if (!(cond)) {                                                                    \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
			g_testFailures++;                                                             \
		}                                                                                 \
	} while (0)

#define CHECK_EQ(a, b)                                                                     \
	do {                                                                                   \
		auto _a = (a);                                                                     \
		auto _b = (b);                                                                     \
		if (!(_a == _b)) {                                                                 \
			printf("  FAILED %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b, \
			       (unsigned long long)_a, (unsigned long long)_b);                        \
			g_testFailures++;                                                              \
		}                                                                                  \
	} while (0)
