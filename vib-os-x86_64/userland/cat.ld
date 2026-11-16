/* Standalone program layout: one blob at the base of the window the kernel
 * maps, no dynamic linking, no relocations. Kept separate from the kernel's
 * user/app.ld on purpose - third-party code will bring its own scripts too. */

ENTRY(_start)

SECTIONS {
    . = 0x0000008000000000;

    .text : { *(.text._start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
