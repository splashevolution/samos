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
 * Ring-3 model (honest scope):
 *   - Single user task, entered from kernel via iretq with CS=0x1B SS=0x23.
 *   - User code/data live at fixed identity-mapped addresses:
 *       USER_CODE_BASE  0x19000000  (400 MiB — above domains, inside RAM)
 *       USER_STACK_TOP  0x19400000
 *   - The int 0x80 IDT gate has DPL=3 so user code may raise it.
 *   - No page-table isolation yet (same as rest of SAM OS): ring 3 blocks
 *     privileged instructions, but memory isolation waits for per-task CR3.
 *
 * The asm trampolines (_user_enter/_user_exit/_syscall80) are defined in
 * kernel/userasm.asm and linked into the kernel.
 */

#ifndef SAM_SYSCALL_H
#define SAM_SYSCALL_H

#include <stdint.h>
#include "idt.h"        /* cpu_frame_t */

#define SAM_SYS_WRITE 1
#define SAM_SYS_EXIT  2
#define SAM_SYS_READ  3

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
extern void sam_user_resume(const cpu_frame_t *frame, uint64_t cr3);

/* Kernel stack pointer saved by _user_enter, consumed by _user_exit.
 * RBP cannot be listed as an asm clobber (GCC rejects it), so it is
 * stashed in this global instead of on the stack. */
extern uint64_t sam_kernel_rsp;
uint64_t sam_kernel_rbp;
uint64_t sam_kernel_ret;
uint64_t sam_kernel_cr3;    /* Sprint 17: boot CR3, restored on resume */
uint64_t g_task_end_reason; /* TASK_END_* — set before longjmp resume  */

/* ── Sprint 20/21: preemption + resident task table ───────────────────── */
#define TASK_END_NONE     0
#define TASK_END_EXIT     1
#define TASK_END_FAULT    2
#define TASK_END_PREEMPT  3

typedef struct {
    cpu_frame_t frame;      /* full register snapshot / initial state */
    uint64_t    cr3;        /* task address space                     */
    int         started;    /* 0 = never dispatched                   */
} sam_tcb_t;

#define SAM_MAX_TASKS 2

sam_tcb_t g_tasks[SAM_MAX_TASKS];   /* resident tasks                    */
uint64_t  g_entry[SAM_MAX_TASKS];   /* entry RIPs (set before launch)    */
uint64_t  g_stktop[SAM_MAX_TASKS];  /* user stack tops                   */
int       g_ntasks      = 0;        /* how many slots are in play        */
int       g_cur         = 0;        /* currently scheduled index         */
int       g_end_idx     = 0;        /* task that exited/faulted          */
uint64_t  g_exit_code   = 0;        /* Sprint 22: exit(code) of last end */
uint64_t  g_preempt_count = 0;      /* total preemptions                 */
uint64_t  g_tick_count  = 0;        /* PIT ticks since boot              */
uint64_t  g_quantum     = 0;        /* ticks used in current slice       */
#define SAM_QUANTUM_TICKS 3         /* ~30 ms slices @ 100 Hz            */

void *g_task_jb[5];

/* Sprint 21: prepare a task slot's initial user frame (first dispatch). */
static void sam_task_mark_start(int i) {
    if (g_tasks[i].started) return;
    cpu_frame_t *fr = &g_tasks[i].frame;
    for (int z = 0; z < 21; z++) ((uint64_t *)fr)[z] = 0;
    fr->rip    = g_entry[i];
    fr->rsp    = g_stktop[i];
    fr->cs     = SEL_USER_CODE;
    fr->ss     = SEL_USER_DATA;
    fr->rflags = 0x202;
    g_tasks[i].started = 1;
}

/* ── Enter ring 3 in a fresh address space. Control returns ONLY via
 * __builtin_longjmp (exit syscall or ring-3 fault) into the setjmp site
 * in sam_run_task(), which restores the kernel CR3. ───────────────────── */
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
    /* Sprint 20/21: PIT tick — EOI, then round-robin the resident tasks */
    if (f->vector == 32) {
        outb(0x20, 0x20);           /* EOI to master PIC            */
        g_tick_count++;
        if (g_ntasks > 0 && (f->cs & 3) == 3 &&
            ++g_quantum >= SAM_QUANTUM_TICKS) {
            g_quantum = 0;
            g_preempt_count++;
            g_tasks[g_cur].frame = *f;          /* snapshot current   */

            int next = (g_cur + 1) % g_ntasks;
            sam_task_mark_start(next);
            g_cur = next;
            sam_user_resume(&g_tasks[next].frame, g_tasks[next].cr3);
            /* sam_user_resume never returns here (iretq into the task) */
        }
        return;                     /* plain tick: iretq back       */
    }

    if (f->vector == 128) {
        sam_syscall_handler(f);     /* SYS_EXIT resumes the kernel caller */
        return;
    }

    /* Sprint 17/18: a page fault raised FROM RING 3 means the task touched
     * memory it must not. With per-task CR3 this is isolation WORKING.
     * Record the reason and resume the kernel via longjmp. */
    if (f->vector == 14 && (f->cs & 3) == 3) {
        serial_puts("[OK] ring-3 access violation faulted (#PF) at ");
        serial_puthex(f->rip); serial_puts("\n");
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
        const char *buf = (const char *)f->rsi;
        uint64_t len = f->rdx;
        /* fd is f->rdi; only stdout(1) supported, others -> error */
        if (f->rdi != 1) { f->rax = (uint64_t)-1; break; }
        if (!buf)        { f->rax = (uint64_t)-1; break; }
        /* serial_puts/serial_putchar are static in main.c, which includes
         * this header AFTER defining them. serial_putchar already expands
         * '\n' to CR-LF. */
        for (uint64_t i = 0; i < len; i++)
            serial_putchar(buf[i]);
        f->rax = len;
        break;
    }

    case SAM_SYS_EXIT:
        /* Sprint 18/21+22: record the exit CODE, then longjmp to the
         * kernel await-point, which decides what runs next. */
        serial_puts("[OK] ring-3 task exited cleanly\n");
        g_exit_code = f->rdi;
        g_task_end_reason = TASK_END_EXIT;
        g_end_idx = g_cur;
        __builtin_longjmp(g_task_jb, 1);
        /* not reached */
        for (;;) __asm__ volatile ("hlt");

    case SAM_SYS_READ:
        f->rax = 0;     /* EOF — no input device wired to syscalls yet */
        break;

    default:
        f->rax = (uint64_t)-1;
        break;
    }
    return f->rax;
}

#endif /* SAM_SYSCALL_H */
