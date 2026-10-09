// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>

#include "allegrex/decode.h"
#include "testing.h"

using ag::Decode;
using ag::Op;

TEST(decode_basic) {
	CHECK(Decode(0x00851021) == Op::ADDU);  // addu v0, a0, a1
	CHECK(Decode(0x27BDFFF0) == Op::ADDIU); // addiu sp, sp, -16
	CHECK(Decode(0x03E00008) == Op::JR);    // jr ra
	CHECK(Decode(0x0C000000) == Op::JAL);
	CHECK(Decode(0x00000000) == Op::SLL);   // nop
	CHECK(Decode(0x0000000C) == Op::SYSCALL);
	CHECK(Decode(0x8FBF0000) == Op::LW);
	CHECK(Decode(0xAFBF0000) == Op::SW);
}

TEST(decode_allegrex) {
	CHECK(Decode((1u << 21) | (4u << 16) | (2u << 11) | (3u << 6) | 2u) == Op::ROTR);
	CHECK(Decode((4u << 16) | (2u << 11) | (3u << 6) | 2u) == Op::SRL);
	CHECK(Decode(0x00000016) == Op::CLZ);
	CHECK(Decode(0x0000001C) == Op::MADD);
	CHECK(Decode(0x0000002C) == Op::MAX);
	CHECK(Decode(0x0000002D) == Op::MIN);
	CHECK(Decode(0x7C000000) == Op::EXT);
	CHECK(Decode(0x7C000004) == Op::INS);
	CHECK(Decode(0x7C000420) == Op::SEB);
	CHECK(Decode(0x7C000620) == Op::SEH);
	CHECK(Decode(0x7C0000A0) == Op::WSBH);
	CHECK(Decode(0x7C0000E0) == Op::WSBW);
	CHECK(Decode(0x7C000520) == Op::BITREV);
}

TEST(decode_fpu) {
	CHECK(Decode(0x46000000) == Op::ADD_S);
	CHECK(Decode(0x46800020) == Op::CVT_S_W);
	CHECK(Decode(0x46000024) == Op::CVT_W_S);
	CHECK(Decode(0x4600003C) == Op::C_COND_S);  // c.lt.s
	CHECK(Decode(0x45000000) == Op::BC1F);
	CHECK(Decode(0x45010000) == Op::BC1T);
	CHECK(Decode(0xC4000000) == Op::LWC1);
}

TEST(disassemble) {
	char buf[128];
	ag::Disassemble(0x08804000, 0x00851021, buf, sizeof(buf));
	CHECK(strcmp(buf, "addu v0, a0, a1") == 0);
	ag::Disassemble(0x08804000, 0x03E00008, buf, sizeof(buf));
	CHECK(strcmp(buf, "jr ra") == 0);
}
