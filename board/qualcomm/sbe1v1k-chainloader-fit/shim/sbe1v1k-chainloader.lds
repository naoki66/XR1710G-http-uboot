ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x4a188000;

	_start = .;

	.text : ALIGN(8) {
		KEEP(*(.text.boot))
		*(.text .text.*)
	} :text

	.rodata : ALIGN(8) {
		*(.rodata .rodata.*)
	} :text

	.data : ALIGN(8) {
		*(.data .data.*)
	} :data

	.bss (NOLOAD) : ALIGN(16) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__stack_bottom = .;
		. += 0x4000;
		__stack_top = .;
		__bss_end = .;
	} :data

	_image_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
		*(.interp)
		*(.dynsym)
		*(.dynstr)
		*(.dynamic)
		*(.gnu*)
	}
}
