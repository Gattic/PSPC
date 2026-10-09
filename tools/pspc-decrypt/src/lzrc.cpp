// SPDX-License-Identifier: GPL-3.0-or-later
// tlzrc.c: LZRC decoder
//   based on benhur's code, rewrite by tpu
//
// Ported from PPSSPP's Core/FileSystems/tlzrc.cpp - Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later. Changes: no framework dependencies, a const input, a consumed-bytes count,
// and the literal context mixes in the output position as the firmware's decoder (and KL4E)
// does - identical to tlzrc for lc <= 5, which is all the NPUMDIMG images tlzrc was written for.

#include "lzrc.h"

namespace {

struct LZRC_DECODE {
	// input stream
	const u8 *input;
	int in_ptr;
	int in_len;

	// output stream
	u8 *output;
	int out_ptr;
	int out_len;

	// Set when input/output bounds are exceeded; decompression should abort.
	int error;

	// range decode
	u32 range;
	u32 code;
	u8 lc;

	u8 bm_literal[8][256];
	// rc_bittree() is called on &bm_dist_bits[len_bits][dist_state] with a limit of up
	// to 44 (dist_state can be 7), and can index up to limit-1 past that base - i.e.
	// column index up to 7+43=50, so the second dimension must be at least 51.
	u8 bm_dist_bits[8][51];
	// rc_number() is called with &bm_dist[dist_bits][0] where dist_bits can reach 43.
	u8 bm_dist[44][8];
	u8 bm_match[8][8];
	u8 bm_len[8][31];
};

u8 rc_getbyte(LZRC_DECODE *rc) {
	if (rc->in_ptr >= rc->in_len) {
		rc->error = 1;
		return 0;
	}
	return rc->input[rc->in_ptr++];
}

void rc_putbyte(LZRC_DECODE *rc, u8 byte) {
	if (rc->out_ptr >= rc->out_len) {
		rc->error = 1;
		return;
	}
	rc->output[rc->out_ptr++] = byte;
}

void rc_init(LZRC_DECODE *rc, u8 *out, int out_len, const u8 *in, int in_len) {
	rc->input = in;
	rc->in_len = in_len;
	rc->in_ptr = 0;

	rc->output = out;
	rc->out_len = out_len;
	rc->out_ptr = 0;

	rc->error = 0;

	rc->range = 0xffffffff;
	rc->lc = rc_getbyte(rc);
	rc->code = (u32)rc_getbyte(rc) << 24;
	rc->code |= (u32)rc_getbyte(rc) << 16;
	rc->code |= (u32)rc_getbyte(rc) << 8;
	rc->code |= (u32)rc_getbyte(rc);

	memset(rc->bm_literal, 0x80, sizeof(rc->bm_literal));
	memset(rc->bm_dist_bits, 0x80, sizeof(rc->bm_dist_bits));
	memset(rc->bm_dist, 0x80, sizeof(rc->bm_dist));
	memset(rc->bm_match, 0x80, sizeof(rc->bm_match));
	memset(rc->bm_len, 0x80, sizeof(rc->bm_len));
}

/* range decode */

void normalize(LZRC_DECODE *rc) {
	if (rc->range < 0x01000000) {
		rc->range <<= 8;
		rc->code = (rc->code << 8) + rc_getbyte(rc);
	}
}

/* Decode a bit */
int rc_bit(LZRC_DECODE *rc, u8 *prob) {
	normalize(rc);

	const u32 bound = (rc->range >> 8) * (*prob);
	*prob -= *prob >> 3;

	if (rc->code < bound) {
		rc->range = bound;
		*prob += 31;
		return 1;
	} else {
		rc->code -= bound;
		rc->range -= bound;
		return 0;
	}
}

/* Decode a bittree starting from MSB */
int rc_bittree(LZRC_DECODE *rc, u8 *probs, int limit) {
	int number = 1;
	do {
		number = (number << 1) + rc_bit(rc, probs + number);
	} while (number < limit);
	return number;
}

/*
 * decode a number
 *
 * a number are divide into three part:
 *   MSB 2bits
 *   direct bits (don't use probability modle)
 *   LSB 3bits
 */
int rc_number(LZRC_DECODE *rc, u8 *prob, int n) {
	int i, number = 1;

	if (n > 3) {
		number = (number << 1) + rc_bit(rc, prob + 3);
		if (n > 4) {
			number = (number << 1) + rc_bit(rc, prob + 3);
			if (n > 5) {
				// direct bits
				normalize(rc);

				for (i = 0; i < n - 5; i++) {
					rc->range >>= 1;
					number <<= 1;
					if (rc->code < rc->range) {
						number += 1;
					} else {
						rc->code -= rc->range;
					}
				}
			}
		}
	}

	if (n > 0) {
		number = (number << 1) + rc_bit(rc, prob);
		if (n > 1) {
			number = (number << 1) + rc_bit(rc, prob + 1);
			if (n > 2) {
				number = (number << 1) + rc_bit(rc, prob + 2);
			}
		}
	}

	return number;
}

}  // namespace

int DecompressLZRC(u8 *out, int out_len, const u8 *in, int in_len, int *consumed) {
	// The decode state is a few KB of probability tables; keep it off the stack.
	std::vector<LZRC_DECODE> state(1);
	LZRC_DECODE &rc = state[0];
	int match_step, rc_state, len_state, dist_state;
	int i, bit, byte, last_byte;
	int match_len, len_bits;
	int match_dist, dist_bits, limit;

	if (consumed) {
		*consumed = 0;
	}
	rc_init(&rc, out, out_len, in, in_len);

	if (rc.error)
		return -1;

	if (rc.lc & 0x80) {
		/* plain text */
		const u32 copySize = rc.code;
		if (copySize > (u32)out_len || (u64)copySize + 5 > (u64)in_len) {
			return -1;
		}
		memcpy(rc.output, rc.input + 5, copySize);
		if (consumed) {
			*consumed = 5 + (int)copySize;
		}
		return (int)copySize;
	}

	rc_state = 0;
	last_byte = 0;

	while (1) {
		if (rc.error)
			return -1;

		match_step = 0;

		bit = rc_bit(&rc, &rc.bm_match[rc_state][match_step]);
		if (bit == 0) {
			/* 0 -> raw char */
			if (rc_state > 0)
				rc_state -= 1;

			const int ctx = ((((rc.out_ptr & 7) << 8) | last_byte) >> rc.lc) & 0x07;
			byte = rc_bittree(&rc, &rc.bm_literal[ctx][0], 0x100);
			byte -= 0x100;

			rc_putbyte(&rc, (u8)byte);
		} else {
			/* 1 -> a match */

			/* find bits of match length */
			len_bits = 0;
			for (i = 0; i < 7; i++) {
				match_step += 1;
				bit = rc_bit(&rc, &rc.bm_match[rc_state][match_step]);
				if (bit == 0)
					break;
				len_bits += 1;
			}

			/* find match length */
			if (len_bits == 0) {
				match_len = 1;
			} else {
				len_state = ((len_bits - 1) << 2) + ((rc.out_ptr << (len_bits - 1)) & 0x03);
				match_len = rc_number(&rc, &rc.bm_len[rc_state][len_state], len_bits);
				if (match_len == 0xFF) {
					// end of stream
					if (rc.error)
						return -1;
					if (consumed) {
						*consumed = rc.in_ptr;
					}
					return rc.out_ptr;
				}
			}

			/* find number of bits of match distance */
			dist_state = 0;
			limit = 8;
			if (match_len > 2) {
				dist_state += 7;
				limit = 44;
			}
			dist_bits = rc_bittree(&rc, &rc.bm_dist_bits[len_bits][dist_state], limit);
			dist_bits -= limit;

			/* find match distance */
			if (dist_bits > 0) {
				match_dist = rc_number(&rc, &rc.bm_dist[dist_bits][0], dist_bits);
			} else {
				match_dist = 1;
			}

			/* copy match bytes */
			if (match_dist > rc.out_ptr || match_dist < 0) {
				return -1;
			}
			const u8 *match_src = rc.output + rc.out_ptr - match_dist;
			for (i = 0; i < match_len + 1; i++) {
				rc_putbyte(&rc, *match_src++);
			}
			rc_state = 6 + ((rc.out_ptr + 1) & 1);
		}
		if (rc.error)
			return -1;
		last_byte = rc.output[rc.out_ptr - 1];
	}
}
