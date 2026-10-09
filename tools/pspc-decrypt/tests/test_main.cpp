// SPDX-License-Identifier: GPL-3.0-or-later
// pspc_decrypt_tests: a separate executable from pspc_unittests, because the code under test is
// GPL-3.0-or-later. Usage: pspc_decrypt_tests --tool <pspc-decrypt> --repo <PSPC root>
// --tmp <scratch dir> [test names...]
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "test_util.h"

int main(int argc, char **argv) {
	std::vector<std::string> only;
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if ((a == "--tool" || a == "--repo" || a == "--tmp") && i + 1 < argc) {
			std::string &target = a == "--tool" ? g_env.tool : a == "--repo" ? g_env.repo : g_env.tmp;
			target = argv[++i];
		} else {
			only.push_back(a);
		}
	}
	if (g_env.tmp.empty()) {
		g_env.tmp = PathToU8(std::filesystem::temp_directory_path() / "pspc-decrypt-tests");
	}
	std::error_code ec;
	std::filesystem::remove_all(U8Path(g_env.tmp), ec);
	std::filesystem::create_directories(U8Path(g_env.tmp), ec);

	int ran = 0, failedTests = 0;
	for (const auto &[name, fn] : TestList()) {
		if (!only.empty() && std::find(only.begin(), only.end(), name) == only.end()) {
			continue;
		}
		const int before = g_failures;
		printf("[ RUN  ] %s\n", name);
		fflush(stdout);
		fn();
		ran++;
		if (g_failures != before) {
			failedTests++;
			printf("[ FAIL ] %s\n", name);
		} else {
			printf("[  OK  ] %s\n", name);
		}
		fflush(stdout);
	}
	printf("%d tests, %d checks, %d failed checks, %d failed tests\n", ran, g_checks, g_failures, failedTests);
	return failedTests ? 1 : 0;
}
