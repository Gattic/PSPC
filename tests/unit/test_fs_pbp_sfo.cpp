// SPDX-License-Identifier: GPL-2.0-or-later
// PbpFile and ParamSfo, on the real 4.01 M33 EBOOT.PBPs in reference/401M33 and on synthetic data.
#include <cstdio>
#include <cstring>

#include "fs/param_sfo.h"
#include "fs/pbp.h"
#include "test_fs_helpers.h"
#include "testing.h"

using namespace vfs;

namespace {

std::filesystem::path Ref(const char *rel) {
	std::filesystem::path p = fstest::FindRepoFile(rel);
	if (p.empty()) {
		printf("  SKIP: %s not found (set PSPC_REPO_ROOT)\n", rel);
	}
	return p;
}

}  // namespace

TEST(fs_pbp_m33_update) {
	std::filesystem::path path = Ref("reference/401M33/UPDATE/EBOOT.PBP");
	if (path.empty()) return;
	PbpFile pbp;
	CHECK(pbp.Open(path));
	CHECK(pbp.IsOpen());
	CHECK_EQ(pbp.Version(), 0x00010000u);
	CHECK_EQ(pbp.FileSize(), (u64)497928);
	CHECK_EQ(pbp.SectionOffset(PbpSection::ParamSfo), (u64)0x28);
	CHECK_EQ(pbp.SectionSize(PbpSection::ParamSfo), (u64)(0x848 - 0x28));
	CHECK_EQ(pbp.SectionSize(PbpSection::Icon0Png), (u64)(0x29E2 - 0x848));
	CHECK_EQ(pbp.SectionSize(PbpSection::Icon1Pmf), (u64)0);
	CHECK_EQ(pbp.SectionSize(PbpSection::Pic0Png), (u64)0);
	CHECK_EQ(pbp.SectionSize(PbpSection::Pic1Png), (u64)0);
	CHECK_EQ(pbp.SectionSize(PbpSection::Snd0At3), (u64)0);
	CHECK_EQ(pbp.SectionOffset(PbpSection::DataPsp), (u64)0x29E2);
	CHECK_EQ(pbp.SectionSize(PbpSection::DataPsp), (u64)(0x79908 - 0x29E2));
	CHECK_EQ(pbp.SectionSize(PbpSection::DataPsar), (u64)0);
	CHECK(!strcmp(PbpFile::SectionName(PbpSection::DataPsar), "DATA.PSAR"));

	std::vector<u8> icon, elf, psar, sfoBytes;
	CHECK(pbp.GetSection(PbpSection::Icon0Png, icon));
	CHECK(icon.size() > 8 && !memcmp(icon.data(), "\x89PNG\r\n\x1a\n", 8));
	CHECK(pbp.GetSection(PbpSection::DataPsp, elf));
	CHECK(elf.size() == 0x79908 - 0x29E2 && !memcmp(elf.data(), "\x7f" "ELF", 4));
	CHECK(pbp.GetSection(PbpSection::DataPsar, psar));
	CHECK(psar.empty());
	u8 head[4];
	CHECK_EQ(pbp.ReadSection(PbpSection::DataPsp, 0, head, 4), (size_t)4);
	CHECK(!memcmp(head, "\x7f" "ELF", 4));
	CHECK_EQ(pbp.ReadSection(PbpSection::DataPsp, elf.size() - 2, head, 4), (size_t)2);

	CHECK(pbp.GetSection(PbpSection::ParamSfo, sfoBytes));
	ParamSfo sfo;
	CHECK(sfo.Parse(sfoBytes));
	CHECK(sfo.GetString("TITLE") == "PSP\xE2\x84\xA2 Update ver 4.01");
	CHECK(sfo.GetString("DISC_ID") == "MSTKUPDATE");
	CHECK(sfo.GetString("CATEGORY") == "MG");
	CHECK(sfo.GetString("DISC_VERSION") == "1.00");
	CHECK(sfo.GetString("UPDATER_VER") == "6.66");
	CHECK(sfo.GetString("TITLE_2") == "Mise \xC3\xA0 jour PSP\xE2\x84\xA2 ver. 4.01");
	CHECK_EQ(sfo.GetInt("BOOTABLE"), 1u);
	CHECK_EQ(sfo.GetInt("PARENTAL_LEVEL"), 1u);
	CHECK_EQ(sfo.GetInt("REGION"), 0x8000u);
	CHECK_EQ(sfo.GetInt("NOPE", 77), 77u);
	CHECK(sfo.GetString("NOPE", "dflt") == "dflt");
	CHECK(sfo.GetString("BOOTABLE", "x") == "x");  // an int isn't a string
	CHECK_EQ(sfo.GetInt("TITLE", 5), 5u);
	CHECK_EQ(sfo.Entries().size(), (size_t)19);
	// Byte-exact round trip.
	CHECK(sfo.Serialize() == sfoBytes);
}

TEST(fs_pbp_bootload_sample) {
	std::filesystem::path path = Ref("reference/401M33/SDK/samples/bootload/EBOOT.PBP");
	std::filesystem::path sfoPath = Ref("reference/401M33/SDK/samples/bootload/PARAM.SFO");
	if (path.empty() || sfoPath.empty()) return;
	std::vector<u8> bytes, sfoFile;
	CHECK(ReadWholeFile(path, bytes));
	CHECK(ReadWholeFile(sfoPath, sfoFile));
	PbpFile pbp;
	CHECK(pbp.Open(bytes));  // from memory this time
	std::vector<u8> sfoBytes, data;
	CHECK(pbp.GetSection(PbpSection::ParamSfo, sfoBytes));
	CHECK(sfoBytes == sfoFile);
	for (PbpSection s : {PbpSection::Icon0Png, PbpSection::Icon1Pmf, PbpSection::Pic0Png, PbpSection::Pic1Png, PbpSection::Snd0At3}) {
		CHECK_EQ(pbp.SectionSize(s), (u64)0);
	}
	CHECK(pbp.GetSection(PbpSection::DataPsp, data));
	CHECK_EQ(data.size(), (size_t)(0xA7F1 - 0x1C0));
	CHECK(data.size() > 4 && !memcmp(data.data(), "~PSP", 4));
	CHECK_EQ(pbp.SectionSize(PbpSection::DataPsar), (u64)0);

	ParamSfo sfo;
	CHECK(sfo.Parse(sfoBytes));
	CHECK(sfo.GetString("TITLE") == "BootLoad Sample");  // stored without its NUL
	CHECK(sfo.GetString("DISC_ID") == "UCJS10041");
	CHECK(sfo.GetString("PSP_SYSTEM_VER") == "1.00");
	CHECK_EQ(sfo.GetInt("REGION"), 0x8000u);
	CHECK(sfo.Serialize() == sfoBytes);
}

TEST(fs_pbp_synthetic) {
	PbpFile pbp;
	CHECK(!pbp.Open(std::vector<u8>{'n', 'o', 'p', 'e'}));
	CHECK(!pbp.IsOpen());
	std::vector<u8> elf(100, 0);
	memcpy(elf.data(), "\x7f" "ELF", 4);
	CHECK(!pbp.Open(elf));
	CHECK(!pbp.Open(std::filesystem::path("Z:/missing/EBOOT.PBP")));
	// Header + "SFO" + "ICON" + rest empty, with DATA.PSP offset going backwards.
	std::vector<u8> b(0x28 + 7, 0);
	memcpy(b.data(), "\0PBP", 4);
	fstest::Put32LE(&b[4], 0x10000);
	const u32 offs[8] = {0x28, 0x2B, 0x2F, 0x2F, 0x2F, 0x2F, 0x10, 0x2F};
	for (int i = 0; i < 8; i++) fstest::Put32LE(&b[8 + i * 4], offs[i]);
	memcpy(&b[0x28], "SFOICON", 7);
	CHECK(pbp.Open(b));
	std::vector<u8> s;
	CHECK(pbp.GetSection(PbpSection::ParamSfo, s));
	CHECK(std::string(s.begin(), s.end()) == "SFO");
	CHECK(pbp.GetSection(PbpSection::Icon0Png, s));
	CHECK(std::string(s.begin(), s.end()) == "ICON");
	CHECK_EQ(pbp.SectionSize(PbpSection::Snd0At3), (u64)0);   // runs into DATA.PSP's bogus offset
	CHECK_EQ(pbp.SectionSize(PbpSection::DataPsp), (u64)0x1F);
	CHECK_EQ(pbp.SectionSize(PbpSection::DataPsar), (u64)0);
	CHECK_EQ(pbp.SectionSize(PbpSection::Count), (u64)0);
}

TEST(fs_param_sfo_edit) {
	ParamSfo sfo;
	CHECK(!sfo.Parse(nullptr, 0));
	const u8 junk[24] = {1, 2, 3};
	CHECK(!sfo.Parse(junk, sizeof(junk)));
	// Build a save data style SFO from scratch.
	sfo.SetString("TITLE", "My Game", 128);
	sfo.SetString("SAVEDATA_TITLE", "Slot 1", 128);
	sfo.SetString("CATEGORY", "MS");
	sfo.SetInt("PARENTAL_LEVEL", 5);
	const u8 params[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	sfo.SetBinary("SAVEDATA_PARAMS", params, sizeof(params), 128);
	std::vector<u8> bytes = sfo.Serialize();
	ParamSfo back;
	CHECK(back.Parse(bytes));
	CHECK(back.GetString("TITLE") == "My Game");
	CHECK(back.GetString("SAVEDATA_TITLE") == "Slot 1");
	CHECK(back.GetString("CATEGORY") == "MS");
	CHECK_EQ(back.GetInt("PARENTAL_LEVEL"), 5u);
	std::vector<u8> bin;
	CHECK(back.GetBinary("SAVEDATA_PARAMS", bin));
	CHECK(bin == std::vector<u8>(params, params + 8));
	CHECK(back.Serialize() == bytes);
	// Keys stay sorted, as the firmware writes them.
	const auto &e = back.Entries();
	CHECK_EQ(e.size(), (size_t)5);
	for (size_t i = 1; i < e.size(); i++) CHECK(e[i - 1].key < e[i].key);
	CHECK_EQ(e[0].maxLen, 4u);  // "MS\0" padded to 4
	// Layout details: the key table is padded to 4, every value takes its reserved size.
	const u32 keyTable = (u32)bytes[8] | (bytes[9] << 8);
	const u32 dataTable = (u32)bytes[12] | (bytes[13] << 8);
	CHECK_EQ(keyTable, 20u + 5 * 16);
	CHECK_EQ(dataTable % 4, 0u);
	CHECK_EQ(bytes.size(), (size_t)(dataTable + 4 + 128 + 4 + 128 + 128));
	// Replacing keeps the reservation; a fixed reservation truncates on a UTF-8 boundary.
	back.SetString("TITLE", "Another title");
	CHECK_EQ(back.Entries()[4].maxLen, 128u);
	back.SetString("CATEGORY", "\xE2\x84\xA2\xE2\x84\xA2", 5);
	CHECK(back.GetString("CATEGORY") == "\xE2\x84\xA2");
	back.SetString("CATEGORY", "a much longer category value");
	CHECK(back.GetString("CATEGORY") == "a much longer category value");
	CHECK_EQ(back.Entries()[0].maxLen, 32u);
	CHECK(back.Remove("SAVEDATA_PARAMS"));
	CHECK(!back.Remove("SAVEDATA_PARAMS"));
	CHECK(!back.Has("SAVEDATA_PARAMS"));
	ParamSfo again;
	CHECK(again.Parse(back.Serialize()));
	CHECK(again.GetString("TITLE") == "Another title");
	CHECK_EQ(again.Entries().size(), (size_t)4);
	// Truncated input is rejected or clamped, never read past the end.
	for (size_t cut = 0; cut < bytes.size(); cut += 7) {
		ParamSfo t;
		t.Parse(bytes.data(), cut);
	}
}
