/* SPDX-License-Identifier: GPL-2.0 */
/*
 * setjmp/longjmp for ARCH=um SUBARCH=arm64.
 *
 * UML uses its own setjmp/longjmp for kernel-side context switching rather than
 * libc's -- arch/um/Makefile renames the symbols to kernel_setjmp/kernel_longjmp
 * to keep libc's out of the way. libc's versions are unsuitable because they may
 * mangle the saved SP and PC with a per-process pointer guard, and glibc's
 * siglongjmp variant touches the signal mask; UML needs a plain, predictable
 * register save.
 *
 * The buffer layout matches struct __jmp_buf in
 * arch/arm64/um/shared/sysdep/archsetjmp.h:
 *
 *	  0	x19		 88	x30 (return address)
 *	  8	x20		 96	sp
 *	 16	x21		104	(padding, keeps 16-byte alignment)
 *	 24	x22		112	d8
 *	 32	x23		120	d9
 *	 40	x24		128	d10
 *	 48	x25		136	d11
 *	 56	x26		144	d12
 *	 64	x27		152	d13
 *	 72	x28		160	d14
 *	 80	x29 (fp)	168	d15
 *
 * That is exactly the AAPCS64 callee-saved set: x19-x28, the frame pointer, the
 * link register, SP, and the low 64 bits of v8-v15. Note the *low 64 bits* only
 * -- AAPCS64 requires callees to preserve no more than that, so saving full
 * 128-bit vectors would be wasted work, and the guest's vector state never
 * passes through here in any case (it lives in the stub's mcontext).
 */

	.text
	.align 4
	.globl kernel_setjmp
	.type kernel_setjmp, @function
kernel_setjmp:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]

	/*
	 * SP cannot be an operand of stp, so route it through a scratch
	 * register. x2 is caller-saved, so clobbering it is free.
	 */
	mov	x2, sp
	str	x2, [x0, #96]

	stp	d8,  d9,  [x0, #112]
	stp	d10, d11, [x0, #128]
	stp	d12, d13, [x0, #144]
	stp	d14, d15, [x0, #160]

	mov	w0, #0
	ret

	.size kernel_setjmp,.-kernel_setjmp

	.text
	.align 4
	.globl kernel_longjmp
	.type kernel_longjmp, @function
kernel_longjmp:
	ldp	x19, x20, [x0, #0]
	ldp	x21, x22, [x0, #16]
	ldp	x23, x24, [x0, #32]
	ldp	x25, x26, [x0, #48]
	ldp	x27, x28, [x0, #64]
	ldp	x29, x30, [x0, #80]

	ldr	x2, [x0, #96]
	mov	sp, x2

	ldp	d8,  d9,  [x0, #112]
	ldp	d10, d11, [x0, #128]
	ldp	d12, d13, [x0, #144]
	ldp	d14, d15, [x0, #160]

	/*
	 * longjmp(buf, 0) must appear to setjmp as a return of 1, otherwise the
	 * caller cannot distinguish it from the original setjmp.
	 * csinc w0, w1, wzr, ne  =>  w0 = (w1 != 0) ? w1 : wzr + 1
	 */
	cmp	w1, #0
	csinc	w0, w1, wzr, ne

	ret

	.size kernel_longjmp,.-kernel_longjmp

/* UML's binary is an ordinary ELF executable; keep the stack non-executable. */
	.section .note.GNU-stack,"",@progbits
