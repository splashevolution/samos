/*
 * SAM OS — kernel/vmm.h
 * Sprint 17 / Phase 4: per-task address spaces — real ring-3 isolation
 *
 * Design (honest scope):
 *   - A bump allocator carves page-table pages from a reserved physical
 *     window (0x18400000, between the GENERAL domain end and user space).
 *   - vmm_create_user_as() builds a fresh PML4 hierarchy:
 *       * user region: two 2 MiB pages at 0x19000000 (code) and
 *         0x19200000 (stack), U/S=1, RW
 *       * EVERYTHING else: supervisor-only (U/S=0), identity-mapped,
 *         cloned permissions from the boot tables minus the user bits.
 *   - Ring 3 can therefore only touch its own 4 MiB. Kernel text, the
 *     framebuffer mapping, domains and modules are all invisible to it.
 *   - No paging on demand, no swapping, no kernel higher-half: the kernel
 *     keeps its boot identity map; each task gets one CR3, destroyed on exit.
 */

#ifndef SAM_VMM_H
#define SAM_VMM_H

#include <stdint.h>

/* Reserved physical window for page-table pages (free zone: 0x18200000-
 * 0x19000000 lies above all domains, below user space). */
#define VMM_POOL_BASE 0x18400000UL
#define VMM_POOL_SIZE 0x00400000UL

/* User-space layout must match syscall.h.
 * Sprint 24: CANONICAL user virtual layout — identical in every task CR3.
 * Virtual addresses diverge from physical backing; each task's PD maps
 * these same VAs to its own slot-owned physical frames. */
#ifndef USER_CODE_BASE
#define USER_CODE_BASE 0x19000000UL   /* USER_CODE_VA  (2 MiB region) */
#endif
#ifndef USER_STACK_BASE
#define USER_STACK_BASE 0x19200000UL  /* USER_STACK_VA (2 MiB region) */
#endif
#ifndef USER_STACK_TOP
#define USER_STACK_TOP 0x19400000UL   /* exclusive top of user stack VA */
#endif

/* Boot page tables (defined in boot.asm, exported since Sprint 17) */
extern uint64_t pml4_table[512];
extern uint64_t pdpt_table[512];
extern uint64_t pd_table0[512];

static uint64_t vmm_bump = VMM_POOL_BASE;

/* Sprint 25H: lifetime telemetry — finite pool, documented exhaustion. */
static uint64_t sam_vmm_remaining(void) {
    return (VMM_POOL_BASE + VMM_POOL_SIZE) - vmm_bump;
}

/* ── Allocate one zeroed 4 KiB page-table page ────────────────────────── */
static void *vmm_alloc_pt(void) {
    if (vmm_bump + 0x1000 > VMM_POOL_BASE + VMM_POOL_SIZE) return 0;
    uint64_t *p = (uint64_t *)vmm_bump;
    for (int i = 0; i < 512; i++) p[i] = 0;
    vmm_bump += 0x1000;
    return p;
}

/* ── Build a user address space. Returns physical addr of new PML4 ────── */
/* `base` is the task's user-region start (2 MiB aligned); its 4 MiB
 * window [base, base+4MiB) becomes ring-3 accessible, everything else
 * supervisor-only. Sprint 21: per-task bases allow two resident tasks
 * to live at different addresses side by side.                        */
static uint64_t vmm_create_user_as_at(uint64_t base) {
    uint64_t *npml4 = (uint64_t *)vmm_alloc_pt();
    uint64_t *npdpt = (uint64_t *)vmm_alloc_pt();
    uint64_t *npd   = (uint64_t *)vmm_alloc_pt();
    if (!npml4 || !npdpt || !npd) return 0;

    /* PML4[0] -> fresh PDPT. NOTE: U/S must be 1 here — user access
     * requires EVERY level to permit it; per-GiB granularity is enforced
     * one level down at the PDPT. */
    npml4[0] = ((uint64_t)(uintptr_t)npdpt) | 0x07;

    /* PDPT[0] -> fresh PD covering GiB 0, user-accessible.
     * PDPT[1..3] point at the BOOT tables (GiB 1-3) with the user bit
     * stripped: kernel-only identity mappings. */
    npdpt[0] = ((uint64_t)(uintptr_t)npd) | 0x07;
    for (int i = 1; i < 4; i++)
        npdpt[i] = (pdpt_table[i] & ~0x04ULL) | 0x03;

    /* Fresh PD for GiB 0: every 2 MiB page supervisor-only RW, except the
     * two pages forming this task's window [base, base+4MiB). */
    for (int i = 0; i < 512; i++) {
        uint64_t b = (uint64_t)i << 21;
        npd[i] = b | 0x83;                          /* present+rw+2MiB */
    }
    npd[(base >> 21) & 511]                    |= 0x04;  /* code page: U/S=1 */
    npd[((base + 0x400000UL - 1) >> 21) & 511] |= 0x04;  /* stack page: U/S=1 */

    /* Physical address of PML4 == its virtual address (identity map). */
    return (uint64_t)(uintptr_t)npml4;
}

/* Default layout wrapper (single task at the classic base).
 * Unused since Sprint 23 (callers use vmm_create_user_as_at per-slot). */
static uint64_t __attribute__((unused))
vmm_create_user_as(void) {
    return vmm_create_user_as_at(USER_CODE_BASE);
}

/* ── Sprint 24: canonical-VA → per-task-PA address space ────────────────
 * Every task sees the SAME user virtual layout:
 *   USER_CODE_BASE  (0x19000000, 2 MiB) → code_pa   U/S=1 RW
 *   USER_STACK_BASE (0x19200000, 2 MiB) → stack_pa  U/S=1 RW
 * All other GiB-0 entries stay supervisor-only identity (kernel/isolation
 * architecture unchanged). GiB 1-3 remain boot-table clones minus U/S.
 *
 * code_pa/stack_pa must be 2 MiB-aligned physical frames owned by the task.
 * Returns physical address of the new PML4 (== its VA, identity pool). */
static uint64_t vmm_create_user_as_pa(uint64_t code_pa, uint64_t stack_pa) {
    uint64_t *npml4 = (uint64_t *)vmm_alloc_pt();
    uint64_t *npdpt = (uint64_t *)vmm_alloc_pt();
    uint64_t *npd   = (uint64_t *)vmm_alloc_pt();
    if (!npml4 || !npdpt || !npd) return 0;

    npml4[0] = ((uint64_t)(uintptr_t)npdpt) | 0x07;
    npdpt[0] = ((uint64_t)(uintptr_t)npd) | 0x07;
    for (int i = 1; i < 4; i++)
        npdpt[i] = (pdpt_table[i] & ~0x04ULL) | 0x03;

    /* GiB 0: every 2 MiB page supervisor-only identity RW … */
    for (int i = 0; i < 512; i++) {
        uint64_t b = (uint64_t)i << 21;
        npd[i] = b | 0x83;
    }
    /* … except the TWO canonical user entries, which map to this task's
     * own physical backing. These are REPLACED wholesale (not OR-ed onto
     * the identity entry), so for slot≠0 the VA no longer aliases its PA:
     * exactly two U/S=1 entries exist, and they point at task-owned frames. */
    npd[(USER_CODE_BASE  >> 21) & 511] = (code_pa  & ~0x1FFFFFULL) | 0x87;
    npd[(USER_STACK_BASE >> 21) & 511] = (stack_pa & ~0x1FFFFFULL) | 0x87;

    return (uint64_t)(uintptr_t)npml4;
}

/* ── Reset the allocator (called once at boot before any task runs) ───── */
static void vmm_init(void) {
    vmm_bump = VMM_POOL_BASE;
}

#endif /* SAM_VMM_H */
