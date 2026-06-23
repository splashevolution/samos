/*
 * SAM OS — kernel/idt.h
 * Sprint 14: Interrupt Descriptor Table — all 32 CPU exception vectors
 *
 * Architecture: x86-64, ring 0 only, no user mode yet.
 *
 * Design:
 *   - 256-entry IDT (32 CPU exceptions + 224 unused, all pointing to a
 *     generic "unexpected IRQ" stub that just returns)
 *   - Each exception stub saves the full register frame, calls the C
 *     handler sam_exception_handler(), then restores and iretq.
 *   - Exceptions WITH an error code pushed by the CPU: 8,10-14,17,21,29,30
 *     All other vectors push a dummy 0 so the frame layout is uniform.
 *   - sam_exception_handler() calls sam_panic() — no recovery yet.
 *     Phase 3 can route IRQ 1 (keyboard) here instead.
 *
 * Usage (from main.c):
 *   idt_init();   // call once before any code that might fault
 */

#ifndef SAM_IDT_H
#define SAM_IDT_H

#include <stdint.h>
#include "panic.h"      /* sam_panic() */

/* ── IDT gate descriptor (64-bit interrupt gate) ──────────────────────── */
typedef struct {
    uint16_t offset_lo;     /* bits 0-15  of handler address */
    uint16_t selector;      /* code segment selector (GDT_CODE64 = 0x08)   */
    uint8_t  ist;           /* interrupt stack table index (0 = don't use)  */
    uint8_t  type_attr;     /* type=0xEE (interrupt gate, DPL=0, present)   */
    uint16_t offset_mid;    /* bits 16-31 of handler address */
    uint32_t offset_hi;     /* bits 32-63 of handler address */
    uint32_t zero;          /* reserved, must be 0 */
} __attribute__((packed)) idt_gate_t;

/* ── IDTR descriptor for LIDT ─────────────────────────────────────────── */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

/* ── Register frame pushed by our stubs ───────────────────────────────── */
/* Layout (stack grows down, RSP points at vector after stubs complete):
 *
 *   [CPU auto-push on exception]
 *     SS, RSP, RFLAGS, CS, RIP   ← always (even if same ring, CPU pushes all)
 *     error_code                  ← CPU pushes for vectors 8,10-14,17,21,29,30
 *                                    our stub pushes 0 for all others
 *   [our stub pushes]
 *     vector number
 *     rax, rbx, rcx, rdx, rsi, rdi, rbp
 *     r8..r15
 */
typedef struct {
    /* GP registers saved by stub (in push order, lowest address first) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* pushed by stub */
    uint64_t vector;
    uint64_t error_code;    /* CPU-pushed or stub-pushed 0 */
    /* CPU auto-push */
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) cpu_frame_t;

/* ── The IDT (file-scope, referenced by idt_init) ─────────────────────── */
static idt_gate_t idt_table[256];

/* ── Helper: install one gate ─────────────────────────────────────────── */
static inline void idt_set_gate(int vec, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;
    idt_gate_t *g = &idt_table[vec];
    g->offset_lo  = (uint16_t)(addr & 0xFFFF);
    g->selector   = 0x08;               /* GDT_CODE64 */
    g->ist        = 0;
    g->type_attr  = 0x8E;               /* present, DPL=0, 64-bit interrupt gate */
    g->offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    g->offset_hi  = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g->zero       = 0;
}

/* ── C exception handler (called from every stub) ─────────────────────── */
/*
 * Exception names — Intel SDM Vol.3 Table 6-1
 */
static const char *exc_names[32] = {
    "#DE Divide Error",          /* 0  */
    "#DB Debug",                 /* 1  */
    "NMI",                       /* 2  */
    "#BP Breakpoint",            /* 3  */
    "#OF Overflow",              /* 4  */
    "#BR Bound Range",           /* 5  */
    "#UD Invalid Opcode",        /* 6  */
    "#NM Device Not Available",  /* 7  */
    "#DF Double Fault",          /* 8  — has error code (always 0) */
    "Coprocessor Seg Overrun",   /* 9  — legacy, not used */
    "#TS Invalid TSS",           /* 10 — has error code */
    "#NP Seg Not Present",       /* 11 — has error code */
    "#SS Stack Fault",           /* 12 — has error code */
    "#GP General Protection",    /* 13 — has error code */
    "#PF Page Fault",            /* 14 — has error code (CR2 = fault addr) */
    "Reserved",                  /* 15 */
    "#MF x87 FP Exception",      /* 16 */
    "#AC Alignment Check",       /* 17 — has error code */
    "#MC Machine Check",         /* 18 */
    "#XM SIMD FP Exception",     /* 19 */
    "#VE Virt Exception",        /* 20 */
    "#CP Control Protection",    /* 21 — has error code */
    "Reserved",                  /* 22 */
    "Reserved",                  /* 23 */
    "Reserved",                  /* 24 */
    "Reserved",                  /* 25 */
    "Reserved",                  /* 26 */
    "Reserved",                  /* 27 */
    "#HV Hypervisor Injection",  /* 28 */
    "#VC VMM Communication",     /* 29 — has error code */
    "#SX Security Exception",    /* 30 — has error code */
    "Reserved",                  /* 31 */
};

void sam_exception_handler(cpu_frame_t *frame) {
    uint64_t cr2 = 0;
    if (frame->vector == 14) {
        /* Page fault: CR2 holds the faulting virtual address */
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    }

    const char *name = (frame->vector < 32) ? exc_names[frame->vector] : "Unknown";

    sam_panic(name, frame->vector, frame->error_code, frame->rip, frame->rsp, cr2);
}

/* ─────────────────────────────────────────────────────────────────────────
 * ISR stub macros
 *
 * We define 32 tiny stubs in inline asm, each placed in the .text section.
 * Two variants:
 *   ISR_NOERR(n) — CPU does NOT push error code; stub pushes 0
 *   ISR_ERR(n)   — CPU pushes error code; stub does not
 *
 * Both stubs:
 *   1. push error_code (or 0)
 *   2. push vector number
 *   3. save all GP regs
 *   4. call sam_exception_handler(rdi = &frame)
 *   5. restore GP regs
 *   6. add 16, rsp   (pop vector + error_code)
 *   7. iretq
 *
 * The stubs are referenced via extern declarations and installed into the
 * IDT by idt_init().
 * ───────────────────────────────────────────────────────────────────────── */

/* Forward-declare all 32 stub entry points */
#define DECL_ISR(n) extern void _isr##n(void);
DECL_ISR(0)  DECL_ISR(1)  DECL_ISR(2)  DECL_ISR(3)
DECL_ISR(4)  DECL_ISR(5)  DECL_ISR(6)  DECL_ISR(7)
DECL_ISR(8)  DECL_ISR(9)  DECL_ISR(10) DECL_ISR(11)
DECL_ISR(12) DECL_ISR(13) DECL_ISR(14) DECL_ISR(15)
DECL_ISR(16) DECL_ISR(17) DECL_ISR(18) DECL_ISR(19)
DECL_ISR(20) DECL_ISR(21) DECL_ISR(22) DECL_ISR(23)
DECL_ISR(24) DECL_ISR(25) DECL_ISR(26) DECL_ISR(27)
DECL_ISR(28) DECL_ISR(29) DECL_ISR(30) DECL_ISR(31)
/* Generic stub for vectors 32-255 (hardware IRQs we don't handle yet) */
extern void _isr_generic(void);

/*
 * Define all stubs via a single large inline asm block.
 * This keeps them in the C compilation unit (main.c includes this header)
 * without requiring a separate .asm file for Phase 2.
 *
 * IMPORTANT: call this macro exactly ONCE from main.c before idt_init().
 */
#define IDT_DEFINE_STUBS() \
__asm__ ( \
".text\n\t" \
/* ── Shared save/restore/call/return body ──────────────────────────── */ \
"_isr_common:\n\t" \
    "push %rax\n\t" \
    "push %rbx\n\t" \
    "push %rcx\n\t" \
    "push %rdx\n\t" \
    "push %rsi\n\t" \
    "push %rdi\n\t" \
    "push %rbp\n\t" \
    "push %r8\n\t"  \
    "push %r9\n\t"  \
    "push %r10\n\t" \
    "push %r11\n\t" \
    "push %r12\n\t" \
    "push %r13\n\t" \
    "push %r14\n\t" \
    "push %r15\n\t" \
    /* rdi = pointer to the frame (rsp points at r15 now) */ \
    "mov %rsp, %rdi\n\t" \
    /* Align stack to 16 bytes for System V ABI */ \
    "and $-16, %rsp\n\t" \
    "sub $8, %rsp\n\t"   \
    "call sam_exception_handler\n\t" \
    "add $8, %rsp\n\t"   \
    "mov %rdi, %rsp\n\t" \
    /* Restore GP regs */ \
    "pop %r15\n\t" \
    "pop %r14\n\t" \
    "pop %r13\n\t" \
    "pop %r12\n\t" \
    "pop %r11\n\t" \
    "pop %r10\n\t" \
    "pop %r9\n\t"  \
    "pop %r8\n\t"  \
    "pop %rbp\n\t" \
    "pop %rdi\n\t" \
    "pop %rsi\n\t" \
    "pop %rdx\n\t" \
    "pop %rcx\n\t" \
    "pop %rbx\n\t" \
    "pop %rax\n\t" \
    /* discard vector + error_code */ \
    "add $16, %rsp\n\t" \
    "iretq\n\t" \
/* ── Per-vector stubs ──────────────────────────────────────────────── */ \
/* Vectors WITHOUT CPU error code: push 0, push vector, jump common    */ \
"_isr0:\n\t"  "push $0\n\t" "push $0\n\t"  "jmp _isr_common\n\t" \
"_isr1:\n\t"  "push $0\n\t" "push $1\n\t"  "jmp _isr_common\n\t" \
"_isr2:\n\t"  "push $0\n\t" "push $2\n\t"  "jmp _isr_common\n\t" \
"_isr3:\n\t"  "push $0\n\t" "push $3\n\t"  "jmp _isr_common\n\t" \
"_isr4:\n\t"  "push $0\n\t" "push $4\n\t"  "jmp _isr_common\n\t" \
"_isr5:\n\t"  "push $0\n\t" "push $5\n\t"  "jmp _isr_common\n\t" \
"_isr6:\n\t"  "push $0\n\t" "push $6\n\t"  "jmp _isr_common\n\t" \
"_isr7:\n\t"  "push $0\n\t" "push $7\n\t"  "jmp _isr_common\n\t" \
/* Vector 8 (#DF): CPU pushes error code (always 0) */ \
"_isr8:\n\t"                "push $8\n\t"  "jmp _isr_common\n\t" \
"_isr9:\n\t"  "push $0\n\t" "push $9\n\t"  "jmp _isr_common\n\t" \
/* Vectors 10-14: CPU pushes error code */ \
"_isr10:\n\t"               "push $10\n\t" "jmp _isr_common\n\t" \
"_isr11:\n\t"               "push $11\n\t" "jmp _isr_common\n\t" \
"_isr12:\n\t"               "push $12\n\t" "jmp _isr_common\n\t" \
"_isr13:\n\t"               "push $13\n\t" "jmp _isr_common\n\t" \
"_isr14:\n\t"               "push $14\n\t" "jmp _isr_common\n\t" \
"_isr15:\n\t" "push $0\n\t" "push $15\n\t" "jmp _isr_common\n\t" \
"_isr16:\n\t" "push $0\n\t" "push $16\n\t" "jmp _isr_common\n\t" \
/* Vector 17 (#AC): CPU pushes error code */ \
"_isr17:\n\t"               "push $17\n\t" "jmp _isr_common\n\t" \
"_isr18:\n\t" "push $0\n\t" "push $18\n\t" "jmp _isr_common\n\t" \
"_isr19:\n\t" "push $0\n\t" "push $19\n\t" "jmp _isr_common\n\t" \
"_isr20:\n\t" "push $0\n\t" "push $20\n\t" "jmp _isr_common\n\t" \
/* Vector 21 (#CP): CPU pushes error code */ \
"_isr21:\n\t"               "push $21\n\t" "jmp _isr_common\n\t" \
"_isr22:\n\t" "push $0\n\t" "push $22\n\t" "jmp _isr_common\n\t" \
"_isr23:\n\t" "push $0\n\t" "push $23\n\t" "jmp _isr_common\n\t" \
"_isr24:\n\t" "push $0\n\t" "push $24\n\t" "jmp _isr_common\n\t" \
"_isr25:\n\t" "push $0\n\t" "push $25\n\t" "jmp _isr_common\n\t" \
"_isr26:\n\t" "push $0\n\t" "push $26\n\t" "jmp _isr_common\n\t" \
"_isr27:\n\t" "push $0\n\t" "push $27\n\t" "jmp _isr_common\n\t" \
"_isr28:\n\t" "push $0\n\t" "push $28\n\t" "jmp _isr_common\n\t" \
/* Vector 29 (#VC): CPU pushes error code */ \
"_isr29:\n\t"               "push $29\n\t" "jmp _isr_common\n\t" \
/* Vector 30 (#SX): CPU pushes error code */ \
"_isr30:\n\t"               "push $30\n\t" "jmp _isr_common\n\t" \
"_isr31:\n\t" "push $0\n\t" "push $31\n\t" "jmp _isr_common\n\t" \
/* Generic stub for hardware IRQs 32-255: just iretq */ \
"_isr_generic:\n\t" "iretq\n\t" \
);

/* ── idt_init: install all gates and load the IDT ─────────────────────── */
static void idt_init(void) {
    /* CPU exception vectors 0-31 */
    idt_set_gate(0,  _isr0);  idt_set_gate(1,  _isr1);
    idt_set_gate(2,  _isr2);  idt_set_gate(3,  _isr3);
    idt_set_gate(4,  _isr4);  idt_set_gate(5,  _isr5);
    idt_set_gate(6,  _isr6);  idt_set_gate(7,  _isr7);
    idt_set_gate(8,  _isr8);  idt_set_gate(9,  _isr9);
    idt_set_gate(10, _isr10); idt_set_gate(11, _isr11);
    idt_set_gate(12, _isr12); idt_set_gate(13, _isr13);
    idt_set_gate(14, _isr14); idt_set_gate(15, _isr15);
    idt_set_gate(16, _isr16); idt_set_gate(17, _isr17);
    idt_set_gate(18, _isr18); idt_set_gate(19, _isr19);
    idt_set_gate(20, _isr20); idt_set_gate(21, _isr21);
    idt_set_gate(22, _isr22); idt_set_gate(23, _isr23);
    idt_set_gate(24, _isr24); idt_set_gate(25, _isr25);
    idt_set_gate(26, _isr26); idt_set_gate(27, _isr27);
    idt_set_gate(28, _isr28); idt_set_gate(29, _isr29);
    idt_set_gate(30, _isr30); idt_set_gate(31, _isr31);

    /* Hardware IRQ vectors 32-255: point to generic stub (just iretq) */
    for (int i = 32; i < 256; i++)
        idt_set_gate(i, _isr_generic);

    /* Load the IDT */
    idtr_t idtr;
    idtr.limit = (uint16_t)(sizeof(idt_table) - 1);
    idtr.base  = (uint64_t)idt_table;
    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

#endif /* SAM_IDT_H */
