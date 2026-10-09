// SPDX-License-Identifier: GPL-3.0-or-later
// AES, SHA-1, KIRK and DES against published vectors and round trips.
#include "test_util.h"

#include "des.h"
#include "prx_decrypter.h"

#include "../libkirk/kirk_engine.h"
#include "../libkirk/SHA1.h"

static Bytes Sha1Of(const Bytes &data) {
	SHA_CTX ctx;
	SHAInit(&ctx);
	SHAUpdate(&ctx, data.data(), (int)data.size());
	Bytes out(20);
	SHAFinal(out.data(), &ctx);
	return out;
}

TEST(aes_fips197) {
	// FIPS-197 appendix C.1.
	const Bytes key = HexBytes("000102030405060708090a0b0c0d0e0f");
	const Bytes pt = HexBytes("00112233445566778899aabbccddeeff");
	AES_ctx ctx;
	AES_set_key(&ctx, key.data(), 128);
	u8 ct[16], back[16];
	AES_encrypt(&ctx, pt.data(), ct);
	CHECK(Bytes(ct, ct + 16) == HexBytes("69c4e0d86a7b0430d8cdb78070b4c55a"));
	AES_decrypt(&ctx, ct, back);
	CHECK(Bytes(back, back + 16) == pt);
}

TEST(aes_cmac_rfc4493) {
	const Bytes key = HexBytes("2b7e151628aed2a6abf7158809cf4f3c");
	const Bytes msg = HexBytes(
		"6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51"
		"30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710");
	AES_ctx ctx;
	AES_set_key(&ctx, key.data(), 128);
	struct { int len; const char *mac; } cases[] = {
		{ 0, "bb1d6929e95937287fa37d129b756746" },
		{ 16, "070a16b46b4d4144f79bdd9dd04a287c" },
		{ 40, "dfa66747de9ae63030ca32611497c827" },
		{ 64, "51f0bebf7e3b9d92fc49741779363cfe" },
	};
	for (const auto &c : cases) {
		u8 mac[16];
		AES_CMAC(&ctx, msg.data(), c.len, mac);
		CHECK(Bytes(mac, mac + 16) == HexBytes(c.mac));
	}
}

TEST(sha1_vectors) {
	CHECK(Sha1Of(Bytes()) == HexBytes("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
	const std::string abc = "abc";
	CHECK(Sha1Of(Bytes(abc.begin(), abc.end())) == HexBytes("a9993e364706816aba3e25717850c26c9cd0d89d"));
	const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(Sha1Of(Bytes(two.begin(), two.end())) == HexBytes("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
	CHECK(Sha1Hex((const u8 *)abc.data(), 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

// KIRK CMD11 (SHA-1 with a length header), from pspautotests' kirk test: 0x20 zero bytes.
TEST(kirk_cmd11_sha1) {
	KirkState kirk{};
	kirk_init(&kirk);
	u8 in[4 + 0x20] = {};
	in[0] = 0x20;
	u8 out[0x14];
	CHECK_EQ(kirk_sceUtilsBufferCopyWithRange(&kirk, out, 0x14, in, sizeof(in), KIRK_CMD_SHA1_HASH), 0);
	CHECK(Bytes(out, out + 20) == HexBytes("de8a847bff8c343d69b853a215e6ee775ef2ef96"));
}

// A real signature: the NPEH90049 demo's NPUMDIMG header, checked against np9660's public key
// (pspautotests tests/kirk).
TEST(kirk_cmd17_known_signature) {
	const Bytes pub = HexBytes(
		"0121ea6ecdb23a3e2375671c5362e8e28b1e783b1a2732158b8ced98466c18a3ac3b1106afb4ec3b");
	const Bytes hash = HexBytes("2c39c14622d555023a03b12d170000368c28bd50");
	const Bytes sig = HexBytes(
		"4bbcbcb50170cd23206f519abed7d8cc04564c9e17e01e2e63123860580b21849f5213f1312c6abc");
	Bytes buf = pub;
	buf.insert(buf.end(), hash.begin(), hash.end());
	buf.insert(buf.end(), sig.begin(), sig.end());
	KirkState kirk{};
	kirk_init(&kirk);
	CHECK_EQ(kirk_CMD17(&kirk, buf.data(), (int)buf.size()), 0);
	buf[0x28] ^= 1;  // a different message hash
	CHECK(kirk_CMD17(&kirk, buf.data(), (int)buf.size()) != 0);
}

// A real KIRK CMD1 block signed with ECDSA, from the kirk-engine test harness via pspautotests.
TEST(kirk_cmd1_ecdsa_sample) {
	Bytes block = HexBytes(
		"106d655df7b4c0415dab173cae6dd8f2664fe1f2e9d66336f7330bcab9556db6"
		"ebe805dcf557e2f8c81fd95cb60b601bf0862ddb1fcb4eafcde388a63c1d57dc"
		"5e94eeac2e6c9f2e81c71c583af18174cbb5c6104b09d17908b8b74492085c23"
		"0100000001000000000000000000000026000000800000000000000000000000"
		"000000000000000000000000000000007e5053500710010008016578616d706c"
		"6531000000000000000000000000000000000000000000000000000000000000"
		"0000000000000000000000000000000000000000000000000000000000000000"
		"0000000000000000000000000000000000000000000000000000000000000000"
		"00000000000000000000000000000000ee928eae50ca4e01199c1ca690b86170"
		"0a9554578fc5ef2ca249a6b508df8c782e3894289be11fc4ce61db320b5e05f0");
	REQUIRE(block.size() == 320);
	KirkState kirk{};
	kirk_init(&kirk);
	u8 out[320] = {};
	CHECK_EQ(kirk_CMD1(&kirk, out, block.data(), (int)block.size()), 0);
	block[0x130] ^= 1;  // the signed data
	CHECK(kirk_CMD1(&kirk, out, block.data(), (int)block.size()) != 0);
}

TEST(kirk_cmd4_cmd7_roundtrip) {
	KirkState kirk{};
	kirk_init(&kirk);
	for (int keyseed : { 0x02, 0x03, 0x42, 0x55, 0x5D, 0x63 }) {
		const Bytes plain = PseudoRandom(0x100, keyseed);
		Bytes in(0x14 + 0x100), enc(0x14 + 0x100), dec(0x100);
		wr32(&in[0], KIRK_MODE_ENCRYPT_CBC);
		wr32(&in[0x0C], keyseed);
		wr32(&in[0x10], 0x100);
		memcpy(&in[0x14], plain.data(), 0x100);
		CHECK_EQ(kirk_CMD4(&kirk, enc.data(), in.data(), (int)in.size()), 0);
		wr32(&enc[0], KIRK_MODE_DECRYPT_CBC);
		wr32(&enc[0x0C], keyseed);
		wr32(&enc[0x10], 0x100);
		CHECK_EQ(kirk_CMD7(&kirk, dec.data(), enc.data(), (int)enc.size()), 0);
		CHECK(dec == plain);
		// The overhead-free helpers agree with the commands.
		Bytes quick(0x100);
		kirk4(quick.data(), plain.data(), 0x100, keyseed);
		CHECK(memcmp(quick.data(), &enc[0x14], 0x100) == 0);
		kirk7(quick.data(), quick.data(), 0x100, keyseed);
		CHECK(quick == plain);
	}
}

TEST(kirk_cmd0_cmd1_roundtrip) {
	const Bytes area = PseudoRandom(0x40, 1);
	const Bytes plain = PseudoRandom(0x123, 2);
	Bytes block = KirkCmd1Block(area, plain, 3);
	KirkState kirk{};
	kirk_init(&kirk);
	Bytes copy = block;
	CHECK_EQ(kirk_CMD10(&kirk, copy.data(), (int)copy.size()), 0);
	Bytes out(block.size());
	CHECK_EQ(kirk_CMD1(&kirk, out.data(), copy.data(), (int)copy.size()), 0);
	CHECK(memcmp(out.data(), plain.data(), plain.size()) == 0);
	// The data CMAC covers the payload...
	copy = block;
	copy[0x90 + 0x40 + 5] ^= 0x80;
	CHECK_EQ(kirk_CMD1(&kirk, out.data(), copy.data(), (int)copy.size()), KIRK_DATA_HASH_INVALID);
	// ...and the header CMAC the header.
	copy = block;
	copy[0x70] ^= 1;
	CHECK(kirk_CMD1(&kirk, out.data(), copy.data(), (int)copy.size()) != 0);
}

TEST(kirk_ecdsa_sign_verify) {
	KirkState kirk{};
	kirk_init(&kirk);
	u8 keypair[0x3C];
	CHECK_EQ(kirk_CMD12(&kirk, keypair, 0x3C), 0);

	// Sign with CMD16 (it takes the private key encrypted under the fuse-derived key).
	u8 signIn[0x34] = {};
	u8 priv[0x20] = {};
	memcpy(priv, keypair, 0x14);
	encrypt_kirk16_private(&kirk, signIn, priv);
	const Bytes hash = PseudoRandom(0x14, 9);
	memcpy(signIn + 0x20, hash.data(), 0x14);
	u8 sig[0x28];
	CHECK_EQ(kirk_CMD16(&kirk, sig, 0x28, signIn, 0x34), 0);

	u8 verify[0x64];
	memcpy(verify, keypair + 0x14, 0x28);
	memcpy(verify + 0x28, hash.data(), 0x14);
	memcpy(verify + 0x3C, sig, 0x28);
	CHECK_EQ(kirk_CMD17(&kirk, verify, 0x64), 0);
	verify[0x30] ^= 0x10;
	CHECK(kirk_CMD17(&kirk, verify, 0x64) != 0);

	// CMD13 point multiplication: k2 * (k1 * G) == k1 * (k2 * G).
	u8 keypair2[0x3C];
	CHECK_EQ(kirk_CMD12(&kirk, keypair2, 0x3C), 0);
	u8 in1[0x3C], in2[0x3C], p1[0x28], p2[0x28];
	memcpy(in1, keypair2, 0x14);
	memcpy(in1 + 0x14, keypair + 0x14, 0x28);
	memcpy(in2, keypair, 0x14);
	memcpy(in2 + 0x14, keypair2 + 0x14, 0x28);
	CHECK_EQ(kirk_CMD13(&kirk, p1, 0x28, in1, 0x3C), 0);
	CHECK_EQ(kirk_CMD13(&kirk, p2, 0x28, in2, 0x3C), 0);
	CHECK(memcmp(p1, p2, 0x28) == 0);
}

// pspdecrypt carries tag 0x0E000000's key pre-scrambled; PPSSPP carries it raw and scrambles it
// with kirk7 at use. They must agree.
TEST(psar_tag_key_forms_agree) {
	const Bytes prescrambled = HexBytes(
		"a00cff959724d07e1f076331c0fb266e879cccf9d23e9da647c604f07fa35601"
		"8b333d9f301ce4754b14bee65b35f635132e6681b59566db1609bdcb9a90a4ab"
		"55f51f9255e60489c30ee6647c993785d35e78ff1cd1770f2de8e1479061347a"
		"e6c46fd5e40eabe91ea62e2f377c3874db975373dbb64c593b7524538c76880d"
		"412b2ce3baee941e73cf5d17b6f0141a");
	u8 key[0x90], code = 0;
	REQUIRE(PrxDebugTagKey(0x0E000000, key, &code));
	CHECK_EQ(code, (u8)0x51);
	CHECK(Bytes(key, key + 0x90) == prescrambled);
}

TEST(des_vectors) {
	// The classic worked example (FIPS 46 tutorials): key 133457799BBCDFF1.
	const Bytes key = HexBytes("133457799bbcdff1");
	const Bytes pt = HexBytes("0123456789abcdef");
	DES des(key.data());
	u8 ct[8], back[8];
	des.EncryptBlock(pt.data(), ct);
	CHECK(Bytes(ct, ct + 8) == HexBytes("85e813540f0ab405"));
	des.DecryptBlock(ct, back);
	CHECK(Bytes(back, back + 8) == pt);
	// NIST SP 800-17 variable plaintext known answer: key 0101..01, pt 8000..00.
	const Bytes weakKey = HexBytes("0101010101010101");
	DES des2(weakKey.data());
	des2.EncryptBlock(HexBytes("8000000000000000").data(), ct);
	CHECK(Bytes(ct, ct + 8) == HexBytes("95f8a5e5dd31d900"));
	// CBC round trip.
	Bytes data = PseudoRandom(64, 5);
	const Bytes orig = data;
	const u8 iv[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	des.CbcEncrypt(data.data(), data.size(), iv);
	CHECK(data != orig);
	des.CbcDecrypt(data.data(), data.size(), iv);
	CHECK(data == orig);
}
