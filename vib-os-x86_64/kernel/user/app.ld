/* Layout of a user program. One PT_LOAD per section group, everything inside
 * the window ring3.c maps (0x0000008000000000 .. 0x0000008000100000), and no
 * dynamic linking: the kernel copies the segments in at these exact addresses. */

ENTRY(_start)

SECTIONS {
    . = 0x0000008000000000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}