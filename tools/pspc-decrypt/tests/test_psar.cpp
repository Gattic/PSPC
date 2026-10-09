// SPDX-License-Identifier: GPL-3.0-or-later
// PSAR unpacking against synthetic updaters: a 4.x-style archive (grouped short names, DES file
// lists, per-model files, an IPL, loadexec with reboot.bin inside), a version 5 archive with real
// paths, and a pre-decrypted one. Real updaters aren't available here; these are built with the
// same layers in reverse, so every decoding step is exercised end to end.
#include "test_util.h"

#include <filesystem>

#include "psar.h"

#include "../libkirk/kirk_engine.h"

namespace {

const u8 kV5K1[16] = { 0xD8, 0x69, 0xB8, 0x95, 0x33, 0x6B, 0x63, 0x34, 0x98, 0xB9, 0xFC, 0x3C, 0xB7, 0x26, 0x2B, 0xD7 };
const u8 kV5K2[16] = { 0x0D, 0xA0, 0x90, 0x84, 0xAF, 0x9E, 0xB6, 0xE2, 0xD2, 0x94, 0xF2, 0xAA, 0xEF, 0x99, 0x68, 0x71 };

struct PsarBuilder {
	Bytes out;
	u8 version;
	bool predecrypted;
	u32 keySeed = 1000;

	PsarBuilder(u8 v, bool pre = false) : version(v), predecrypted(pre) {
		out.assign(0x10, 0);
		memcpy(out.data(), "PSAR", 4);
		out[4] = v;
	}

	// One record: the payload behind a 0x150-byte PRX header, encrypted and mangled.
	Bytes Block(const Bytes &plain) {
		if (predecrypted) {
			return plain;
		}
		u8 hdr[0x80] = {};
		hdr[0] = 0x55;
		Bytes blob = EncryptPrxType0(0x0E000000, hdr, plain, keySeed++);
		if (version == 5) {
			// The demangle is kirk7(x ^ K1) ^ K2; run it backwards.
			u8 *m = blob.data() + 0x20;
			for (int i = 0; i < 0x130; i++) {
				m[i] ^= kV5K2[i & 0xF];
			}
			kirk4(m, m, 0x130, 0x55);
			for (int i = 0; i < 0x130; i++) {
				m[i] ^= kV5K1[i & 0xF];
			}
		} else if (version != 1) {
			MangleBlock(blob.data());
		}
		return blob;
	}

	void Append(const Bytes &b) {
		out.insert(out.end(), b.begin(), b.end());
	}

	void Header(const std::string &versionText) {
		Bytes first(0x110, 0);
		memcpy(&first[0x10], versionText.data(), versionText.size());
		if (predecrypted) {
			wr32(&first[0x90], 100);
		}
		Append(Block(first));
		if (version != 1) {
			const Bytes second(100, 0x5A);
			Bytes block = Block(second);
			if (predecrypted) {
				Append(block);
			} else {
				// The reader advances by the header plus the payload rounded up to 16.
				Append(block);
			}
		}
	}

	void Entry(const std::string &name, const Bytes *contents, u8 flags = 0) {
		Bytes desc(0x110, 0);
		memcpy(&desc[4], name.data(), name.size());
		desc[0x10F] = flags;
		Bytes chunk;
		if (contents) {
			chunk = Block(ZlibStore(*contents));
		}
		wr32(&desc[0x104], (u32)chunk.size());
		wr32(&desc[0x108], contents ? (u32)contents->size() : 0);
		Append(Block(desc));
		Append(chunk);
	}

	Bytes Finish() {
		wr32(&out[8], (u32)out.size());
		// Real archives have a little padding after the declared end.
		out.insert(out.end(), 16, 0);
		return out;
	}
};

// An IPL: KIRK CMD1 blocks of 0x1000 bytes, each [addr, count, entry, checksum] + data.
Bytes MakeIpl(const Bytes &image, u32 loadAddress, u32 chunk) {
	Bytes ipl;
	for (size_t pos = 0; pos < image.size(); pos += chunk) {
		const u32 count = (u32)std::min<size_t>(chunk, image.size() - pos);
		Bytes data(0x10 + count, 0);
		wr32(&data[0], loadAddress + (u32)pos);
		wr32(&data[4], count);
		wr32(&data[8], pos + count >= image.size() ? loadAddress : 0);
		memcpy(&data[0x10], image.data() + pos, count);
		const Bytes block = KirkCmd1Block(Bytes(), data, 0x1D1 + (u32)pos);
		Bytes padded(0x1000, 0);
		memcpy(padded.data(), block.data(), block.size());
		ipl.insert(ipl.end(), padded.begin(), padded.end());
	}
	return ipl;
}

struct Fixture {
	Bytes sysmemElf, fontData, packedElf, loadexecElf, rebootPayload, kl4eElf, model01g, model02g, iplImage, version, dicData;
	Bytes sysmem, packed, loadexec, unknown, kl4e, model1, model2, ipl;
	Bytes psar;

	Fixture() {
		sysmemElf = MakeTestElf("sceSystemMemoryManager", 6000, 1, 0x1007);
		sysmem = MakePspModule(sysmemElf, "sceSystemMemoryManager", TestPacking::KirkType2, 0x4C9414F0, "gzip", 10);
		fontData = PseudoRandom(5000, 2);
		packedElf = MakeTestElf("M33Packed", 3000, 3);
		packed = MakePspModule(packedElf, "M33Packed", TestPacking::CfwGzip, 0x4C9414F0, "gzip", 11);

		rebootPayload = PseudoRandom(3000, 77);
		const Bytes reboot = MakePspModule(rebootPayload, "reboot", TestPacking::KirkType2, 0x4C9415F0, "gzip", 12);
		loadexecElf = MakeTestElf("sceLoadExec", 0x1000 + reboot.size() + 0x200, 4, 0x1007);
		memcpy(&loadexecElf[0x1000], reboot.data(), reboot.size());
		loadexec = MakePspModule(loadexecElf, "sceLoadExec", TestPacking::KirkType2, 0x4C9414F0, "kl4e", 13);

		unknown = MakePspModule(MakeTestElf("Unknown", 1000, 5), "Unknown", TestPacking::UnknownTag, 0, "gzip", 14);
		kl4eElf = MakeTestElf("sceMemlmd", 4000, 6, 0x1007);
		kl4e = MakePspModule(kl4eElf, "sceMemlmd", TestPacking::KirkType2, 0x457B0AF0, "kl4e", 15);
		model01g = MakeTestElf("sceModel", 2000, 7);
		model02g = MakeTestElf("sceModel", 2000, 8);
		model1 = MakePspModule(model01g, "sceModel", TestPacking::KirkType2, 0x4C9414F0, "gzip", 16);
		model2 = MakePspModule(model02g, "sceModel", TestPacking::KirkType2, 0x4C9415F0, "gzip", 17);
		iplImage = PseudoRandom(0x1800, 9);
		ipl = MakeIpl(iplImage, 0x040F0000, 0xF00);
		const std::string v = "release:4.01:\nbuild:test\n";
		version.assign(v.begin(), v.end());
		dicData = PseudoRandom(300, 10);

		PsarBuilder b(3);
		b.Header("pspc synthetic updater,4.01");
		const Bytes com = EncryptFileList(
			"00001|flash0/kd/sysmem.prx\r\n"
			"00002|flash0/font/ltn0.pgf\r\n"
			"00003|flash0/kd/m33packed.prx\r\n"
			"00004|flash0/kd/loadexec.prx\r\n"
			"00005|flash0/kd/unknown.prx\r\n"
			"00006|flash0/vsh/etc/version.txt\r\n"
			"00007|flash0/kd/memlmd.prx\r\n", 2);
		const Bytes list01 = EncryptFileList("00001|flash0/kd/model.prx\r\n00002|ipl/ipl_01g.bin\r\n", 2);
		const Bytes list02 = EncryptFileList("00001|flash0/kd/model.prx\r\n", 2);
		b.Entry("com:00000", &com);
		b.Entry("01g:00000", &list01);
		b.Entry("02g:00000", &list02);
		b.Entry("flash0:/vsh", nullptr);
		b.Entry("com:00001", &sysmem, 2);
		b.Entry("com:00002", &fontData);
		b.Entry("com:00003", &packed);
		b.Entry("com:00004", &loadexec);
		b.Entry("com:00005", &unknown);
		b.Entry("com:00006", &version);
		b.Entry("com:00007", &kl4e);
		b.Entry("01g:00001", &model1);
		b.Entry("02g:00001", &model2);
		b.Entry("01g:00002", &ipl);
		b.Entry("com:00099", &fontData);
		b.Entry("flash1:/dic/test.dic", &dicData);
		psar = b.Finish();
	}
};

const Fixture &GetFixture() {
	static Fixture f;
	return f;
}

Bytes Out(const std::string &dir, const std::string &rel) {
	return ReadAll(JoinPath(dir, rel));
}

const Json *FindEntry(const Json &manifest, const std::string &key, const std::string &value) {
	const Json *entries = manifest.Get("entries");
	for (size_t i = 0; entries && i < entries->Size(); i++) {
		const Json *v = entries->At(i).Get(key);
		if (v && v->type() == Json::Type::String && v->AsString() == value) {
			return &entries->At(i);
		}
	}
	return nullptr;
}

}  // namespace

TEST(psar_synthetic_401_any_model) {
	const Fixture &f = GetFixture();
	const std::string dir = TempPath("psar-any");
	PsarOptions options;
	options.keepEncrypted = true;
	Json manifest;
	PsarSummary s;
	std::string error;
	REQUIRE(UnpackPsar(f.psar.data(), f.psar.size(), dir, options, &manifest, &s, &error));
	CHECK(error.empty());
	CHECK_EQ(s.firmwareVersion, std::string("4.01"));
	CHECK_EQ(s.psarVersion, 3);
	CHECK_EQ(s.entries, 16);
	CHECK_EQ(s.fileLists, 3);
	CHECK_EQ(s.directories, 1);
	CHECK_EQ(s.modules, 7);
	CHECK_EQ(s.modulesDecrypted, 6);
	CHECK_EQ(s.needsKey, 1);
	CHECK_EQ(s.failed, 0);
	CHECK_EQ(s.unnamed, 1);
	CHECK_EQ(s.conflicts, 1);
	CHECK_EQ(s.derived, 2);  // reboot.bin and the IPL's first stage

	CHECK(Out(dir, "flash0/kd/sysmem.prx") == f.sysmemElf);
	CHECK(Out(dir, "encrypted/flash0/kd/sysmem.prx") == f.sysmem);
	CHECK(Out(dir, "flash0/font/ltn0.pgf") == f.fontData);
	CHECK(Out(dir, "flash0/kd/m33packed.prx") == f.packedElf);
	CHECK(Out(dir, "flash0/kd/loadexec.prx") == f.loadexecElf);
	CHECK(Out(dir, "derived/reboot.bin") == f.rebootPayload);
	CHECK(Out(dir, "flash0/kd/memlmd.prx") == f.kl4eElf);
	CHECK(Out(dir, "flash0/vsh/etc/version.txt") == f.version);
	CHECK(Out(dir, "flash1/dic/test.dic") == f.dicData);
	CHECK(Out(dir, "unnamed/com_00099") == f.fontData);
	// Both models' model.prx: the first one listed wins the path, the other is kept aside.
	CHECK(Out(dir, "flash0/kd/model.prx") == f.model01g);
	CHECK(Out(dir, "conflicts/02g/flash0/kd/model.prx") == f.model02g);
	// The IPL as stored, and its decrypted, linearized first stage.
	CHECK(Out(dir, "ipl/ipl_01g.bin") == f.ipl);
	CHECK(Out(dir, "derived/ipl/ipl_01g.bin.stage1.bin") == f.iplImage);
	// A module we can't decrypt stays in the tree as it was, and the manifest says so.
	CHECK(Out(dir, "flash0/kd/unknown.prx") == f.unknown);
	CHECK(FileExists(JoinPath(dir, "manifest.json")));
	CHECK(FileExists(JoinPath(dir, "psar/lists/com_00000.txt")));
	CHECK(std::filesystem::is_directory(U8Path(JoinPath(dir, "flash0/vsh"))));

	const Json *e = FindEntry(manifest, "path", "flash0/kd/unknown.prx");
	REQUIRE(e != nullptr);
	CHECK(!e->Get("decrypted")->AsBool());
	CHECK_EQ(e->Get("status")->AsString(), std::string("needs-key"));
	e = FindEntry(manifest, "path", "flash0/kd/sysmem.prx");
	REQUIRE(e != nullptr);
	CHECK(e->Get("decrypted")->AsBool());
	CHECK(e->Get("signcheck") && e->Get("signcheck")->AsBool());
	CHECK_EQ(e->Get("sha1")->AsString(), Sha1Hex(f.sysmemElf.data(), f.sysmemElf.size()));
	CHECK_EQ(e->Get("tag")->AsString(), std::string("0x4c9414f0"));
	CHECK_EQ(e->Get("device_path")->AsString(), std::string("flash0:/kd/sysmem.prx"));
	e = FindEntry(manifest, "path", "flash0/kd/memlmd.prx");
	REQUIRE(e != nullptr);
	CHECK_EQ(e->Get("compression")->AsString(), std::string("KL4E"));
	CHECK_EQ(manifest.Get("firmware_version")->AsString(), std::string("4.01"));
	CHECK_EQ(manifest.Get("table_key")->AsInt(), 2);
}

TEST(psar_synthetic_401_one_model) {
	const Fixture &f = GetFixture();
	const std::string dir = TempPath("psar-02g");
	PsarOptions options;
	options.model = "02g";
	Json manifest;
	PsarSummary s;
	std::string error;
	REQUIRE(UnpackPsar(f.psar.data(), f.psar.size(), dir, options, &manifest, &s, &error));
	CHECK(Out(dir, "flash0/kd/model.prx") == f.model02g);
	CHECK(!FileExists(JoinPath(dir, "ipl/ipl_01g.bin")));
	CHECK(!FileExists(JoinPath(dir, "encrypted/flash0/kd/sysmem.prx")));
	CHECK_EQ(s.otherModel, 2);
	CHECK_EQ(s.conflicts, 0);
	CHECK(Out(dir, "flash0/kd/sysmem.prx") == f.sysmemElf);

	PsarOptions bad;
	bad.model = "13g";
	CHECK(!UnpackPsar(f.psar.data(), f.psar.size(), dir, bad, &manifest, &s, &error));
}

TEST(psar_version5_real_paths) {
	PsarBuilder b(5);
	b.Header("pspc v5,6.20");
	const Bytes data = PseudoRandom(1234, 50);
	const Bytes elf = MakeTestElf("sceV5", 3000, 51);
	const Bytes module = MakePspModule(elf, "sceV5", TestPacking::KirkType2, 0x4C941DF0, "gzip", 52);
	b.Entry("flash0:/kd", nullptr);
	b.Entry("flash0:/kd/v5.prx", &module);
	b.Entry("flash0:/data/blob.bin", &data);
	const Bytes psar = b.Finish();
	const std::string dir = TempPath("psar-v5");
	PsarOptions options;
	Json manifest;
	PsarSummary s;
	std::string error;
	REQUIRE(UnpackPsar(psar.data(), psar.size(), dir, options, &manifest, &s, &error));
	CHECK_EQ(s.firmwareVersion, std::string("6.20"));
	CHECK_EQ(manifest.Get("table_key")->AsInt(), 5);
	CHECK(Out(dir, "flash0/kd/v5.prx") == elf);
	CHECK(Out(dir, "flash0/data/blob.bin") == data);
	CHECK_EQ(s.failed, 0);
}

TEST(psar_predecrypted) {
	PsarBuilder b(2, true);
	b.Header("333,3.52");  // a pre-decrypted archive's marker is the start of this text
	const Bytes data = PseudoRandom(999, 60);
	b.Entry("flash0:/font/x.pgf", &data);
	const Bytes psar = b.Finish();
	const std::string dir = TempPath("psar-pre");
	PsarOptions options;
	Json manifest;
	PsarSummary s;
	std::string error;
	REQUIRE(UnpackPsar(psar.data(), psar.size(), dir, options, &manifest, &s, &error));
	CHECK(manifest.Get("predecrypted")->AsBool());
	CHECK(Out(dir, "flash0/font/x.pgf") == data);
}

TEST(psar_truncated_and_garbage) {
	const Fixture &f = GetFixture();
	PsarOptions options;
	Json manifest;
	PsarSummary s;
	std::string error;
	// Cut in the middle: the declared size no longer fits.
	const Bytes cut(f.psar.begin(), f.psar.begin() + f.psar.size() / 2);
	CHECK(!UnpackPsar(cut.data(), cut.size(), TempPath("psar-cut"), options, &manifest, &s, &error));
	CHECK(error.find("truncated") != std::string::npos);
	// Corrupt an entry in the middle: the walk stops there but keeps what it has.
	Bytes bad = f.psar;
	bad[f.psar.size() / 2] ^= 0xFF;
	error.clear();
	CHECK(UnpackPsar(bad.data(), bad.size(), TempPath("psar-bad"), options, &manifest, &s, &error));
	CHECK(s.failed > 0);
	Bytes junk = PseudoRandom(4096, 1);
	memcpy(junk.data(), "PSAR", 4);
	wr32(&junk[8], 4096);
	CHECK(!UnpackPsar(junk.data(), junk.size(), TempPath("psar-junk"), options, &manifest, &s, &error));
}

// The same archive through the command line, wrapped in an updater EBOOT.PBP.
TEST(psar_cli_with_pbp) {
	const Fixture &f = GetFixture();
	Bytes pbp(0x28, 0);
	memcpy(pbp.data(), "\0PBP", 4);
	wr32(&pbp[4], 0x10000);
	const Bytes sfo = HexBytes("0050534601010000140000001400000000000000");
	for (int i = 0; i < 7; i++) {
		wr32(&pbp[8 + i * 4], 0x28 + (i ? (u32)sfo.size() : 0));
	}
	wr32(&pbp[8 + 7 * 4], 0x28 + (u32)sfo.size());
	pbp.insert(pbp.end(), sfo.begin(), sfo.end());
	pbp.insert(pbp.end(), f.psar.begin(), f.psar.end());
	const std::string in = TempPath("401-synthetic.PBP");
	std::string error;
	REQUIRE(WriteFileBytes(in, pbp, &error));

	const std::string dir = TempPath("psar-cli");
	std::string out;
	// One module needs a key we don't have: exit code 3.
	CHECK_EQ(RunTool({ "psar", in, dir, "--json", "--keep-encrypted" }, &out), 3);
	CHECK(out.find("\"firmware_version\": \"4.01\"") != std::string::npos);
	CHECK(out.find("\"modules_decrypted\": 6") != std::string::npos);
	CHECK(Out(dir, "flash0/kd/sysmem.prx") == f.sysmemElf);
	CHECK(Out(dir, "encrypted/flash0/kd/sysmem.prx") == f.sysmem);
	const Bytes manifest = Out(dir, "manifest.json");
	const std::string text(manifest.begin(), manifest.end());
	CHECK(text.find("\"path\": \"flash0/kd/loadexec.prx\"") != std::string::npos);
	CHECK(text.find("\"status\": \"needs-key\"") != std::string::npos);

	CHECK_EQ(RunTool({ "info", in }, &out), 0);
	CHECK(out.find("firmware_version: 4.01") != std::string::npos);
	CHECK_EQ(RunTool({ "psar", in, dir, "--model", "zz" }, &out), 1);
	CHECK_EQ(RunTool({ "psar", JoinPath(g_env.repo, "reference/401M33/UPDATE/EBOOT.PBP"), dir }, &out), 2);
	CHECK(out.find("not a firmware updater") != std::string::npos || out.find("no PSAR") != std::string::npos);
}
