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

/* Kernel stack pointer saved by _user_enter, consumed by _user_exit.
 * RBP cannot be listed as an asm clobber (GCC rejects it), so it is
 * stashed in this global instead of on the stack. */
extern uint64_t sam_kernel_rsp;
uint64_t sam_kernel_rbp;
uint64_t sam_kernel_ret;

/* ── Enter ring 3 (never returns normally; exits via _user_exit) ─────── */
/* MUST NOT be inlined: the asm relies on a real call frame (return
 * address on the stack) — see the movq 0x30(%rsp) stash below. */
static void __attribute__((noinline)) sam_user_enter(uint64_t rip, uint64_t user_rsp) {
    __asm__ volatile (
        "pushfq\n\t"
        "movq %%rbp, %1\n\t"
        "push %%rbx\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "movq %%rsp, %0\n\t"
        /* Stash the return address in a global: the kernel stack below
         * this point cannot be trusted across the ring-3 excursion. */
        "movq 0x30(%%rsp), %%rax\n\t"
        "movq %%rax, %2\n\t"
        /* Build the interrupt frame for iretq to ring 3 */
        "push %q4\n\t"          /* SS   = user data  */
        "push %3\n\t"           /* RSP  = user stack */
        "push $0x202\n\t"       /* RFLAGS: IF=1, reserved bit 1 */
        "push %q5\n\t"          /* CS   = user code  */
        "push %6\n\t"           /* RIP  = entry      */
        "iretq\n\t"
        : "=m"(sam_kernel_rsp), "=m"(sam_kernel_rbp), "=m"(sam_kernel_ret)
        : "r"(user_rsp), "r"((uint64_t)SEL_USER_DATA),
          "r"((uint64_t)SEL_USER_CODE), "r"(rip)
        : "rax", "rbx", "r12", "r13", "r14", "r15", "cc"
    );
    /* Not reached when the task exits cleanly. If it somehow falls through,
     * halt here rather than corrupt kernel state. */
    for (;;) __asm__ volatile ("hlt");
}

/* ── C-level syscall dispatcher ────────────────────────────────────────── */
static uint64_t sam_syscall_handler(cpu_frame_t *f);    /* fwd decl */

/* ── Unified interrupt entry: exceptions panic, vector 0x80 = syscall ─── */
void sam_interrupt_dispatcher(cpu_frame_t *f) {
    if (f->vector == 128)
        sam_syscall_handler(f);     /* SYS_EXIT never returns from here */
    else
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
        /* Task finished. Never returns: _user_exit restores the kernel
         * stack saved at enter time and returns into sam_user_enter's
         * caller, abandoning this interrupt frame entirely. */
        serial_puts("[OK] Sprint 16: ring-3 task exited cleanly\n");
        {
            extern uint64_t sam_kernel_rsp;
            extern uint64_t sam_kernel_ret;
            uint64_t dbg_rsp;
            __asm__ volatile ("movq %%rsp, %0" : "=r"(dbg_rsp));
            serial_puts("     [dbg] isr rsp="); serial_puthex(dbg_rsp);
            serial_puts("  saved="); serial_puthex(sam_kernel_rsp);
            serial_puts("  ret="); serial_puthex(sam_kernel_ret);
            serial_puts("\n");
        }
        _user_exit();
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
