// SPDX-License-Identifier: GPL-2.0-or-later
// Coroutines that switch stacks (co_switch in switch.S), as Daxter's task scheduler does: the main
// context and two tasks on stacks of their own pass control back and forth 100000 times. Run by
// tools/guest_tests.py in the interpreter and as freshly built native code, where every switch
// lands in the middle of a function that has no native entry point. The output is fixed
// (expected.txt): run counts, and sums kept in locals across the switches.
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("coroutines", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

void co_switch(void);
void *g_other_sp;  // the context co_switch resumes; it leaves the one it left there

#define ROUNDS 100000
#define STACK_WORDS 4096

static unsigned g_stack[2][STACK_WORDS] __attribute__((aligned(16)));
static unsigned g_runs[2];
static volatile unsigned g_sum[2];

static void Task(int id) {
	// Has to survive every switch (in a callee-saved register or on this task's stack).
	unsigned acc = 1000u * (unsigned)(id + 1);
	for (;;) {
		g_runs[id]++;
		acc += g_runs[id];
		g_sum[id] = acc;
		co_switch();  // back to main, whose context is in g_other_sp
	}
}

static void Task0(void) { Task(0); }
static void Task1(void) { Task(1); }

// A context that starts `entry`: a co_switch frame near the top of `stack` whose saved $ra is it.
static void *NewContext(unsigned *stack, void (*entry)(void)) {
	unsigned *frame = stack + STACK_WORDS - 16 - 12;  // 48 bytes, with 64 above for entry's use
	memset(frame, 0, 48);
	frame[1] = (unsigned)entry;   // 4(sp): loaded into $a1 and jumped to
	frame[11] = (unsigned)entry;  // 44(sp): $ra
	return frame;
}

int main(void) {
	void *ctx[2];
	unsigned i, check = 0;
	ctx[0] = NewContext(g_stack[0], Task0);
	ctx[1] = NewContext(g_stack[1], Task1);
	for (i = 0; i < ROUNDS; i++) {
		int t = (i % 3 == 2) ? 1 : 0;  // task 0 twice as often as task 1
		g_other_sp = ctx[t];
		co_switch();
		ctx[t] = g_other_sp;  // where the task stopped
		check += i ^ g_runs[t];
	}
	printf("rounds %u\n", ROUNDS);
	printf("task0 runs %u sum %u\n", g_runs[0], g_sum[0]);
	printf("task1 runs %u sum %u\n", g_runs[1], g_sum[1]);
	printf("check %u\n", check);
	printf("done\n");
	sceKernelExitGame();
	return 0;
}
