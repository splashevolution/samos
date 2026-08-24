/*
 * SAM OS — user/linker.ld
 * Sprint 19: user programs link at the fixed user-region base.
 * Everything lands inside [0x19000000, 0x19400000) — the only ring-3
 * accessible window in a SAM OS task address space.
 */

ENTRY(start)

SECTIONS {
    . = 0x19000000;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
