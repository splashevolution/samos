/*
 * SAM OS — kernel/syscall.h
 * Sprint 16 / Phase 4: int 0x80 syscall gate + ring-3 entry/exit
 *
 * First syscall ABI (frozen for Sprint 16):
 *   rax = syscall number
 *   rdi = arg0, rsi = arg1, rdx = arg2
 *   return value in rax
 *
 *   1 = write(fd, buf, len)  — fd must be 1 (COM1 serial), returns len
 *   2 = exit(code)           — does not return; resumes kernel context
 *   3 = read(fd, buf, len)   — returns 0 (EOF) until stdin exists
 *
 * Ring-3 model (honest scope, updated Sprint 24):
 *   - N resident ring-3 tasks (fixed-capacity table), PIT-preemptive
 *     round-robin scheduler, per-task CR3 with hardware U/S isolation.
 *   - CANONICAL user virtual layout — identical in every task:
 *       USER_CODE_BASE  0x19000000  (2 MiB code/data region)
 *       USER_STACK_BASE 0x19200000  (2 MiB stack region)
 *       USER_STACK_TOP  0x19400000
 *     each task's page tables map these VAs to its own slot-owned
 *     physical backing; all other GiB-0 mappings stay supervisor-only.
 *   - The int 0x80 IDT gate has DPL=3 so user code may raise it.
 *
 * The asm trampolines (_user_enter/_user_exit/_syscall80) are defined in
 * kernel/userasm.asm and linked into the kernel.
 */

#ifndef SAM_SYSCALL_H
#define SAM_SYSCALL_H

#include <stdint.h>
#include "idt.h"        /* cpu_frame_t */

#define SAM_SYS_WRITE   1
#define SAM_SYS_EXIT    2
#define SAM_SYS_READ    3
#define SAM_SYS_WAITPID 4   /* Sprint 25 */
#define SAM_SYS_GETPID  5   /* Sprint 25 */
#define SAM_SYS_GETPPID 6   /* Sprint 25 */

/* Fixed user-space layout (identity-mapped by boot.asm page tables).
 * 400 MiB: above all three domains (< 386 MiB), below the -m 512M test RAM. */
#define USER_CODE_BASE  0x19000000UL
#define USER_STACK_TOP  0x19400000UL

/* GDT selectors for ring 3 (see boot.asm gdt64 — entries 3 and 4 added there) */
#define SEL_USER_CODE 0x1B      /* index 3 | RPL 3 */
#define SEL_USER_DATA 0x23      /* index 4 | RPL 3 */

/* Defined in userasm.asm */
extern void _syscall80(void);
extern void _user_exit(void);
extern void _user_halt(void);
extern void sam_user_resume(const cpu_frame_t *frame, uint64_t cr3,
                            void *fpu_ctx);

/* Kernel stack pointer saved by _user_enter, consumed by _user_exit.
 * RBP cannot be listed as an asm clobber (GCC rejects it), so it is
 * stashed in this global instead of on the stack. */
extern uint64_t sam_kernel_rsp;
uint64_t sam_kernel_rbp;
uint64_t sam_kernel_ret;
uint64_t sam_kernel_cr3;    /* Sprint 17: boot CR3, restored on resume */
uint64_t g_task_end_reason; /* TASK_END_* — set before longjmp resume  */

/* ── Sprint 20/21/23: preemption + resident task table ───────────────────── */
#define TASK_END_NONE     0
#define TASK_END_EXIT     1
#define TASK_END_FAULT    2
#define TASK_END_PREEMPT  3

typedef enum {
    TASK_FREE     = 0,   /* slot available */
    TASK_READY    = 1,   /* loaded, never dispatched (started=0) */
    TASK_RUNNING  = 2,   /* currently executing or preempted with saved frame */
    TASK_ZOMBIE   = 3,   /* exited/faulted, status valid, awaiting reap/drain */
    TASK_WAITING  = 4,   /* Sprint 25: blocked in waitpid, continuation saved */
} task_state_t;

/* ── Sprint 25: process identity + termination taxonomy ──────────────── */
typedef uint32_t sam_pid_t;
#define SAM_PID_KERNEL 0            /* reserved parent id; never a task    */
#define SAM_WAIT_ANY   0xFFFFFFFFu  /* wait_target sentinel for pid == -1  */

#define TERM_EXIT   0               /* termination reason codes            */
#define TERM_FAULT  1

/* SAM-native wait status: bits 31:0 code, bits 63:32 reason */
#define SAM_WSTATUS(reason, code) \
    ((((uint64_t)(uint32_t)(reason)) << 32) | ((uint64_t)(code) & 0xFFFFFFFFULL))

/* SAM-native negative waitpid error codes (no Linux errno import) */
#define WPID_E_BADPID     (-1)      /* malformed pid (0, < -1)             */
#define WPID_E_NOTCHILD   (-2)      /* target exists but is not our child  */
#define WPID_E_NOCHILDREN (-3)      /* wait-any with zero children         */
#define WPID_E_BADPTR     (-4)      /* status_va non-null and invalid      */
#define WPID_E_BADOPTS    (-5)      /* options != 0                        */
#define WPID_E_DEADLOCK   (-6)      /* blocking wait, no READY successor   */

typedef struct {
    cpu_frame_t  frame;      /* canonical virtual execution state        */
    uint64_t     cr3;        /* task address space (PML4 phys)           */
    int          started;    /* 0 = never dispatched                     */
    task_state_t state;      /* explicit state machine                   */
    uint64_t     exit_code;  /* valid when state == TASK_ZOMBIE          */
    uint64_t     init_rsp;   /* canonical user-VA RSP holding argc/argv  */
    uint64_t     code_pa;    /* Sprint 24: physical code backing owner   */
    uint64_t     stack_pa;   /* Sprint 24: physical stack backing owner  */
    char         name[32];   /* executable name for debug                */
    /* ── Sprint 25: lifecycle metadata (kernel-internal, not a user ABI) ── */
    sam_pid_t    pid;           /* process identity                       */
    sam_pid_t    ppid;          /* SAM_PID_KERNEL for shell/boot-created  */
    uint32_t     term_reason;   /* TERM_EXIT / TERM_FAULT                 */
    uint32_t     wait_target;   /* child pid or SAM_WAIT_ANY (WAITING)    */
    uint64_t     wait_status_va;/* validated user VA or 0 (WAITING)       */
} sam_tcb_t;

#define SAM_MAX_TASKS 16

sam_tcb_t g_tasks[SAM_MAX_TASKS];
int       g_ntasks      = 0;        /* count of non-FREE slots            */
int       g_cur         = -1;       /* currently scheduled index (-1=none) */
int       g_end_idx     = -1;       /* task that exited/faulted           */
uint64_t  g_exit_code   = 0;        /* Sprint 22: exit(code) of last end  */
uint64_t  g_exit_pid    = 0;        /* Sprint 25H: WHO exited last (verdict binding) */
uint64_t  g_preempt_count = 0;      /* total preemptions                  */
volatile uint64_t  g_tick_count  = 0;        /* PIT ticks since boot
                                              * (async-modified by ISR — must be volatile or
                                              *  straight-line C readers may cache stale values) */
uint64_t  g_quantum     = 0;        /* ticks used in current slice        */
#define SAM_QUANTUM_TICKS 3         /* ~30 ms slices @ 100 Hz             */

void *g_task_jb[5];
uint64_t g_last_batch_faults = 0;  /* Sprint 24: #PF-ended tasks in last drain */
sam_pid_t g_next_pid = 1;          /* Sprint 25: monotonic PID counter        */
int g_max_pid_seen = 0;            /* Sprint 25: slot-reuse/new-PID evidence  */
int g_acct_broken = 0;             /* Sprint 25H: set if accounting ever drifts */
int g_inject_audit_fail_once = 0;  /* Sprint 25H: deterministic create-failure hook */
int g_pit_live_fail = 0;           /* Sprint 25H: post-drain PIT liveness check */
uint64_t g_reap_count = 0;         /* Sprint 25H: total reaps (order-free verdicts) */

/* ── Sprint 25H: FPU/SIMD extended-context storage ──────────────────────
 * Per-slot fixed region in the free gap below user space (64 B-aligned,
 * 4 KiB per slot — comfortably above max XSAVE size on target CPUs).
 * Policy: kernel code does NOT use SIMD while any task is resident, so
 * the only live extended state belongs to the current task; it is saved
 * on every switch and restored for every dispatch. Virgin tasks get an
 * initialized legacy state (FCW/MXCSR) instead of stale registers. */
#define SAM_EXT_BASE 0x18900000UL
#define SAM_EXT_SLOT 4096
int g_fpu_ok   = 0;                /* backing validated at boot */
int g_use_xsave = 0;               /* XSAVE/XRSTOR when OSXSAVE present */

static inline void *sam_ext_ctx(int slot) {
    return (void *)(uintptr_t)(SAM_EXT_BASE + (uint64_t)slot * SAM_EXT_SLOT);
}
static void __attribute__((unused)) sam_fpu_save_dbg_unused(int slot) {
    void *c = sam_ext_ctx(slot);
    if (g_use_xsave)
        __asm__ volatile ("xsave %0" :: "m"(*(char *)c), "d"(0xFFFFFFFFu), "a"(0xFFFFFFFFu));
    else
        __asm__ volatile ("fxsave %0" :: "m"(*(char *)c));
}
static void __attribute__((unused)) sam_fpu_restore(int slot) {
    if (!g_fpu_ok) return;
    void *c = sam_ext_ctx(slot);
#ifdef SAM_25H_FPU_TRACE
    {
        unsigned long long *q = (unsigned long long *)c;
        serial_puts("     [fpu] restore s"); serial_putdec(slot);
        serial_puts(" q0="); serial_puthex(q[20]);   /* XMM0 low  @320 */
        serial_puts(" q1="); serial_puthex(q[21]);   /* XMM0 high @328 */
        serial_puts("\n");
    }
#endif
    if (g_use_xsave)
        __asm__ volatile ("xrstor %0" :: "m"(*(char *)c), "d"(0xFFFFFFFFu), "a"(0xFFFFFFFFu));
    else
        __asm__ volatile ("fxrstor %0" :: "m"(*(char *)c));
}
static void sam_fpu_virgin(int slot) {
    if (!g_fpu_ok) return;
    char *c = (char *)sam_ext_ctx(slot);
    for (int i = 0; i < SAM_EXT_SLOT; i++) c[i] = 0;
    *(uint16_t *)(c + 0)  = 0x037F;      /* FCW: all masks set            */
    *(uint32_t *)(c + 24) = 0x1F80;      /* MXCSR @24: mask all, rnd-near */
    if (g_use_xsave)
        __asm__ volatile ("xrstor %0" :: "m"(*(char *)c), "d"(0), "a"(0));
    else {
        __asm__ volatile ("fxrstor %0" :: "m"(*(char *)c));
        __asm__ volatile ("ldmxcsr %0" :: "m"(*(const uint32_t *)(c + 24)));
    }
}

/* Sprint 25H error codes shared by syscalls taking user memory */
#define SYS_E_BADFD   (-7)
#define SYS_E_TOOBIG  (-8)
#define SAM_WRITE_MAX 0x10000UL   /* 64 KiB per write() — finite by contract */

/* ── Sprint 25H: exact task accounting ─────────────────────────────────
 * THE single write path for TCB state. Keeps the frozen invariant
 *   g_ntasks == number of slots whose state != TASK_FREE
 * true through every transition (create publish, preemption, blocking,
 * termination, wake-reap, immediate reap, drain). Lifecycle code must
 * never assign .state directly again. */
static void sam_task_set_state(int i, task_state_t s) {
    task_state_t o = g_tasks[i].state;
    if (o == s) return;
    g_tasks[i].state = s;
    if (o == TASK_FREE && s != TASK_FREE) g_ntasks++;
    if (o != TASK_FREE && s == TASK_FREE) g_ntasks--;
}

/* Continuous consistency check — cheap (16 slots); called at every reap
 * and at drain. Any drift is loud and sticky for CI verdicts. */
static void sam_ntasks_sane(const char *at) {
    int n = 0;
    for (int i = 0; i < SAM_MAX_TASKS; i++)
        if (g_tasks[i].state != TASK_FREE) n++;
    if (n != g_ntasks) {
        serial_puts("     [FAIL] 25H accounting drift at "); serial_puts(at);
        serial_puts(" recorded="); serial_putdec((uint64_t)g_ntasks);
        serial_puts(" actual=");   serial_putdec((uint64_t)n);
        serial_puts("\n");
        g_acct_broken = 1;
        g_ntasks = n;             /* resynchronize; defect already reported */
    }
}

/* Sprint 25H: orchestrator interrupt-state invariant.
 * Interrupt gates clear IF on entry; __builtin_longjmp restores callee
 * registers and PC but NOT RFLAGS, so a longjmp out of dispatch would
 * otherwise leave the kernel orchestrator running with IF=0 forever —
 * starving PIT ticks and hanging any HLT-based device wait (e.g.
 * ps2_wait_char in the wizard path). The await point is THE boundary
 * where orchestration resumes: restore IF there, exactly once. */
static inline uint64_t sam_read_rflags(void) {
    uint64_t f;
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(f));
    return f;
}
static inline void sam_orchestrator_irq_restore(void) {
    __asm__ volatile ("sti");
}

/* Sprint 21/23: prepare a task slot's initial user frame (first dispatch).
 * The frame RSP is set to the task's init_rsp (adjusted for argv), NOT the
 * raw stack top. The entry RIP is read BEFORE the frame is zeroed — the
 * creator stores it in frame.rip. */
static void sam_task_mark_start(int i) {
    if (g_tasks[i].started) return;
    uint64_t entry = g_tasks[i].frame.rip;   /* save BEFORE zeroing */
    cpu_frame_t *fr = &g_tasks[i].frame;
    for (int z = 0; z < 21; z++) ((uint64_t *)fr)[z] = 0;
    fr->rip    = entry;
    fr->rsp    = g_tasks[i].init_rsp;   /* adjusted RSP with argv — authoritative */
    fr->cs     = SEL_USER_CODE;
    fr->ss     = SEL_USER_DATA;
    fr->rflags = 0x202;
    g_tasks[i].started = 1;
}

/* ── Scheduler tick: called from interrupt dispatcher on PIT IRQ (vector 32) ── */
static void sam_scheduler_tick(cpu_frame_t *f) {
    outb(0x20, 0x20);           /* EOI to master PIC */
    g_tick_count++;

    if (g_ntasks == 0) return;
    if ((f->cs & 3) != 3) return;  /* not in ring 3 */

    if (++g_quantum >= SAM_QUANTUM_TICKS) {
        g_quantum = 0;
        g_preempt_count++;

        /* Save current task frame */
        g_tasks[g_cur].frame = *f;
        sam_task_set_state(g_cur, TASK_READY);   /* preempted task becomes READY */

        /* Find next runnable task (skip ZOMBIE/FREE) */
        int next = -1;
        for (int i = 1; i <= SAM_MAX_TASKS; i++) {
            int idx = (g_cur + i) % SAM_MAX_TASKS;
            if (g_tasks[idx].state == TASK_READY || g_tasks[idx].state == TASK_RUNNING) {
                next = idx; break;
            }
        }

        if (next != -1 && next != g_cur) {
            /* Never-dispatched task? Synthesize its initial frame
             * (rip/init_rsp/cs/ss/rflags) exactly as the old inline tick did.
             * Without this, resuming a zeroed frame faults (#GP). */
            int virgin = !g_tasks[next].started;
            if (virgin)
                sam_task_mark_start(next);
#ifdef SAM_25H_FPU_TRACE
            /* dump what the trap stub captured for the OUTGOING task */
            {
                unsigned long long *q=(unsigned long long *)sam_ext_ctx(g_cur);
                serial_puts("     [fpu] saved   s"); serial_putdec(g_cur);
                serial_puts(" q0="); serial_puthex(q[20]);
                serial_puts("\n");
            }
#endif
            /* extended state already saved by the trap stub (25H) */
            g_cur = next;
            sam_task_set_state(next, TASK_RUNNING);
            serial_puts("     [tick] preempted, resuming task ");
            serial_putdec((uint64_t)next);
            serial_puts("\n");
            /* Sprint 25H: extended state loads INSIDE resume asm,
             * immediately adjacent to iretq — no window remains. */
            if (virgin) sam_fpu_virgin(next);
            sam_user_resume(&g_tasks[next].frame, g_tasks[next].cr3,
                            sam_ext_ctx(next));
            /* sam_user_resume never returns here (iretq into the task) */
        } else {
            /* next == g_cur (single runnable) or next == -1:
             * restore current to RUNNING — never leave g_cur READY. */
            sam_task_set_state(g_cur, TASK_RUNNING);
        }
    }
}

/* ============================================================================
 * Sprint 25: process identity, user-pointer translation, and lifecycle.
 * ========================================================================== */

/* ── Deterministic per-slot physical backing geometry (Sprint 24) ──────
 * slot k: code_pa = TASK_ARENA_BASE + k·TASK_PHYS_STRIDE
 *         stack_pa = code_pa + TASK_CODE_SIZE
 * A freed slot reuses its SAME physical backing — the arena does not leak
 * across task batches. (VMM page-table bump allocation remains separate
 * and still leaks ~12 KiB per newly created CR3; reclamation deferred.) */
#define TASK_ARENA_BASE   0x19000000UL
#define TASK_PHYS_STRIDE  0x400000UL
#define TASK_CODE_SIZE    0x200000UL

/* Runtime capacity: min(SAM_MAX_TASKS, consecutive usable E820 slots at
 * TASK_ARENA_BASE), discovered once in kernel_main. Legacy boards with a
 * memory hole above the arena can still run as many tasks as fit. */
int g_task_slot_capacity = SAM_MAX_TASKS;

/* Monotonic PID allocation. PID 0 is reserved for the kernel and is never
 * returned; uint32 wraparound is handled by skipping 0 and scanning all
 * non-FREE TCBs for collisions before assignment. A reused slot therefore
 * always receives a fresh PID. */
static sam_pid_t sam_pid_alloc(void) {
    for (;;) {
        sam_pid_t cand = g_next_pid++;
        if (cand == SAM_PID_KERNEL) continue;              /* wrap guard */
        int collide = 0;
        for (int i = 0; i < SAM_MAX_TASKS; i++) {
            if (g_tasks[i].state != TASK_FREE && g_tasks[i].pid == cand) {
                collide = 1; break;
            }
        }
        if (!collide) {
            if ((int)cand > g_max_pid_seen) g_max_pid_seen = (int)cand;
            return cand;
        }
    }
}

/* Translate a canonical user VA through a task's DECLARED physical backing
 * (Sprint 24 ownership model). Returns 0 for any address outside the two
 * canonical regions — callers must range-validate BEFORE calling. This is
 * the single path used for both immediate and deferred status writes: no
 * cross-address-space raw dereference ever occurs. */
static uint64_t sam_uva_to_pa(const sam_tcb_t *t, uint64_t va) {
    if (va >= USER_CODE_BASE && va < USER_CODE_BASE + TASK_CODE_SIZE)
        return t->code_pa + (va - USER_CODE_BASE);
    if (va >= USER_STACK_BASE && va < USER_STACK_TOP)
        return t->stack_pa + (va - USER_STACK_BASE);
    return 0;
}

/* Validate that [va, va+len) lies entirely inside one canonical region.
 * va == 0 is "absent pointer" and validated by callers' semantics. */
static int sam_uva_range_ok(uint64_t va, uint64_t len) {
    if (va == 0 || len == 0) return 0;
    uint64_t off = va - USER_CODE_BASE;
    if (va >= USER_CODE_BASE && off <= TASK_CODE_SIZE && len <= TASK_CODE_SIZE - off)
        return 1;
    off = va - USER_STACK_BASE;
    if (va >= USER_STACK_BASE && off <= (USER_STACK_TOP - USER_STACK_BASE)
        && len <= (USER_STACK_TOP - USER_STACK_BASE) - off)
        return 1;
    return 0;
}

/* Lowest-index READY slot starting the scan after `exclude`. Used by the
 * blocking-wait switch; the tick path keeps its own inline scan. */
static int sam_pick_ready(int exclude) {
    for (int i = 1; i <= SAM_MAX_TASKS; i++) {
        int idx = (exclude + i) % SAM_MAX_TASKS;
        if (g_tasks[idx].state == TASK_READY) return idx;
    }
    return -1;
}

/* ── Unified termination bookkeeping (exit AND fatal ring-3 fault) ──────
 * MUST run in dispatch context (IF=0 via interrupt gate): atomic vs PIT.
 *   1. victim RUNNING → ZOMBIE, reason/code recorded
 *   2. orphan rewrite: children of victim re-parented to SAM_PID_KERNEL
 *   3. wake ONE matching WAITING parent (lowest slot index wins):
 *        status encoded + written through validated VA→PA ownership,
 *        parent's saved RAX preset to child pid, WAITING → READY,
 *        child ZOMBIE → FREE only after transfer completes (reap-at-wake)
 *      no waiter: child remains ZOMBIE for immediate-reap / drain sweep.
 * Mirrors exit code into g_exit_code so shell/Sprint-22 semantics hold. */
static void sam_task_terminate(int vidx, uint32_t reason, uint64_t code) {
    sam_tcb_t *v = &g_tasks[vidx];
    sam_task_set_state(vidx, TASK_ZOMBIE);
    g_exit_pid  = v->pid;     /* Sprint 25H: verdicts bind to PID, not order */
    v->term_reason = reason;
    v->exit_code   = code;
    g_exit_code    = code;

    /* Orphan policy: children of a dead parent answer to the kernel. */
    for (int i = 0; i < SAM_MAX_TASKS; i++) {
        if (i != vidx && g_tasks[i].state != TASK_FREE &&
            g_tasks[i].ppid == v->pid)
            g_tasks[i].ppid = SAM_PID_KERNEL;
    }

    /* Wake-and-reap: one waiter may reap one child. */
    uint64_t status = SAM_WSTATUS(reason, code);
    for (int w = 0; w < SAM_MAX_TASKS; w++) {
        sam_tcb_t *p = &g_tasks[w];
        if (p->state != TASK_WAITING) continue;
        if (!(p->wait_target == v->pid || p->wait_target == SAM_WAIT_ANY))
            continue;

        serial_puts("     [wake] task ");
        serial_putdec((uint64_t)w);
        serial_puts(" READY <- child ");
        serial_putdec((uint64_t)v->pid);
        serial_puts(reason == TERM_FAULT ? " (fault)\n" : " (exit)\n");

        if (p->wait_status_va) {
            uint64_t pa = sam_uva_to_pa(p, p->wait_status_va);
            if (pa) *(volatile uint64_t *)(uintptr_t)pa = status;
        }
        p->frame.rax = (uint64_t)v->pid;
        sam_task_set_state(w, TASK_READY);
        sam_task_set_state(vidx, TASK_FREE);          /* reap only AFTER status/rax */
        g_reap_count++; sam_ntasks_sane("wake-reap");
        break;
    }
}

/* ── Enter ring 3 in a fresh address space. Control returns ONLY via
 * __builtin_longjmp (exit syscall or ring-3 fault) into the await-point
 * setjmp in sam_task_run_loop(), which restores the kernel CR3. ───────── */
static void __attribute__((noinline)) sam_user_enter(uint64_t rip, uint64_t user_rsp,
                                                     uint64_t user_cr3) {
    __asm__ volatile (
        "movq %%cr3, %%rax\n\t"     /* save kernel CR3               */
        "movq %%rax, %0\n\t"
        "movq %1, %%rax\n\t"
        "movq %%rax, %%cr3\n\t"     /* switch to task address space  */
        "pushfq\n\t"
        "push %%rbx\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "push %q2\n\t"          /* SS   = user data  */
        "push %3\n\t"           /* RSP  = user stack */
        "push $0x202\n\t"       /* RFLAGS: IF=1, reserved bit 1 */
        "push %q4\n\t"          /* CS   = user code  */
        "push %5\n\t"           /* RIP  = entry      */
        "iretq\n\t"
        :
        : "m"(sam_kernel_cr3), "r"(user_cr3), "r"((uint64_t)SEL_USER_DATA),
          "r"(user_rsp), "r"((uint64_t)SEL_USER_CODE), "r"(rip)
        : "rax", "rbx", "r12", "r13", "r14", "r15", "cc", "memory"
    );
    __builtin_unreachable();    /* control resumes via longjmp */
}

/* ── C-level syscall dispatcher ────────────────────────────────────────── */
static uint64_t sam_syscall_handler(cpu_frame_t *f);    /* fwd decl */

/* ── C-level syscall dispatcher ────────────────────────────────────────── */

/* ── Unified interrupt entry: exceptions panic, vector 0x80 = syscall ─── */
void sam_interrupt_dispatcher(cpu_frame_t *f) {
    /* Sprint 20/21/23: PIT tick — delegate to scheduler */
    if (f->vector == 32) {
        sam_scheduler_tick(f);
        return;
    }

    if (f->vector == 128) {
        sam_syscall_handler(f);     /* SYS_EXIT resumes the kernel caller */
        return;
    }

    /* Sprint 25H: CPL3 exception containment. Synchronous faults raised by
     * ordinary ring-3 execution are PROCESS-LOCAL failures, not kernel
     * corruption: terminate only the offending task via the Sprint 25
     * model (ZOMBIE, TERM_FAULT, code=vector) and let survivors run.
     *
     * Recoverable from CPL3: #DE #DB #BP #OF #BR #UD #NM #TS #NP #SS #GP
     * #PF #MF #AC #XM (#VE-reserved slot 20/22 kept fatal until used).
     * ALWAYS fatal regardless of CPL: NMI(2), #DF(8), #MC(18) — genuine
     * system-level events; recovering them would hide real corruption.
     * Kernel-originated (CPL0) faults remain fatal — unchanged policy. */
    if ((f->cs & 3) == 3 && f->vector < 32 &&
        (f->vector != 2 && f->vector != 8 && f->vector != 18)) {
        if (f->vector == 14) {   /* keep CR2 diagnostics on the common case */
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            serial_puts("[OK] ring-3 access violation faulted (#PF) at ");
            serial_puthex(f->rip); serial_puts("\n");
            serial_puts("     [CR2] faulting linear address: ");
            serial_puthex(cr2); serial_puts("\n");
        } else {
            serial_puts("[OK] ring-3 fault vector ");
            serial_putdec((uint64_t)f->vector);
            serial_puts(" contained at ");
            serial_puthex(f->rip); serial_puts("\n");
        }
        sam_task_terminate(g_cur, TERM_FAULT, f->vector);
        g_task_end_reason = TASK_END_FAULT;
        g_end_idx = g_cur;
        __builtin_longjmp(g_task_jb, 1);
        /* not reached */
    }

    sam_exception_handler(f);
}

/* ── C-level syscall dispatcher ────────────────────────────────────────── */
static uint64_t sam_syscall_handler(cpu_frame_t *f) {
    switch (f->rax) {

    case SAM_SYS_WRITE: {
        /* Sprint 25H boundary rules:
         *  - fd must be 1 (COM1 serial stdout)
         *  - zero length is valid and returns 0 without touching memory
         *  - buffer must lie entirely inside the caller's canonical user
         *    regions (subtraction-form range check; supervisor/kernel
         *    addresses and region-crossing ranges are rejected)
         *  - length is capped at SAM_WRITE_MAX so a malformed call can
         *    never turn into unbounded (IF=0) kernel work or a serial
         *    dump of arbitrary kernel memory */
        const char *buf = (const char *)f->rsi;
        uint64_t len = f->rdx;
        if (f->rdi != 1)            { f->rax = (uint64_t)(int64_t)SYS_E_BADFD;  break; }
        if (len == 0)               { f->rax = 0; break; }
        /* Cheapest rejects first: a caller-controlled length beyond the
         * documented per-call maximum is refused without any address
         * arithmetic; only then is the full buffer range validated. */
        if (len > SAM_WRITE_MAX)    { f->rax = (uint64_t)(int64_t)SYS_E_TOOBIG;  break; }
        if (!buf || !sam_uva_range_ok((uint64_t)(uintptr_t)buf, len)) {
#ifdef SAM_25H_DBG
            serial_puts("     [dbg] write reject buf=");
            serial_puthex((uint64_t)(uintptr_t)buf);
            serial_puts(" len="); serial_putdec(len);
            serial_puts(" rngok="); serial_putdec((uint64_t)sam_uva_range_ok((uint64_t)(uintptr_t)buf,len));
            serial_puts("\n");
#endif
            f->rax = (uint64_t)(int64_t)WPID_E_BADPTR; break;
        }
        for (uint64_t i = 0; i < len; i++)
            serial_putchar(buf[i]);
        f->rax = len;
        break;
    }

    case SAM_SYS_EXIT:
        /* Sprint 18/21+22+25: unified termination bookkeeping (zombie,
         * orphan rewrite, wake-and-reap), then longjmp to the kernel
         * await-point, which decides what runs next. */
        serial_puts("[OK] ring-3 task exited cleanly\n");
        sam_task_terminate(g_cur, TERM_EXIT, f->rdi);
        g_task_end_reason = TASK_END_EXIT;
        g_end_idx = g_cur;
        __builtin_longjmp(g_task_jb, 1);
        /* not reached */
        for (;;) __asm__ volatile ("hlt");

    case SAM_SYS_READ:
        f->rax = 0;     /* EOF — no input device wired to syscalls yet */
        break;

    /* ── Sprint 25: process identity + blocking wait ─────────────────── */
    case SAM_SYS_GETPID:
        f->rax = (uint64_t)g_tasks[g_cur].pid;
        break;

    case SAM_SYS_GETPPID:
        f->rax = (uint64_t)g_tasks[g_cur].ppid;
        break;

    case SAM_SYS_WAITPID: {
        int64_t  spid = (int64_t)f->rdi;      /* user-signed pid          */
        uint64_t sva  = f->rsi;               /* status user VA or 0      */
        uint64_t opts = f->rdx;
        sam_tcb_t *me = &g_tasks[g_cur];

        /* 1. validate everything BEFORE any state change */
        if (opts != 0) { f->rax = (uint64_t)(int64_t)WPID_E_BADOPTS; break; }
        if (sva != 0) {
            if ((sva & 7) != 0 || !sam_uva_range_ok(sva, 8)) {
                f->rax = (uint64_t)(int64_t)WPID_E_BADPTR; break;
            }
        }
        if (spid == 0 || spid < -1) {
            f->rax = (uint64_t)(int64_t)WPID_E_BADPID; break;
        }

        /* 2. survey children (ppid metadata is the ONLY parentage source) */
        int   zomb      = -1;   /* lowest-slot reapable match              */
        int   have_any  = 0;    /* any non-FREE child at all               */
        int   found_tgt = 0;    /* specific-pid child exists?              */
        for (int i = 0; i < SAM_MAX_TASKS; i++) {
            if (i == g_cur) continue;
            sam_tcb_t *t = &g_tasks[i];
            if (t->state == TASK_FREE || t->ppid != me->pid) continue;
            have_any = 1;
            if (spid > 0 && (sam_pid_t)spid == t->pid) found_tgt = 1;
            if (t->state == TASK_ZOMBIE &&
                zomb == -1 &&
                (spid < 0 || (sam_pid_t)spid == t->pid))
                zomb = i;                     /* deterministic lowest slot */
        }
        if (spid > 0 && !found_tgt) {
            f->rax = (uint64_t)(int64_t)WPID_E_NOTCHILD; break;
        }
        if (spid == -1 && !have_any) {
            f->rax = (uint64_t)(int64_t)WPID_E_NOCHILDREN; break;
        }

        /* 3. immediate reap — no scheduler involvement */
        if (zomb != -1) {
            sam_tcb_t *v = &g_tasks[zomb];
            if (sva) {
                uint64_t pa = sam_uva_to_pa(me, sva);   /* same helper path */
                *(volatile uint64_t *)(uintptr_t)pa =
                    SAM_WSTATUS(v->term_reason, v->exit_code);
            }
            serial_puts("     [reap] task ");
            serial_putdec((uint64_t)g_cur);
            serial_puts(" reaped child ");
            serial_putdec((uint64_t)v->pid);
            serial_puts(" (slot ");
            serial_putdec((uint64_t)zomb);
            serial_puts(" freed)\n");
            f->rax = (uint64_t)v->pid;
            sam_task_set_state(zomb, TASK_FREE);
            g_reap_count++;
            sam_ntasks_sane("immediate-reap");
            break;
        }

        /* 4. blocking wait — verify a successor BEFORE leaving RUNNING */
        int nxt = sam_pick_ready(g_cur);
        if (nxt == -1) {
            /* caller stays RUNNING; nothing is corrupted */
            f->rax = (uint64_t)(int64_t)WPID_E_DEADLOCK; break;
        }
        serial_puts("     [block] task ");
        serial_putdec((uint64_t)g_cur);
        serial_puts(" WAITING (target ");
        serial_putdec(spid < 0 ? (uint64_t)(uint32_t)SAM_WAIT_ANY
                               : (uint64_t)(uint32_t)spid);
        serial_puts(")\n");
        me->frame          = *f;                  /* full user continuation */
        me->wait_target    = (spid < 0) ? SAM_WAIT_ANY : (uint32_t)spid;
        me->wait_status_va = sva;
        if (spid > 0) me->frame.rax = (uint64_t)(uint32_t)spid; /* preset  */
        sam_task_set_state(g_cur, TASK_WAITING);

        int virgin2 = !g_tasks[nxt].started;
        if (virgin2) sam_task_mark_start(nxt);
        /* extended state of the blocker saved by its own syscall trap */
        g_cur = nxt;
        sam_task_set_state(nxt, TASK_RUNNING);
        serial_puts("     [run] successor task ");
        serial_putdec((uint64_t)nxt);
        serial_puts(" dispatched from syscall context\n");
        if (virgin2) sam_fpu_virgin(nxt);
        sam_user_resume(&g_tasks[nxt].frame, g_tasks[nxt].cr3,
                        sam_ext_ctx(nxt));
        __builtin_unreachable();   /* noreturn — no fall-through to default */
        }

    default:
        f->rax = (uint64_t)-1;
        break;
    }
    return f->rax;
}

/* ============================================================================
 * Sprint 23: Generic task creation, scheduling, and run/drain loop.
 *
 * Dependencies resolved by main.c include order (all visible here):
 *   vfs.h      — vfs_file_handle_t, vfs_open/vfs_read/vfs_close (static)
 *   vmm.h      — vmm_create_user_as_at (static)
 *   elf.h      — elf_load_in (static)
 *   mcp.h      — MB2_TAG_*, GENERAL_DOMAIN_BASE is a main.c #define
 *   main.c     — outb, serial_*, sam_build_args, sam_e820_range_usable
 * ========================================================================== */

/* Forward decls for main.c statics defined above this include point. */
static uint64_t sam_build_args(uint64_t stktop_pa, uint64_t pa2va_delta,
                               char **argv, int argc);
static int sam_e820_range_usable(uint64_t base, uint64_t size);

/* ── Sprint 24 backing geometry (defines live in the Sprint 25 block above,
 *    which precedes all users; kept here for provenance only). */
static uint64_t sam_task_code_pa(int slot) {
    return TASK_ARENA_BASE + (uint64_t)slot * TASK_PHYS_STRIDE;
}
static uint64_t sam_task_stack_pa(int slot) {
    return sam_task_code_pa(slot) + TASK_CODE_SIZE;
}

/* ── Sprint 24: verify the fresh CR3 really carries U/S=1 on exactly the
 * two canonical user entries and they target THIS task's frames.
 * Walks the page-table chain through the kernel identity map. Returns 1
 * if the audit passes (result also printed to serial). */
static int sam_audit_user_pd(uint64_t cr3, uint64_t code_pa, uint64_t stack_pa) {
    uint64_t pdpt = (*(volatile uint64_t *)(uintptr_t)cr3)      & ~0xFFFULL;
    uint64_t pd   = (*(volatile uint64_t *)(uintptr_t)pdpt)     & ~0xFFFULL;
    volatile uint64_t *e_code  = (volatile uint64_t *)(uintptr_t)(pd + 8*((USER_CODE_BASE  >> 21) & 511));
    volatile uint64_t *e_stack = (volatile uint64_t *)(uintptr_t)(pd + 8*((USER_STACK_BASE >> 21) & 511));

    int ok = 1;
    ok &= ((*e_code  & 0x001) != 0);                    /* present       */
    ok &= ((*e_code  & 0x004) != 0);                    /* U/S = user    */
    ok &= ((*e_code  & ~0x1FFFFFULL) == code_pa);
    ok &= ((*e_stack & 0x001) != 0);
    ok &= ((*e_stack & 0x004) != 0);
    ok &= ((*e_stack & ~0x1FFFFFULL) == stack_pa);

    serial_puts("     [aud] pd[");
    serial_putdec((USER_CODE_BASE >> 21) & 511);
    serial_puts("]→"); serial_puthex(*e_code & ~0x1FFFFFULL);
    serial_puts(" pd[");
    serial_putdec((USER_STACK_BASE >> 21) & 511);
    serial_puts("]→"); serial_puthex(*e_stack & ~0x1FFFFFULL);
    serial_puts(ok ? "  U/S=1  [PASS]\n" : "  [FAIL] audit\n");
    return ok;
}

/* Create a task from an initrd ELF. Returns slot index or -1 on failure.
 * On failure the slot remains FREE, g_ntasks unchanged; VMM PT pages may
 * be leaked (deferred reclamation). */
static int sam_task_create(const char *name, char **argv, int argc) {
    if (g_ntasks >= g_task_slot_capacity) {
        serial_puts("     [FAIL] task table full (capacity ");
        serial_putdec((uint64_t)g_task_slot_capacity);
        serial_puts(")\n");
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < g_task_slot_capacity; i++) {
        if (g_tasks[i].state == TASK_FREE) { slot = i; break; }
    }
    if (slot == -1) return -1;

    /* Deterministic physical backing for this slot */
    uint64_t code_pa  = sam_task_code_pa(slot);
    uint64_t stack_pa = sam_task_stack_pa(slot);

    /* Defensive per-create E820 validation of the whole slot slice */
    if (!sam_e820_range_usable(code_pa, TASK_PHYS_STRIDE)) {
        serial_puts("     [FAIL] task backing @");
        serial_puthex(code_pa);
        serial_puts(" outside usable RAM (E820)\n");
        return -1;   /* slot stays FREE, nothing consumed yet */
    }

    /* ── Sprint 25H: sequential-confidentiality zeroization ────────────
     * Clear the ENTIRE task-owned backing before any loader/builder
     * touches it. A process reusing a slot must never observe a
     * predecessor's stack data, argv strings, wait statuses, or code
     * remnants. Done through the kernel identity map; also guarantees
     * failed creations leave clean backing for the next occupant. */
    {
        volatile uint64_t *z = (volatile uint64_t *)(uintptr_t)code_pa;
        uint64_t n = (TASK_PHYS_STRIDE / 8);
        for (uint64_t i = 0; i < n; i++) z[i] = 0;
    }

    /* Open and read ELF from initrd into GENERAL-domain staging scratch */
    vfs_file_handle_t h;
    if (vfs_open(name, &h) != 0) {
        serial_puts("     [FAIL] "); serial_puts(name); serial_puts(" not found in initrd\n");
        return -1;
    }
    uint8_t *stage = (uint8_t *)(uintptr_t)(GENERAL_DOMAIN_BASE + 0x00100000UL);
    int32_t got = vfs_read(&h, stage, 0x00100000u);
    vfs_close(&h);
    if (got <= 0) {
        serial_puts("     [FAIL] vfs_read returned "); serial_putdec((uint64_t)got); serial_puts("\n");
        return -1;
    }

    /* Sprint 24: load canonical-VA ELF into PHYSICAL code backing */
    uint64_t entry = 0;
    int er = elf_load_pa(stage, (uint32_t)got, &entry, code_pa);
    if (er != 0) {
        serial_puts("     [reject] elf_load_pa("); serial_puts(name);
        serial_puts(") error: "); serial_putdec((uint64_t)(-er)); serial_puts("\n");
        return -1;
    }

    /* Address space: canonical VAs → this task's PA pair */
    uint64_t user_cr3 = vmm_create_user_as_pa(code_pa, stack_pa);
    if (!user_cr3) {
        serial_puts("     [FAIL] vmm_create_user_as_pa: out of page-table pages\n");
        return -1;
    }

    /* ── argv: PHYSICAL write, VIRTUAL pointers ──────────────────────── */
    uint64_t pa2va_delta = stack_pa - USER_STACK_BASE;
    uint64_t init_rsp    = sam_build_args(stack_pa + TASK_CODE_SIZE,
                                          pa2va_delta, argv, argc);

    /* ── Sprint 25H: mapping audit is the LAST fallible check ────────── */
    int aud = sam_audit_user_pd(user_cr3, code_pa, stack_pa);
    if (g_inject_audit_fail_once) {          /* deterministic failure hook */
        g_inject_audit_fail_once = 0;
        aud = 0;
        serial_puts("     [inject] Sprint 25H audit failure injected\n");
    }
    if (!aud) {
        /* Transactional rollback: the scheduler has seen NOTHING.
         * Slot stays FREE, g_ntasks untouched, no schedulable partial
         * task exists. Backing is already zeroized+partially loaded —
         * the next create re-zeroizes before use. Page-table pages for
         * this CR3 are leaked by policy (documented). */
        serial_puts("     [reject] create rolled back: PD audit failed\n");
        return -1;
    }

    /* ── Single publish point: FREE → READY ──────────────────────────── */
    g_tasks[slot].frame.rip = entry;      /* canonical VA */
    g_tasks[slot].cr3         = user_cr3;
    g_tasks[slot].started     = 0;
    g_tasks[slot].exit_code   = 0;
    g_tasks[slot].init_rsp    = init_rsp; /* canonical VA */
    g_tasks[slot].code_pa     = code_pa;
    g_tasks[slot].stack_pa    = stack_pa;
    g_tasks[slot].pid         = sam_pid_alloc();       /* Sprint 25 */
    g_tasks[slot].ppid        = SAM_PID_KERNEL;        /* shell/boot children */
    g_tasks[slot].term_reason = TERM_EXIT;
    g_tasks[slot].wait_target = 0;
    g_tasks[slot].wait_status_va = 0;
    for (int i = 0; i < 31 && name[i]; i++) g_tasks[slot].name[i] = name[i];
    g_tasks[slot].name[31] = '\0';

    sam_task_set_state(slot, TASK_READY);   /* atomic FREE → READY publish */

    serial_puts("     [OK] Sprint 24: task ");
    serial_putdec(slot);
    serial_puts(" pid=");
    serial_putdec((uint64_t)g_tasks[slot].pid);
    serial_puts(" created ("); serial_puts(name);
    serial_puts(") va=");
    serial_puthex(USER_CODE_BASE);
    serial_puts(" code_pa=");
    serial_puthex(code_pa);
    serial_puts(" stack_pa=");
    serial_puthex(stack_pa);
    serial_puts("\n");

    return slot;   /* Sprint 25H: transactional — failure never publishes */
}

/* Run all resident tasks round-robin until all have exited/faulted.
 * Returns number of tasks that exited cleanly. */
static int sam_task_run_loop(void) {
    int clean = 0;
    g_last_batch_faults = 0;

    while (g_ntasks > 0) {
        /* Find next runnable if none currently running */
        if (g_cur == -1) {
            for (int i = 0; i < SAM_MAX_TASKS; i++) {
                if (g_tasks[i].state == TASK_READY) {
                    g_cur = i;
                    break;
                }
            }
            if (g_cur == -1) break;  /* only ZOMBIE/FREE remain */
        }

        if (__builtin_setjmp(g_task_jb) == 0) {
            if (g_tasks[g_cur].started == 0) {
                /* First dispatch: mark_start builds frame with init_rsp
                 * (argv-adjusted); saved cpu_frame_t becomes authoritative
                 * only after the first preemption snapshots over it.
                 * 25H: the previously-running task died (exit/fault), so
                 * its extended state is abandoned — virgin init only. */
                sam_task_mark_start(g_cur);
                sam_task_set_state(g_cur, TASK_RUNNING);
                serial_puts("     [dispatch] task ");
                serial_putdec((uint64_t)g_cur);
                serial_puts(" started\n");
                sam_fpu_virgin(g_cur);
                sam_user_enter(g_tasks[g_cur].frame.rip,
                               g_tasks[g_cur].frame.rsp,
                               g_tasks[g_cur].cr3);
            } else {
                /* Resume after preemption: saved frame is authoritative;
                 * 25H: extended state was saved by whoever switched away */
                sam_task_set_state(g_cur, TASK_RUNNING);
                serial_puts("     [resume] task ");
                serial_putdec((uint64_t)g_cur);
                serial_puts("\n");
                sam_user_resume(&g_tasks[g_cur].frame, g_tasks[g_cur].cr3,
                                sam_ext_ctx(g_cur));
            }
            __builtin_unreachable();
        }

        /* Longjmp landed here: exit or fault (preempt never longjmps).
         * Sprint 25H invariant: the interrupt gate cleared IF on entry and
         * __builtin_longjmp does NOT restore RFLAGS — without the explicit
         * restore below, the orchestrator (and every later HLT-based wait,
         * e.g. ps2_wait_char in the wizard path) would run with interrupts
         * disabled forever after the first user task terminated. This is
         * THE lifecycle boundary where orchestration resumes. */
        uint64_t kcr3 = sam_kernel_cr3;
        __asm__ volatile ("movq %0, %%cr3" :: "r"(kcr3));
        sam_orchestrator_irq_restore();   /* orchestrator runs with IF=1 */

        /* g_task_end_reason is EXIT or FAULT */
        if (g_task_end_reason == TASK_END_EXIT) clean++;
        if (g_task_end_reason == TASK_END_FAULT) g_last_batch_faults++;

        serial_puts("     [sched] task ");
        serial_putdec((uint64_t)g_end_idx);
        serial_puts(" ended (");
        serial_puts(g_task_end_reason == TASK_END_EXIT ? "exit" : "fault");
        serial_puts(g_tasks[g_end_idx].state == TASK_FREE
                    ? ", already reaped)\n" : ")\n");

        /* Find next runnable — WAITING/ZOMBIE/FREE are never selected */
        int next = -1;
        for (int i = 1; i <= SAM_MAX_TASKS; i++) {
            int idx = (g_end_idx + i) % SAM_MAX_TASKS;
            if (g_tasks[idx].state == TASK_READY || g_tasks[idx].state == TASK_RUNNING) {
                next = idx; break;
            }
        }

        if (next == -1) break;

        g_cur = next;
        /* loop continues, will hit setjmp==0 path and resume next */
    }

    /* Full drain cleanup — helper keeps accounting exact (incl. stranded
     * WAITING/ZOMBIE slots, which are reclaimed here by policy). */
    g_cur = -1;
    for (int i = 0; i < SAM_MAX_TASKS; i++) {
        if (g_tasks[i].state != TASK_FREE)
            sam_task_set_state(i, TASK_FREE);
    }
    sam_ntasks_sane("drain");
    return clean;
}

#endif /* SAM_SYSCALL_H */
