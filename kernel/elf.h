/*
 * SAM OS — kernel/elf.h
 * Sprint 19 / Phase 4: minimal ELF64 executable loader
 *
 * Supports exactly what SAM OS user programs need (honest scope):
 *   - ET_EXEC static binaries, x86-64, little-endian
 *   - PT_LOAD segments copied p_offset->p_vaddr, BSS zeroed
 *   - All vaddrs must land inside the user region [USER_CODE_BASE,
 *     USER_STACK_TOP) — anything else is REJECTED (isolation preserved)
 *
 * No dynamic linking, no shared libraries, no PIE (ASLR later).
 */

#ifndef SAM_ELF_H
#define SAM_ELF_H

#include <stdint.h>

#ifndef USER_CODE_BASE
#define USER_CODE_BASE 0x19000000UL
#endif
#ifndef USER_STACK_TOP
#define USER_STACK_TOP 0x19400000UL
#endif

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum,
             e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) sam_elf_ehdr_t;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed)) sam_elf_phdr_t;

#define SAM_ELF_PT_LOAD 1

/* Returns 0 on success and fills *entry_out. Negative on error:
 *  -1 bad magic/class/type   -2 no PT_LOAD segments
 *  -3 segment outside [lo,hi)        -4 truncated image */
static int elf_load_in(const void *image, uint32_t image_size,
                       uint64_t *entry_out, uint64_t lo, uint64_t hi)
{
    const uint8_t *img = (const uint8_t *)image;
    if (image_size < sizeof(sam_elf_ehdr_t)) return -1;

    const sam_elf_ehdr_t *eh = (const sam_elf_ehdr_t *)img;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return -1;
    if (eh->e_ident[4] != 2) return -1;         /* ELFCLASS64 */
    if (eh->e_ident[5] != 1) return -1;         /* little-endian */
    if (eh->e_type != 2) return -1;             /* ET_EXEC */
    if (eh->e_machine != 0x3E) return -1;       /* EM_X86_64 */

    if (eh->e_phnum == 0 || eh->e_phoff == 0) return -2;
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize
        > image_size) return -4;

    int loaded = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const uint8_t *phb = img + eh->e_phoff + (uint32_t)i * eh->e_phentsize;
        if (phb + sizeof(sam_elf_phdr_t) > img + image_size) return -4;
        const sam_elf_phdr_t *ph = (const sam_elf_phdr_t *)phb;
        if (ph->p_type != SAM_ELF_PT_LOAD) continue;

        /* Region check: whole memory image inside the task's window */
        if (ph->p_vaddr < lo) return -3;
        if (ph->p_vaddr + ph->p_memsz > hi) return -3;
        if (ph->p_offset + ph->p_filesz > image_size) return -4;

        uint8_t *dst = (uint8_t *)(uintptr_t)ph->p_vaddr;
        const uint8_t *src = img + ph->p_offset;
        for (uint64_t b = 0; b < ph->p_filesz; b++) dst[b] = src[b];
        for (uint64_t b = ph->p_filesz; b < ph->p_memsz; b++) dst[b] = 0; /* BSS */
        loaded++;
    }

    if (!loaded) return -2;
    *entry_out = eh->e_entry;
    return 0;
}

/* Classic single-task wrapper (default user window).
 * Unused since Sprint 23 (callers use elf_load_in with per-slot windows). */
static int __attribute__((unused))
elf_load(const void *image, uint32_t image_size, uint64_t *entry_out)
{
    return elf_load_in(image, image_size, entry_out,
                       USER_CODE_BASE, USER_STACK_TOP);
}

/* ── Sprint 24: canonical-VA ELF → task-owned physical code backing ─────
 * The image must be linked at the CANONICAL user virtual layout:
 * every PT_LOAD vaddr (and the entry point) must land inside
 * [USER_CODE_BASE, USER_CODE_BASE + 0x200000). Bytes are copied to
 *   code_pa + (p_vaddr - USER_CODE_BASE)
 * i.e. the kernel writes through its identity map into the task's own
 * physical frame while ELF virtual semantics stay canonical. BSS is
 * zeroed in physical backing. No relocation, no PIE.
 *
 * Returns 0 on success. Negative on error:
 *  -1 bad magic/class/type/machine   -2 no PT_LOAD segments
 *  -3 vaddr outside canonical code region (foreign binary — REJECT)
 *  -4 truncated image / segment overflows 2 MiB code region        */
static int elf_load_pa(const void *image, uint32_t image_size,
                       uint64_t *entry_out, uint64_t code_pa)
{
    const uint8_t *img = (const uint8_t *)image;
    if (image_size < sizeof(sam_elf_ehdr_t)) return -1;

    const sam_elf_ehdr_t *eh = (const sam_elf_ehdr_t *)img;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return -1;
    if (eh->e_ident[4] != 2) return -1;         /* ELFCLASS64 */
    if (eh->e_ident[5] != 1) return -1;         /* little-endian */
    if (eh->e_type != 2) return -1;             /* ET_EXEC */
    if (eh->e_machine != 0x3E) return -1;       /* EM_X86_64 */

    /* Entry must be a canonical user-code VA */
    if (eh->e_entry < USER_CODE_BASE ||
        eh->e_entry >= USER_CODE_BASE + 0x200000UL) return -3;

    if (eh->e_phnum == 0 || eh->e_phoff == 0) return -2;
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize
        > image_size) return -4;

    int loaded = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const uint8_t *phb = img + eh->e_phoff + (uint32_t)i * eh->e_phentsize;
        if (phb + sizeof(sam_elf_phdr_t) > img + image_size) return -4;
        const sam_elf_phdr_t *ph = (const sam_elf_phdr_t *)phb;
        if (ph->p_type != SAM_ELF_PT_LOAD) continue;

        /* Canonical-region check: whole memory image inside the 2 MiB
         * user code VA region. Anything else is a foreign binary. */
        if (ph->p_vaddr < USER_CODE_BASE) return -3;
        if (ph->p_vaddr >= USER_CODE_BASE + 0x200000UL) return -3;
        uint64_t off = ph->p_vaddr - USER_CODE_BASE;
        if (off + ph->p_memsz > 0x200000UL) return -3;
        if (ph->p_offset + ph->p_filesz > image_size) return -4;

        uint8_t *dst = (uint8_t *)(uintptr_t)(code_pa + off);   /* PA write */
        const uint8_t *src = img + ph->p_offset;
        for (uint64_t b = 0; b < ph->p_filesz; b++) dst[b] = src[b];
        for (uint64_t b = ph->p_filesz; b < ph->p_memsz; b++) dst[b] = 0; /* BSS */
        loaded++;
    }

    if (!loaded) return -2;
    *entry_out = eh->e_entry;                    /* canonical VA */
    return 0;
}

#endif /* SAM_ELF_H */
