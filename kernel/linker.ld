/*
 * SAM OS — kernel/linker.ld
 * Links boot.o (multiboot2 header first) + main.o into kernel ELF
 * Load address: 1 MiB (0x100000) — standard for multiboot kernels
 */

ENTRY(_start)

SECTIONS {
    . = 1M;

    /* Multiboot2 header must be within first 32 KB of image */
    .multiboot2 : {
        KEEP(*(.multiboot2))
    }

    /* Code */
    .text ALIGN(4K) : {
        *(.text*)
    }

    /* Read-only data (GDT, strings) */
    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    /* Initialised data */
    .data ALIGN(4K) : {
        *(.data*)
    }

    /* Zero-initialised data (stack, page tables) */
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    }

    /* Discard debug / comment sections to keep image minimal */
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
