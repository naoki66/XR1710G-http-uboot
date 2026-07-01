	.section .text.boot, "ax"
	.align 7
	.globl _start
_start:
	b	reset
	.long	0
	.quad	0x0000000000088000
	.quad	_image_end - _start
	.quad	0x000000000000000a
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"
	.long	0

reset:
	adrp	x1, __bss_start
	add	x1, x1, :lo12:__bss_start
	adrp	x2, __bss_end
	add	x2, x2, :lo12:__bss_end
1:
	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b

2:
	adrp	x1, __stack_top
	add	x1, x1, :lo12:__stack_top
	mov	sp, x1

	bl	chainloader_main

3:
	wfi
	b	3b

	.globl chainloader_jump
chainloader_jump:
	mov	x4, x0
	mov	x0, x1
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x4
