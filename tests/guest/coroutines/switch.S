# SPDX-License-Identifier: GPL-2.0-or-later
# co_switch(): leaves this context and resumes the other one where it last called co_switch, the
# way Daxter's task scheduler does it: save the callee-saved registers and $ra on this stack, swap
# $sp with g_other_sp, restore the other context's registers from its stack and `jr` to its saved
# $ra through $a1 (not `jr $ra`). Recompiled code must not run the other context inside this
# call's host frame, or the host stack grows with every switch.
	.set noreorder
	.set noat
	.text
	.globl co_switch
	.ent co_switch
co_switch:
	addiu $sp, $sp, -48
	sw $ra, 4($sp)
	sw $s0, 8($sp)
	sw $s1, 12($sp)
	sw $s2, 16($sp)
	sw $s3, 20($sp)
	sw $s4, 24($sp)
	sw $s5, 28($sp)
	sw $s6, 32($sp)
	sw $s7, 36($sp)
	sw $fp, 40($sp)
	sw $ra, 44($sp)
	move $t0, $sp
	lui $a0, %hi(g_other_sp)
	lw $a1, %lo(g_other_sp)($a0)
	move $sp, $a1
	sw $t0, %lo(g_other_sp)($a0)
	lw $a1, 4($sp)
	lw $s0, 8($sp)
	lw $s1, 12($sp)
	lw $s2, 16($sp)
	lw $s3, 20($sp)
	lw $s4, 24($sp)
	lw $s5, 28($sp)
	lw $s6, 32($sp)
	lw $s7, 36($sp)
	lw $fp, 40($sp)
	lw $ra, 44($sp)
	jr $a1
	addiu $sp, $sp, 48
	.end co_switch
