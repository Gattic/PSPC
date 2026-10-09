// SPDX-License-Identifier: GPL-3.0-or-later
// The command line, run as the AOT pipeline will run it: as a subprocess, judged by exit code,
// output files and --json output.
#include "test_util.h"

#include <filesystem>

#include "formats.h"

static std::string M33Installer() {
	return JoinPath(g_env.repo, "reference/401M33/UPDATE/EBOOT.PBP");
}

TEST(cli_usage_errors) {
	std::string out;
	CHECK_EQ(RunTool({}, &out), 1);
	CHECK_EQ(RunTool({ "frobnicate" }, &out), 1);
	CHECK_EQ(RunTool({ "prx", "only-one-arg" }, &out), 1);
	CHECK_EQ(RunTool({ "prx", "a", "b", "--bogus" }, &out), 1);
	CHECK_EQ(RunTool({ "prx", "a", "b", "--seed", "1234" }, &out), 1);
	CHECK_EQ(RunTool({ "version" }, &out), 0);
	CHECK(out.find("pspc-decrypt") != std::string::npos);
	CHECK_EQ(RunTool({ "--help" }, &out), 0);
	CHECK(out.find("Exit codes") != std::string::npos);
}

TEST(cli_info) {
	REQUIRE(FileExists(M33Installer()));
	std::string out;
	CHECK_EQ(RunTool({ "info", M33Installer(), "--json" }, &out), 0);
	CHECK(out.find("\"kind\": \"PBP\"") != std::string::npos);
	CHECK(out.find("\"embedded_modules\"") != std::string::npos);
	CHECK(out.find("\"module_name\": \"SystemControl\"") != std::string::npos);
	CHECK(out.find("\"TITLE\"") != std::string::npos);
	CHECK_EQ(RunTool({ "info", M33Installer() }, &out), 0);
	CHECK(out.find("kind: PBP") != std::string::npos);
	CHECK_EQ(RunTool({ "info", TempPath("does-not-exist") }, &out), 2);

	// A ~PSP module: header fields and what decoding it gives.
	const Bytes pbp = ReadAll(M33Installer());
	const std::string carved = TempPath("info-vshctrl.psp");
	std::string error;
	REQUIRE(WriteFileBytes(carved, pbp.data() + 0x177e2, 11743, &error));
	CHECK_EQ(RunTool({ "info", carved, "--json" }, &out), 0);
	CHECK(out.find("\"module_name\": \"VshControl\"") != std::string::npos);
	CHECK(out.find("\"elf_size\": 29142") != std::string::npos);
	CHECK(out.find("\"cfw_packed\": true") != std::string::npos);
	CHECK(out.find("\"tag\": \"0x4c9414f0\"") != std::string::npos);
}

TEST(cli_prx) {
	REQUIRE(FileExists(M33Installer()));
	const Bytes pbp = ReadAll(M33Installer());
	// SystemControl, carved out of the installer as a standalone ~PSP file.
	const std::string in = TempPath("SystemControl.psp");
	std::string error;
	REQUIRE(WriteFileBytes(in, pbp.data() + 0x8ce2, 29650, &error));
	const std::string out = TempPath("out/SystemControl.prx");
	std::string text;
	CHECK_EQ(RunTool({ "prx", in, out, "--json" }, &text), 0);
	const Bytes elf = ReadAll(out);
	CHECK_EQ(elf.size(), (size_t)56350);
	CHECK_EQ(ElfType(elf.data(), elf.size()), (u16)0xFFA0);
	CHECK_EQ(Sha1Hex(elf.data(), elf.size()), std::string("86ce867a2f5ee06b177884c1b50033a5f4c50131"));
	CHECK(text.find("\"status\": \"ok\"") != std::string::npos);
	CHECK(text.find("\"elf_type\": \"0x0000ffa0\"") != std::string::npos);
	CHECK(!FileExists(out + ".part"));

	// A plain ELF is copied through.
	const std::string copy = TempPath("out/copy.prx");
	CHECK_EQ(RunTool({ "prx", out, copy, "-q" }, &text), 0);
	CHECK(ReadAll(copy) == elf);

	// Synthetic encrypted module.
	const Bytes testElf = MakeTestElf("CliTest", 5000, 21);
	const std::string enc = TempPath("cli-enc.prx");
	REQUIRE(WriteFileBytes(enc, MakePspModule(testElf, "CliTest", TestPacking::KirkType2, 0xD91609F0, "gzip", 22), &error));
	CHECK_EQ(RunTool({ "prx", enc, TempPath("out/cli-dec.prx") }, &text), 0);
	CHECK(ReadAll(TempPath("out/cli-dec.prx")) == testElf);

	// Exit 3 for a key we don't have, 2 for garbage; neither writes an output.
	const std::string unknown = TempPath("cli-unknown.prx");
	REQUIRE(WriteFileBytes(unknown, MakePspModule(testElf, "Unknown", TestPacking::UnknownTag, 0, "gzip", 23), &error));
	CHECK_EQ(RunTool({ "prx", unknown, TempPath("out/unknown.elf") }, &text), 3);
	CHECK(text.find("0x12345678") != std::string::npos);
	CHECK(!FileExists(TempPath("out/unknown.elf")));
	const std::string junk = TempPath("cli-junk.bin");
	REQUIRE(WriteFileBytes(junk, PseudoRandom(4000, 24), &error));
	CHECK_EQ(RunTool({ "prx", junk, TempPath("out/junk.elf") }, &text), 2);
	CHECK(!FileExists(TempPath("out/junk.elf")));
	CHECK_EQ(RunTool({ "prx", TempPath("missing.prx"), TempPath("out/missing.elf") }, &text), 2);
}

TEST(cli_pbp) {
	REQUIRE(FileExists(M33Installer()));
	std::string text;
	// The M33 installer: DATA.PSP is a plain ELF, so there's no DATA.ELF.
	const std::string dir = TempPath("pbp-m33");
	CHECK_EQ(RunTool({ "pbp", M33Installer(), dir }, &text), 0);
	CHECK_EQ(ReadAll(JoinPath(dir, "DATA.PSP")).size(), (size_t)487206);
	CHECK_EQ(ReadAll(JoinPath(dir, "PARAM.SFO")).size(), (size_t)2080);
	CHECK_EQ(ReadAll(JoinPath(dir, "ICON0.PNG")).size(), (size_t)8602);
	CHECK(!FileExists(JoinPath(dir, "DATA.ELF")));
	CHECK(!FileExists(JoinPath(dir, "DATA.PSAR")));

	// The SDK sample: DATA.PSP is psp-packer output, and DATA.ELF is the bootload.prx it came from.
	const std::string sample = JoinPath(g_env.repo, "reference/401M33/SDK/samples/bootload");
	const std::string dir2 = TempPath("pbp-sample");
	CHECK_EQ(RunTool({ "pbp", JoinPath(sample, "EBOOT.PBP"), dir2, "--json" }, &text), 0);
	CHECK(ReadAll(JoinPath(dir2, "DATA.ELF")) == ReadAll(JoinPath(sample, "bootload.prx")));
	CHECK(text.find("\"data_elf\"") != std::string::npos);
	CHECK_EQ(RunTool({ "pbp", JoinPath(sample, "bootload.prx"), dir2 }, &text), 2);
}

TEST(cli_carve) {
	REQUIRE(FileExists(M33Installer()));
	const std::string dir = TempPath("carve-m33");
	std::string text;
	CHECK_EQ(RunTool({ "carve", M33Installer(), dir, "--json", "--keep-encrypted" }, &text), 0);
	CHECK(text.find("\"found\": 11") != std::string::npos);
	CHECK(text.find("\"ok\": 11") != std::string::npos);
	const Bytes sysctrl = ReadAll(JoinPath(dir, "00_SystemControl.prx"));
	CHECK_EQ(Sha1Hex(sysctrl.data(), sysctrl.size()), std::string("86ce867a2f5ee06b177884c1b50033a5f4c50131"));
	CHECK_EQ(ReadAll(JoinPath(dir, "00_SystemControl.psp")).size(), (size_t)29650);
	const Bytes ipl = ReadAll(JoinPath(dir, "10_pspIplUpdate.prx"));
	CHECK_EQ(Sha1Hex(ipl.data(), ipl.size()), std::string("2dcd6c3152e4ccf07b2b606b46d2dc147dd65ee0"));
	CHECK_EQ(RunTool({ "carve", JoinPath(g_env.repo, "reference/401M33/SDK/samples/bootload/bootload.prx"), dir }, &text), 2);
}

TEST(cli_batch) {
	REQUIRE(FileExists(M33Installer()));
	const Bytes pbp = ReadAll(M33Installer());
	std::string error;
	const std::string a = TempPath("batch/VshControl.psp");
	const std::string b = TempPath("batch/M33IdCanager.psp");
	REQUIRE(WriteFileBytes(a, pbp.data() + 0x177e2, 11743, &error));
	REQUIRE(WriteFileBytes(b, pbp.data() + 0x29132, 2723, &error));
	const std::string unknown = TempPath("batch/unknown.psp");
	REQUIRE(WriteFileBytes(unknown, MakePspModule(MakeTestElf("U", 900, 1), "U", TestPacking::UnknownTag, 0, "gzip", 2), &error));

	// Paths with spaces, CRLF line ends, a comment and a blank line.
	const std::string outA = TempPath("batch out/vsh ctrl.prx");
	const std::string outB = TempPath("batch out/idc.prx");
	std::string list = "# AOT pipeline list\r\n" + a + "\t" + outA + "\r\n\r\n" + b + "\t" + outB + "\r\n";
	const std::string listFile = TempPath("batch/list.tsv");
	REQUIRE(WriteFileBytes(listFile, (const u8 *)list.data(), list.size(), &error));
	std::string text;
	CHECK_EQ(RunTool({ "batch", listFile, "--json" }, &text), 0);
	CHECK(text.find("\"ok\": 2") != std::string::npos);
	const Bytes va = ReadAll(outA), vb = ReadAll(outB);
	CHECK_EQ(Sha1Hex(va.data(), va.size()), std::string("6aaeb0df86bd9264a75b9653107fb12842695fef"));
	CHECK_EQ(Sha1Hex(vb.data(), vb.size()), std::string("83c7fdde29484196fddb078428238a742c82f0cd"));

	// Only a missing key: exit 3. Add a broken line too: exit 2, and the good lines still run.
	list += unknown + "\t" + TempPath("batch out/u.prx") + "\n";
	REQUIRE(WriteFileBytes(listFile, (const u8 *)list.data(), list.size(), &error));
	CHECK_EQ(RunTool({ "batch", listFile }, &text), 3);
	list += "no tab on this line\n";
	REQUIRE(WriteFileBytes(listFile, (const u8 *)list.data(), list.size(), &error));
	std::filesystem::remove(U8Path(outA));
	CHECK_EQ(RunTool({ "batch", listFile, "--json" }, &text), 2);
	CHECK(text.find("\"needs_key\": 1") != std::string::npos);
	CHECK(text.find("\"failed\": 1") != std::string::npos);
	CHECK(FileExists(outA));
	CHECK_EQ(RunTool({ "batch", TempPath("batch/missing.tsv") }, &text), 2);
}
