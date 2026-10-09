// SPDX-License-Identifier: GPL-3.0-or-later
// pspc-decrypt self-tests: registry, checks, and builders that produce the formats the tool
// reads (so the decoders can be exercised without any Sony data).
#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "common.h"

struct TestEnv {
	std::string tool;  // path to pspc-decrypt.exe
	std::string repo;  // PSPC repository root (for reference/401M33)
	std::string tmp;   // scratch directory
};
extern TestEnv g_env;

extern int g_checks;
extern int g_failures;

using TestFn = void (*)();
std::vector<std::pair<const char *, TestFn>> &TestList();
struct TestRegistrar {
	TestRegistrar(const char *name, TestFn fn) { TestList().emplace_back(name, fn); }
};
#define TEST(name) \
	static void test_##name(); \
	static TestRegistrar registrar_##name(#name, test_##name); \
	static void test_##name()

#define CHECK(cond) do { \
	g_checks++; \
	if (!(cond)) { g_failures++; fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
	g_checks++; \
	const auto checkA_ = (a); const auto checkB_ = (b); \
	if (!(checkA_ == checkB_)) { \
		g_failures++; \
		fprintf(stderr, "  %s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); \
	} \
} while (0)

// Skips the rest of a test when a precondition isn't met (e.g. reference files are missing).
#define REQUIRE(cond) do { \
	g_checks++; \
	if (!(cond)) { g_failures++; fprintf(stderr, "  %s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); return; } \
} while (0)

Bytes HexBytes(const char *hex);
std::string TempPath(const std::string &name);
Bytes PseudoRandom(size_t size, u32 seed);
bool FileExists(const std::string &path);
Bytes ReadAll(const std::string &path);  // empty if missing

// Stored-block (uncompressed) gzip and zlib streams: valid input for any inflater.
Bytes GzipStore(const Bytes &data);
Bytes ZlibStore(const Bytes &data);

// A small but well-formed PSP PRX ELF (e_type 0xFFA0) whose module info names `name`.
Bytes MakeTestElf(const std::string &name, size_t size, u32 seed, u16 attribute = 0x1006);

// KL4E (or KL3E) and LZRC streams made of literals plus the end marker, from a mirror image of
// the decoders' range coder. Both start with their magic ("KL4E"/"KL3E", "2RLZ").
Bytes Kl4eEncodeLiterals(const Bytes &data, u8 flags, bool kl3e = false);
Bytes LzrcEncodeLiterals(const Bytes &data, u8 lc);

// KIRK CMD1 block (header + data_offset bytes + data, encrypted by CMD0) with fresh keys.
Bytes KirkCmd1Block(const Bytes &dataOffsetArea, const Bytes &plain, u32 keySeed);

// PRX blobs that DecryptPRX accepts, built by running each decryption step backwards.
// `prxHeader` is the first 0x80 bytes of the result. The returned blob is 0x150 bytes plus the
// encrypted payload rounded up to 16.
Bytes EncryptPrxType0(u32 tag, const u8 prxHeader[0x80], const Bytes &payload, u32 keySeed);
Bytes EncryptPrxType2(u32 tag, const u8 prxHeader[0x80], const Bytes &payload, u32 keySeed);

enum class TestPacking { KirkType0, KirkType2, CfwGzip, UnknownTag };
// A complete ~PSP module around `elf`: compressed as asked (comp_attribute set accordingly),
// then packed/encrypted. `compression` is "gzip", "kl4e" or "none".
Bytes MakePspModule(const Bytes &elf, const std::string &modname, TestPacking packing, u32 tag,
	const std::string &compression, u32 keySeed);

// The PSAR "mangle" (inverse of the demangle step) over bytes 0x20..0x150 of a block.
void MangleBlock(u8 *block);

// A file list as an updater stores it: a PRX blob around the text, DES-CBC encrypted with one
// of the six table keys (2 is 4.x's).
Bytes EncryptFileList(const std::string &text, int tableKeyIndex);

// Runs the tool. Returns its exit code; stdout goes to *out if given.
int RunTool(const std::vector<std::string> &args, std::string *out = nullptr);
