// SPDX-License-Identifier: GPL-3.0-or-later
// ~PSP decoding: synthetic KIRK-encrypted modules (built by running the decryption backwards),
// custom firmware packing, wrappers, and the real M33 4.01 installer and SDK sample.
#include "test_util.h"

#include "formats.h"
#include "module.h"
#include "prx_decrypter.h"

static DecodeResult Decode(const Bytes &file, const u8 *seed = nullptr) {
	DecodeOptions options;
	options.seed = seed;
	DecodeResult r;
	DecodeExecutable(file.data(), file.size(), options, &r);
	return r;
}

TEST(prx_type0_roundtrip) {
	// 144-byte-key tags: the updater's 0x0E000000 (stored raw, scrambled at use) and two stored
	// pre-scrambled.
	for (u32 tag : { 0x0E000000u, 0x4467415Du, 0x00000000u }) {
		const Bytes elf = MakeTestElf("TypeZero", 3000 + (tag & 0xFF), tag);
		const Bytes module = MakePspModule(elf, "TypeZero", TestPacking::KirkType0, tag, "gzip", 11);
		REQUIRE(!module.empty());
		const DecodeResult r = Decode(module);
		CHECK(r.status == DecodeStatus::Ok);
		CHECK(r.encrypted);
		CHECK_EQ(r.prxType, 0);
		CHECK_EQ(r.compression, std::string("gzip"));
		CHECK(r.output == elf);
		CHECK(r.warnings.empty());
	}
}

TEST(prx_type2_roundtrip) {
	// 16-byte-key tags, including the ones 4.01-era kernel/VSH modules and UMD EBOOTs use.
	const u32 tags[] = { 0x4C9414F0, 0x4C9415F0, 0x457B0AF0, 0x38020AF0, 0xD91609F0, 0x4C940BF0, 0x0B2B0AF0 };
	int i = 0;
	for (u32 tag : tags) {
		for (const char *compression : { "gzip", "kl4e", "none" }) {
			const Bytes elf = MakeTestElf("TypeTwo", 2000 + i * 77, 100 + i);
			const Bytes module = MakePspModule(elf, "TypeTwo", TestPacking::KirkType2, tag, compression, 200 + i);
			i++;
			REQUIRE(!module.empty());
			const DecodeResult r = Decode(module);
			CHECK(r.status == DecodeStatus::Ok);
			if (r.status != DecodeStatus::Ok) {
				fprintf(stderr, "  tag 0x%08x %s: %s\n", tag, compression, r.error.c_str());
				continue;
			}
			CHECK_EQ(r.prxType, 2);
			CHECK(r.output == elf);
			CHECK_EQ(r.compression, std::string(strcmp(compression, "kl4e") == 0 ? "KL4E" : compression));
		}
	}
}

TEST(prx_rejects_bad_input) {
	const Bytes elf = MakeTestElf("Bad", 2000, 5);
	Bytes module = MakePspModule(elf, "Bad", TestPacking::KirkType2, 0x4C9414F0, "gzip", 9);
	// A flipped bit in the encrypted header area: the header hash no longer matches.
	Bytes bad = module;
	bad[0x145] ^= 0x01;
	DecodeResult r = Decode(bad);
	CHECK(r.status == DecodeStatus::Failed);
	CHECK(r.error.find("hash") != std::string::npos);
	// A flipped bit in the payload: KIRK's CMAC catches it.
	bad = module;
	bad[0x200] ^= 0x01;
	r = Decode(bad);
	CHECK(r.status == DecodeStatus::Failed);
	CHECK(r.error.find("KIRK") != std::string::npos);
	// Truncated.
	bad.assign(module.begin(), module.begin() + 0x300);
	r = Decode(bad);
	CHECK(r.status == DecodeStatus::Failed);
	CHECK(r.error.find("truncated") != std::string::npos);
	// A tag no table knows: we need a key we don't have.
	const Bytes unknown = MakePspModule(elf, "Unknown", TestPacking::UnknownTag, 0, "gzip", 9);
	r = Decode(unknown);
	CHECK(r.status == DecodeStatus::NeedsKey);
	CHECK_EQ(r.tag, 0x12345678u);
	// Not an executable at all.
	r = Decode(PseudoRandom(1000, 3));
	CHECK(r.status == DecodeStatus::Failed);
	// The input buffer is never modified, even in place.
	Bytes copy = module;
	Bytes out;
	CHECK(DecryptPRX(copy.data(), copy.size(), copy.size(), &out, nullptr, nullptr) > 0);
	CHECK(copy == module);
}

TEST(prx_cfw_packed_and_plain) {
	const Bytes elf = MakeTestElf("Packed", 4000, 6);
	// psp-packer style: ~PSP header, real-looking tag and oe_tag, plain gzip after the header.
	DecodeResult r = Decode(MakePspModule(elf, "Packed", TestPacking::CfwGzip, 0x4C9414F0, "gzip", 1));
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.cfwPacked);
	CHECK(!r.encrypted);
	CHECK(r.output == elf);
	// A ~PSP header in front of a plain ELF.
	r = Decode(MakePspModule(elf, "Plain", TestPacking::CfwGzip, 0x4C9414F0, "none", 1));
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.output == elf);
	// A plain ELF is copied through.
	r = Decode(elf);
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.output == elf);
	// A bare gzip / KL4E compressed module.
	r = Decode(GzipStore(elf));
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.output == elf);
	r = Decode(Kl4eEncodeLiterals(elf, 0x05));
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.output == elf);
}

TEST(prx_wrappers) {
	const Bytes elf = MakeTestElf("Wrapped", 2500, 7);
	const Bytes module = MakePspModule(elf, "Wrapped", TestPacking::KirkType2, 0xD91609F0, "gzip", 3);
	// ~SCE: a 0x40-byte header whose second word is its size.
	Bytes sce(0x40, 0);
	memcpy(sce.data(), "~SCE", 4);
	wr32(&sce[4], 0x40);
	sce.insert(sce.end(), module.begin(), module.end());
	DecodeResult r = Decode(sce);
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.output == elf);
	CHECK(!r.steps.empty() && r.steps[0].find("~SCE") == 0);

	// A PBP: DATA.PSP is decoded.
	Bytes pbp(0x28, 0);
	memcpy(pbp.data(), "\0PBP", 4);
	wr32(&pbp[4], 0x10000);
	for (int i = 0; i < 6; i++) {
		wr32(&pbp[8 + i * 4], 0x28);
	}
	wr32(&pbp[8 + 6 * 4], 0x28);
	wr32(&pbp[8 + 7 * 4], (u32)(0x28 + module.size()));
	pbp.insert(pbp.end(), module.begin(), module.end());
	r = Decode(pbp);
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.output == elf);

	// A PSPEDAT whose header wants the licensee key, around a module we can't decrypt: the error
	// says which key is missing.
	Bytes edat(0x90, 0);
	memcpy(edat.data(), "\0PSPEDAT", 8);
	edat[0x08] = 3;
	wr16(&edat[0x0C], 0x90);
	wr16(&edat[0x0E], 0x0101);
	memcpy(&edat[0x10], "UP0000-NPUZ00000_00-0000000000000000", 36);
	const Bytes unknown = MakePspModule(elf, "Sprx", TestPacking::UnknownTag, 0, "gzip", 4);
	edat.insert(edat.end(), unknown.begin(), unknown.end());
	r = Decode(edat);
	CHECK(r.status == DecodeStatus::NeedsKey);
	CHECK(r.error.find("licensee") != std::string::npos);
	EdatHeader h;
	CHECK(ParseEdat(edat.data(), edat.size(), &h));
	CHECK_EQ(h.flags, (u8)1);
	CHECK_EQ(h.contentId, std::string("UP0000-NPUZ00000_00-0000000000000000"));
	// Without the licensee flag the seed derives from the content ID alone.
	edat[0x0F] = 0;
	u8 seed1[16], seed2[16];
	CHECK(DeriveEdatModuleSeed(edat.data(), edat.size(), seed1, nullptr));
	CHECK(DeriveEdatModuleSeed(edat.data(), edat.size(), seed2, nullptr));
	CHECK(memcmp(seed1, seed2, 16) == 0);
	edat[0x10] = 'X';
	CHECK(DeriveEdatModuleSeed(edat.data(), edat.size(), seed2, nullptr));
	CHECK(memcmp(seed1, seed2, 16) != 0);
}

// The eleven gzip-packed ~PSP modules inside 4.01 M33's installer, with the sizes and hashes
// Python's zlib gives for them.
struct M33Module {
	u32 offset;
	u32 pspSize;
	u32 elfSize;
	const char *sha1;
};
static const M33Module kM33Modules[] = {
	{ 0x8ce2, 29650, 56350, "86ce867a2f5ee06b177884c1b50033a5f4c50131" },
	{ 0x100e2, 30429, 58130, "270248e3b66bc7febd24d74b4689841838ee6bc6" },
	{ 0x177e2, 11743, 29142, "6aaeb0df86bd9264a75b9653107fb12842695fef" },
	{ 0x1a5e2, 4588, 9498, "a7ba2d2c4198232b6a13ea15940a19ccbfaa56cc" },
	{ 0x1b7ce, 7327, 15126, "1eb8ffebf5cde117bde09d587538c574a580a99c" },
	{ 0x1d4a2, 24297, 57418, "6215c5c4127ce3ff717cb89bcf0946e2e3abc735" },
	{ 0x2338e, 17130, 19206, "a36ab21ab49d10e36502cd241f7346e5f4b7050d" },
	{ 0x276a2, 6799, 15022, "236419aa45b374989d29662a8bc5c0c7ae35e59e" },
	{ 0x29132, 2723, 5462, "83c7fdde29484196fddb078428238a742c82f0cd" },
	{ 0x29bd6, 3392, 7042, "81ff17e765e21218c88bfe004578e73f4b2b5e02" },
	{ 0x766c2, 2121, 4226, "2dcd6c3152e4ccf07b2b606b46d2dc147dd65ee0" },
};

TEST(m33_installer_modules) {
	const Bytes pbp = ReadAll(JoinPath(g_env.repo, "reference/401M33/UPDATE/EBOOT.PBP"));
	REQUIRE(pbp.size() == 497928);
	PbpFile p;
	std::string error;
	REQUIRE(p.Parse(pbp.data(), pbp.size(), &error));
	CHECK_EQ(SniffKind(pbp.data() + p.offset[PbpFile::DATA_PSP], p.size[PbpFile::DATA_PSP]), FileKind::Elf);

	const std::vector<size_t> found = FindEmbeddedModules(pbp.data(), pbp.size());
	CHECK_EQ(found.size(), sizeof(kM33Modules) / sizeof(kM33Modules[0]));
	for (size_t i = 0; i < found.size() && i < sizeof(kM33Modules) / sizeof(kM33Modules[0]); i++) {
		const M33Module &m = kM33Modules[i];
		CHECK_EQ(found[i], (size_t)m.offset);
		// Carve exactly psp_size bytes, as a standalone file would be.
		const Bytes file(pbp.begin() + m.offset, pbp.begin() + m.offset + m.pspSize);
		const DecodeResult r = Decode(file);
		CHECK(r.status == DecodeStatus::Ok);
		CHECK(r.cfwPacked);
		CHECK_EQ(r.output.size(), (size_t)m.elfSize);
		CHECK_EQ(ElfType(r.output.data(), r.output.size()), (u16)0xFFA0);
		CHECK_EQ(Sha1Hex(r.output.data(), r.output.size()), std::string(m.sha1));
		CHECK(r.warnings.empty());
	}

	// SystemControl's module info, through the same path `info` uses.
	const Bytes sysctrl(pbp.begin() + 0x8ce2, pbp.begin() + 0x8ce2 + 29650);
	const DecodeResult r = Decode(sysctrl);
	REQUIRE(r.status == DecodeStatus::Ok);
	const Json elf = DescribeElf(r.output.data(), r.output.size());
	const Json *mi = elf.Get("module_info");
	REQUIRE(mi != nullptr);
	CHECK_EQ(mi->Get("name")->AsString(), std::string("SystemControl"));
	CHECK_EQ(r.psp.modname, std::string("SystemControl"));
	CHECK_EQ(r.psp.tag, 0x4C9414F0u);
}

TEST(m33_sdk_sample_pbp) {
	// psp-packer's PBP mode: DATA.PSP is the gzip-packed bootload.prx that sits next to it.
	const std::string dir = JoinPath(g_env.repo, "reference/401M33/SDK/samples/bootload");
	const Bytes pbp = ReadAll(JoinPath(dir, "EBOOT.PBP"));
	const Bytes prx = ReadAll(JoinPath(dir, "bootload.prx"));
	REQUIRE(pbp.size() == 42993 && prx.size() == 94442);
	const DecodeResult r = Decode(pbp);
	CHECK(r.status == DecodeStatus::Ok);
	CHECK(r.cfwPacked);
	CHECK(r.output == prx);
	CHECK_EQ(r.psp.tag, 0xADF305F0u);
}
