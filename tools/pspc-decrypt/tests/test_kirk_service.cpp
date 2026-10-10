// SPDX-License-Identifier: GPL-3.0-or-later
// The kirk-service command handler: KIRK 4/7 and the console-key commands 5/8 round trip in place,
// the console key depends on the fuse ID, and malformed headers are refused.
#include "test_util.h"

#include "kirk_service.h"

static Bytes AesHeader(u32 mode, u32 keyseed, u32 size) {
	Bytes h(0x14, 0);
	wr32(h.data(), mode);
	wr32(h.data() + 12, keyseed);
	wr32(h.data() + 16, size);
	return h;
}

static Bytes RoundTrip(KirkServiceState *s, int enc, int dec, u32 keyseed, const Bytes &plain, Bytes *cipherOut) {
	Bytes buf = AesHeader(4, keyseed, (u32)plain.size());
	buf.insert(buf.end(), plain.begin(), plain.end());
	CHECK_EQ(KirkServiceCommand(s, enc, buf.data(), (int)buf.size(), buf.data(), (int)buf.size()), 0);
	*cipherOut = Bytes(buf.begin() + 0x14, buf.end());
	Bytes back = AesHeader(5, keyseed, (u32)plain.size());
	back.insert(back.end(), cipherOut->begin(), cipherOut->end());
	CHECK_EQ(KirkServiceCommand(s, dec, back.data(), (int)back.size(), back.data(), (int)back.size()), 0);
	return Bytes(back.begin(), back.begin() + plain.size());
}

TEST(kirk_service_round_trips) {
	KirkServiceState *s = KirkServiceCreate(0x123456789ABCull);
	Bytes plain(64);
	for (size_t i = 0; i < plain.size(); i++) plain[i] = (u8)(i * 7 + 1);
	Bytes c4, c5;
	CHECK(RoundTrip(s, 4, 7, 3, plain, &c4) == plain);
	CHECK(RoundTrip(s, 5, 8, 256, plain, &c5) == plain);
	CHECK(c4 != plain);
	CHECK(c5 != plain && c5 != c4);
	// Another console, another key for 5/8 (but not for 4/7).
	KirkServiceState *other = KirkServiceCreate(0x0FEDCBA98765ull);
	Bytes o4, o5;
	RoundTrip(other, 4, 7, 3, plain, &o4);
	RoundTrip(other, 5, 8, 256, plain, &o5);
	CHECK(o4 == c4);
	CHECK(o5 != c5);
	KirkServiceDestroy(other);

	// Ping, PRNG, and refused requests.
	CHECK_EQ(KirkServiceCommand(s, 0, nullptr, 0, nullptr, 0), 0);
	u8 rnd[20] = {};
	CHECK_EQ(KirkServiceCommand(s, 14, rnd, 20, nullptr, 0), 0);
	Bytes bad = AesHeader(4, 3, 0x1000);  // claims more data than the buffer holds
	bad.resize(0x14 + 16);
	CHECK(KirkServiceCommand(s, 4, bad.data(), (int)bad.size(), bad.data(), (int)bad.size()) != 0);
	Bytes wrongMode = AesHeader(5, 256, 16);
	wrongMode.resize(0x14 + 16);
	CHECK(KirkServiceCommand(s, 5, wrongMode.data(), (int)wrongMode.size(), wrongMode.data(), (int)wrongMode.size()) != 0);
	CHECK(KirkServiceCommand(s, 99, nullptr, 0, nullptr, 0) != 0);
	KirkServiceDestroy(s);
}
