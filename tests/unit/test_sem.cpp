// SPDX-License-Identifier: GPL-2.0-or-later
#include "allegrex/sem.h"
#include "testing.h"

static Ctx MakeCtx() {
	Ctx c;
	memset(&c, 0, sizeof(c));
	return c;
}

// Build an R-type word: op=0 rs rt rd sa funct.
static constexpr u32 RType(u32 rs, u32 rt, u32 rd, u32 sa, u32 funct) {
	return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}

TEST(sem_div_edge_cases) {
	Ctx c = MakeCtx();
	c.r[4] = 0x80000000u;
	c.r[5] = 0xFFFFFFFFu;
	sem_DIV(&c, RType(4, 5, 0, 0, 26));
	CHECK_EQ(c.lo, 0x80000000u);
	CHECK_EQ(c.hi, 0u);

	c.r[4] = 5;
	c.r[5] = 0;
	sem_DIV(&c, RType(4, 5, 0, 0, 26));
	CHECK_EQ(c.lo, 0xFFFFFFFFu);
	CHECK_EQ(c.hi, 5u);

	c.r[4] = 0x10000;
	sem_DIVU(&c, RType(4, 5, 0, 0, 27));
	CHECK_EQ(c.lo, 0xFFFFFFFFu);
	c.r[4] = 0x1234;
	sem_DIVU(&c, RType(4, 5, 0, 0, 27));
	CHECK_EQ(c.lo, 0xFFFFu);
}

TEST(sem_zero_register) {
	Ctx c = MakeCtx();
	c.r[4] = 7;
	sem_ADDU(&c, RType(4, 4, 0, 0, 33));
	CHECK_EQ(c.r[0], 0u);
}

TEST(sem_unaligned_word) {
	Ctx c = MakeCtx();
	GWrite32(0x100, 0x44332211);
	GWrite32(0x104, 0x88776655);
	c.r[4] = 0x101;
	// lwl t0, 3(a0) ; lwr t0, 0(a0) loads the unaligned word at 0x101.
	sem_LWL(&c, (34u << 26) | (4u << 21) | (8u << 16) | 3u);
	sem_LWR(&c, (38u << 26) | (4u << 21) | (8u << 16) | 0u);
	CHECK_EQ(c.r[8], 0x55443322u);
}

TEST(sem_float_to_int) {
	Ctx c = MakeCtx();
	c.f[1] = 2.5f;
	sem_ROUND_W_S(&c, 0x4600080C);  // round.w.s f0, f1
	CHECK_EQ(c.fi[0], 2u);
	c.f[1] = 3.5f;
	sem_ROUND_W_S(&c, 0x4600080C);
	CHECK_EQ(c.fi[0], 4u);
	c.f[1] = 1e20f;
	sem_TRUNC_W_S(&c, 0x4600080D);
	CHECK_EQ(c.fi[0], 0x7FFFFFFFu);
}

TEST(sem_allegrex_bits) {
	Ctx c = MakeCtx();
	c.r[5] = 0x12345678;
	sem_WSBW(&c, RType(0, 5, 2, 3, 32) | (31u << 26));
	CHECK_EQ(c.r[2], 0x78563412u);
	sem_BITREV(&c, RType(0, 5, 2, 20, 32) | (31u << 26));
	CHECK_EQ(c.r[2], 0x1E6A2C48u);
	c.r[5] = 0x80;
	sem_SEB(&c, RType(0, 5, 2, 16, 32) | (31u << 26));
	CHECK_EQ(c.r[2], 0xFFFFFF80u);
}
