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

/* ── Sprint 24/25H: canonical-VA ELF → task-owned physical code backing ─
 * Two-pass loader: PASS 1 validates the complete layout (all arithmetic in
 * subtraction form), PASS 2 copies. Nothing is written until every byte of
 * the image has been proven safe.
 *
 * Rejections:
 *   -1  bad magic/class/type/machine
 *   -2  no PT_LOAD segments / bad phdr table geometry
 *   -3  vaddr outside canonical user-code region (foreign binary)
 *   -4  truncated image / segment exceeds 2 MiB code backing
 *   -5  p_filesz > p_memsz            (filesz must never exceed memsz)
 *   -6  entry point outside any loaded executable segment
 *   -7  overlapping PT_LOAD segments
 * No relocations, no PIE. */
#define SAM_ELF_EXEC 1   /* PF_X */
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

    if (eh->e_phnum == 0 || eh->e_phoff == 0) return -2;
    /* Phdr table geometry, subtraction-form: phoff <= size and
     * phnum*phentsize <= size - phoff. phentsize must be sane. */
    if (eh->e_phoff > image_size) return -4;
    if (eh->e_phentsize < sizeof(sam_elf_phdr_t) ||
        eh->e_phentsize > 64) return -2;
    {
        uint64_t tbl = (uint64_t)eh->e_phnum * eh->e_phentsize;
        if (tbl > image_size - eh->e_phoff) return -4;
    }

    const uint64_t CODE_SZ = 0x200000UL;
    const sam_elf_phdr_t *loads[16];           /* flat binaries: few segs */
    int nload = 0;

    /* ── PASS 1: validate everything, write nothing ─────────────────── */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const uint8_t *phb = img + eh->e_phoff + (uint64_t)i * eh->e_phentsize;
        const sam_elf_phdr_t *ph = (const sam_elf_phdr_t *)phb;
        if (phb + sizeof(sam_elf_phdr_t) > img + image_size) return -4;
        if (ph->p_type != SAM_ELF_PT_LOAD) continue;
        if (nload >= 16) return -2;

        if (ph->p_filesz > ph->p_memsz) return -5;              /* cond 1 */

        if (ph->p_vaddr < USER_CODE_BASE) return -3;             /* cond 6 */
        if (ph->p_vaddr >= USER_CODE_BASE + CODE_SZ) return -3;
        uint64_t off = ph->p_vaddr - USER_CODE_BASE;
        if (ph->p_memsz > CODE_SZ - off) return -3;   /* subs-form: 4/5 */

        if (ph->p_offset > image_size) return -4;               /* cond 3 */
        if (ph->p_filesz > image_size - ph->p_offset) return -4;

        loads[nload++] = ph;
    }
    if (!nload) return -2;

    /* cond 10: pairwise overlap rejection (O(n²), n≤16) */
    for (int a = 0; a < nload; a++)
        for (int b = a + 1; b < nload; b++) {
            uint64_t a0 = loads[a]->p_vaddr - USER_CODE_BASE;
            uint64_t b0 = loads[b]->p_vaddr - USER_CODE_BASE;
            uint64_t ae = a0 + loads[a]->p_memsz;    /* bounded < 2 MiB */
            uint64_t be = b0 + loads[b]->p_memsz;
            if (a0 < be && b0 < ae) return -7;
        }

    /* cond 9: entry inside an actually loaded EXECUTABLE segment's file
     * bytes (filesz, not memsz — BSS has no defined contents to jump to) */
    {
        int entry_ok = 0;
        for (int a = 0; a < nload; a++) {
            const sam_elf_phdr_t *ph = loads[a];
            if (!(ph->p_flags & SAM_ELF_EXEC)) continue;
            uint64_t lo = ph->p_vaddr, hi;
            /* hi bounded: vaddr<BASE+2M and filesz<=memsz<=2MiB-off */
            hi = ph->p_vaddr + ph->p_filesz;
            if (lo <= eh->e_entry && eh->e_entry < hi) { entry_ok = 1; break; }
        }
        if (!entry_ok) return -6;
    }

    /* ── PASS 2: copy — proven safe ─────────────────────────────────── */
    for (int a = 0; a < nload; a++) {
        const sam_elf_phdr_t *ph = loads[a];
        uint64_t off = ph->p_vaddr - USER_CODE_BASE;
        uint8_t *dst = (uint8_t *)(uintptr_t)(code_pa + off);   /* PA write */
        const uint8_t *src = img + ph->p_offset;
        for (uint64_t b = 0; b < ph->p_filesz; b++) dst[b] = src[b];
        for (uint64_t b = ph->p_filesz; b < ph->p_memsz; b++) dst[b] = 0; /* BSS */
    }

    *entry_out = eh->e_entry;                    /* canonical VA */
    return 0;
}

#endif /* SAM_ELF_H */
