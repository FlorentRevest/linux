/* SPDX-License-Identifier: GPL-2.0 */
/*
 * trampoline entry and return code for rethook.
 * Copied from arch/arm64/kernel/probes/kprobes_trampoline.S
 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>

	.text

	.macro	save_all_base_regs
	stp	x0, x1, [sp, #FREGS_X0]
	stp	x2, x3, [sp, #FREGS_X2]
	stp	x4, x5, [sp, #FREGS_X4]
	stp	x6, x7, [sp, #FREGS_X6]
	str	x8,     [sp, #FREGS_X8]
	add x0, sp, #FREGS_SIZE
	str	x0, [sp, #FREGS_SP]

	str	x29, [sp, #FREGS_FP]
	str lr,  [sp, #FREGS_LR]
	str	xzr, [sp, #FREGS_PC]
	.endm

	.macro	restore_all_base_regs
	ldp	x0, x1, [sp, #FREGS_X0]
	ldp	x2, x3, [sp, #FREGS_X2]
	ldp	x4, x5, [sp, #FREGS_X4]
	ldp	x6, x7, [sp, #FREGS_X6]
	ldr	x8,     [sp, #FREGS_X8]
	ldr	x29,    [sp, #FREGS_FP]
	.endm

SYM_CODE_START(arch_rethook_trampoline)
	sub sp, sp, #FREGS_SIZE


	save_all_base_regs

	/* Setup a frame pointer. */
	add x29, sp, #S_FP

	mov x0, sp
	bl arch_rethook_trampoline_callback
	/*
	 * Replace trampoline address in lr with actual orig_ret_addr return
	 * address.
	 */
	mov lr, x0

	/* The frame pointer (x29) is restored with other registers. */
	restore_all_base_regs

	add sp, sp, #FREGS_SIZE
	ret

SYM_CODE_END(arch_rethook_trampoline)
