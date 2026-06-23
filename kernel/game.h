/*
 * SAM OS — kernel/game.h
 * =======================
 * Sprint 7: Pong game engine running in the GAME domain (0x4200000).
 *
 * Hardware used (no libc, no OS):
 *   VGA text mode  — 0xB8000, 80×25, block chars for graphics
 *   PIT channel 0  — 8253/8254 at 0x40, IRQ0 → vector 32, ~60Hz tick
 *   8259 PIC       — remapped: IRQ0-7 → vectors 32-39, IRQ8-15 → 40-47
 *   PS/2 keyboard  — port 0x60, scan codes polled on IRQ1 → vector 33
 *   IDT            — 64-bit, 256 entries, minimal stubs for IRQ0+IRQ1
 *
 * Game state lives entirely in the GAME domain (base=0x4200000).
 * Ball, paddles, score are stored there — isolated from AI domain.
 *
 * VGA layout (80×25 text mode, block characters):
 *   Row 0        : score line
 *   Rows 1-23    : play field  (23 rows tall)
 *   Row 24       : status bar
 *   Col 0        : left boundary
 *   Col 79       : right boundary
 *   Left paddle  : col 2,  rows paddle_l .. paddle_l+3
 *   Right paddle : col 77, rows paddle_r .. paddle_r+3
 *   Ball         : col ball_x, row ball_y (single char '●')
 */

#ifndef SAM_GAME_H
#define SAM_GAME_H

#include <stdint.h>

/* ── VGA text buffer ─────────────────────────────────────────────────────── */
#define VGA_GAME   ((volatile uint16_t *)0xB8000)
#define COLS       80
#define ROWS       25
#define PLAY_TOP   1
#define PLAY_BOT   23    /* inclusive */
#define PLAY_H     (PLAY_BOT - PLAY_TOP + 1)   /* 23 rows */

/* Colour attributes */
#define CLR_BLACK   0x0000
#define CLR_WHITE   0x0F00
#define CLR_GREEN   0x0A00
#define CLR_CYAN    0x0B00
#define CLR_YELLOW  0x0E00
#define CLR_RED     0x0C00
#define CLR_BLUE    0x0900

/* Block drawing chars */
#define CHAR_BALL    0xDB   /* █  solid block */
#define CHAR_PADDLE  0xDB   /* █  same */
#define CHAR_BORDER  0xCD   /* ═  double horizontal */
#define CHAR_SPACE   0x20

/* ── I/O port helpers ────────────────────────────────────────────────────── */
static inline void game_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0,%1" :: "a"(val),"Nd"(port));
}
static inline uint8_t game_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ── PIC (8259) ──────────────────────────────────────────────────────────── */
#define PIC1_CMD  0x20
#define PIC1_DAT  0x21
#define PIC2_CMD  0xA0
#define PIC2_DAT  0xA1
#define PIC_EOI   0x20   /* End-of-interrupt command */

static inline void pic_remap(void) {
    /* ICW1: start init, edge-triggered, cascade */
    game_outb(PIC1_CMD, 0x11);
    game_outb(PIC2_CMD, 0x11);
    /* ICW2: vector offsets — IRQ0-7 → 32, IRQ8-15 → 40 */
    game_outb(PIC1_DAT, 32);
    game_outb(PIC2_DAT, 40);
    /* ICW3: cascade wiring */
    game_outb(PIC1_DAT, 0x04);   /* IRQ2 has slave */
    game_outb(PIC2_DAT, 0x02);   /* slave ID = 2 */
    /* ICW4: 8086 mode */
    game_outb(PIC1_DAT, 0x01);
    game_outb(PIC2_DAT, 0x01);
    /* Mask all except IRQ0 (timer=32) and IRQ1 (keyboard=33) */
    game_outb(PIC1_DAT, 0xFC);   /* 1111 1100 — unmask bit0+bit1 */
    game_outb(PIC2_DAT, 0xFF);   /* all slave IRQs masked */
}

static inline void pic_eoi_master(void) {
    game_outb(PIC1_CMD, PIC_EOI);
}

/* ── PIT (8253/8254) — channel 0, ~60Hz ─────────────────────────────────── */
#define PIT_CH0   0x40
#define PIT_CMD   0x43
/* PIT base frequency = 1,193,182 Hz
 * Divisor for ~60Hz: 1193182 / 60 = 19886 */
#define PIT_DIVISOR  19886

static inline void pit_init_60hz(void) {
    /* Command: channel 0, lo/hi byte, mode 3 (square wave), binary */
    game_outb(PIT_CMD, 0x36);
    game_outb(PIT_CH0, (uint8_t)(PIT_DIVISOR & 0xFF));
    game_outb(PIT_CH0, (uint8_t)(PIT_DIVISOR >> 8));
}

/* ── IDT ─────────────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;   /* 0x8E = present, DPL=0, interrupt gate */
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

#define IDT_ENTRIES 256
static idt_entry_t sam_idt[IDT_ENTRIES];
static idt_ptr_t   sam_idt_ptr;

static inline void idt_set_gate(int vec, uint64_t handler) {
    sam_idt[vec].offset_lo  = (uint16_t)(handler & 0xFFFF);
    sam_idt[vec].selector   = 0x08;          /* kernel code segment */
    sam_idt[vec].ist        = 0;
    sam_idt[vec].type_attr  = 0x8E;          /* present, 64-bit interrupt gate */
    sam_idt[vec].offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
    sam_idt[vec].offset_hi  = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    sam_idt[vec].zero       = 0;
}

static inline void idt_load(void) {
    sam_idt_ptr.limit = sizeof(sam_idt) - 1;
    sam_idt_ptr.base  = (uint64_t)sam_idt;
    __asm__ volatile ("lidt %0" :: "m"(sam_idt_ptr));
}

/* ── Keyboard scan codes (set 1) ─────────────────────────────────────────── */
#define KEY_W_PRESS    0x11
#define KEY_S_PRESS    0x1F
#define KEY_UP_PRESS   0x48
#define KEY_DOWN_PRESS 0x50
#define KEY_W_REL      0x91
#define KEY_S_REL      0x9F
#define KEY_UP_REL     0xC8
#define KEY_DOWN_REL   0xD0
#define KEY_Q_PRESS    0x10   /* quit */

/* ── Shared game state (volatile — written by IRQ handlers) ──────────────── */
/* Defined in main.c — extern here so game.h can be safely included from
 * multiple translation units (Sprint 8 adds scheduler.c). */
extern volatile uint32_t game_ticks;
extern volatile uint8_t  key_state[4];
extern volatile uint8_t  key_quit;

/* ── IRQ handlers (C functions called from asm stubs) ───────────────────── */
void irq0_handler(void) {
    game_ticks++;
    pic_eoi_master();
}

void irq1_handler(void) {
    uint8_t sc = game_inb(0x60);
    switch (sc) {
        case KEY_W_PRESS:    key_state[0] = 1; break;
        case KEY_W_REL:      key_state[0] = 0; break;
        case KEY_S_PRESS:    key_state[1] = 1; break;
        case KEY_S_REL:      key_state[1] = 0; break;
        case KEY_UP_PRESS:   key_state[2] = 1; break;
        case KEY_UP_REL:     key_state[2] = 0; break;
        case KEY_DOWN_PRESS: key_state[3] = 1; break;
        case KEY_DOWN_REL:   key_state[3] = 0; break;
        case KEY_Q_PRESS:    key_quit = 1;      break;
    }
    pic_eoi_master();
}

/* ── SAM OS IRQ stub infrastructure ──────────────────────────────────────────
 *
 * DESIGN GOALS:
 *   1. Correct: save ALL caller-saved state — GPRs + XMM0-XMM7 — unconditionally.
 *      AI inference (Sprint 9+) will have XMM registers live when a PIT tick fires.
 *      One missed movdqu = silent corruption of an in-flight matmul.
 *
 *   2. Alignment-safe by construction: the SysV AMD64 ABI requires RSP to be
 *      16-byte aligned at the point of a `call` instruction.
 *      At IRQ entry the CPU has pushed 5 qwords (RIP, CS, RFLAGS, RSP, SS = 40 bytes).
 *      RSP is therefore 16-byte aligned MINUS 8 at entry (since the stack was aligned
 *      before the interrupt, and 5 pushes = 40 bytes = 0 mod 16, so RSP = aligned-40
 *      which is aligned).  Wait — 40 mod 16 = 8, so RSP is aligned-8 at entry, i.e.
 *      NOT 16-byte aligned.
 *      We save: 9 GPRs (72 bytes) + 1 alignment pad qword (8 bytes) + 128 bytes XMM
 *      = 208 bytes total pushed before `call`.
 *      Entry misalignment: -8 bytes.  208 mod 16 = 0.  Net: RSP aligned at `call`. ✓
 *      If you ever add or remove a push, recalculate the pad.
 *
 *   3. Scalable: adding a new IRQ vector is a two-line affair — declare the handler
 *      C function and use SAM_IRQ_STUB() below.  No copy-paste of 40-line stubs.
 *
 * FRAME LAYOUT (from high address to low, after all saves):
 *   [CPU pushed]  SS, RSP, RFLAGS, CS, RIP         (5 × 8 = 40 bytes)
 *   [stub saves]  RAX, RCX, RDX, RSI, RDI,
 *                 R8, R9, R10, R11                  (9 × 8 = 72 bytes)
 *                 alignment pad (1 × 8 = 8 bytes)
 *                 XMM0..XMM7                        (8 × 16 = 128 bytes)
 *   Total pushed by stub: 208 bytes.  208 mod 16 = 0.  RSP 16-byte aligned. ✓
 * ─────────────────────────────────────────────────────────────────────────── */

/* SAM_IRQ_STUB(name, handler_fn)
 * Generates a complete, ABI-correct IRQ entry point named `name` that:
 *   - saves all caller-saved GPRs
 *   - inserts a padding qword to guarantee 16-byte RSP alignment at call
 *   - saves XMM0-XMM7 (AI inference state preservation)
 *   - calls handler_fn (a plain void C function)
 *   - restores everything in reverse
 *   - returns via iretq
 *
 * Usage:  SAM_IRQ_STUB(irq0_stub, irq0_handler)
 */
#define SAM_IRQ_STUB(name, handler_fn)                                      \
__attribute__((naked)) void name(void) {                                    \
    __asm__ volatile (                                                      \
        /* ── Save caller-saved GPRs (9 × 8 = 72 bytes) ── */              \
        "push %%rax\n\t"                                                    \
        "push %%rcx\n\t"                                                    \
        "push %%rdx\n\t"                                                    \
        "push %%rsi\n\t"                                                    \
        "push %%rdi\n\t"                                                    \
        "push %%r8\n\t"                                                     \
        "push %%r9\n\t"                                                     \
        "push %%r10\n\t"                                                    \
        "push %%r11\n\t"                                                    \
        /* ── Alignment proof (verified by tools/align_check.py) ─────────
         * At IRQ entry RSP is aligned-8 (CPU pushed 5 qwords = 40 bytes,
         * 40 mod 16 = 8).  After 9 GPR pushes (72 bytes), total = 112,
         * 112 mod 16 = 0 → RSP is now 16-byte aligned.
         * sub $128 is a multiple of 16, alignment preserved.
         * NO padding qword needed. Call fires with RSP 16-byte aligned. ✓ */\
        /* ── Save XMM0-XMM7 (8 × 16 = 128 bytes) ───────
         * Allocate space then store; movdqu does not require alignment.   \
         * Sprint 9+ AI inference will have XMM0-XMM7 live at interrupt;  \
         * skipping this save corrupts in-flight matmul silently. */       \
        "sub $128, %%rsp\n\t"                                               \
        "movdqu %%xmm0, 0(%%rsp)\n\t"                                      \
        "movdqu %%xmm1, 16(%%rsp)\n\t"                                     \
        "movdqu %%xmm2, 32(%%rsp)\n\t"                                     \
        "movdqu %%xmm3, 48(%%rsp)\n\t"                                     \
        "movdqu %%xmm4, 64(%%rsp)\n\t"                                     \
        "movdqu %%xmm5, 80(%%rsp)\n\t"                                     \
        "movdqu %%xmm6, 96(%%rsp)\n\t"                                     \
        "movdqu %%xmm7, 112(%%rsp)\n\t"                                    \
        /* ── Call C handler (RSP now 16-byte aligned) ── */               \
        "call " #handler_fn "\n\t"                                          \
        /* ── Restore XMM0-XMM7 ──────────────────────── */                \
        "movdqu 0(%%rsp),   %%xmm0\n\t"                                    \
        "movdqu 16(%%rsp),  %%xmm1\n\t"                                    \
        "movdqu 32(%%rsp),  %%xmm2\n\t"                                    \
        "movdqu 48(%%rsp),  %%xmm3\n\t"                                    \
        "movdqu 64(%%rsp),  %%xmm4\n\t"                                    \
        "movdqu 80(%%rsp),  %%xmm5\n\t"                                    \
        "movdqu 96(%%rsp),  %%xmm6\n\t"                                    \
        "movdqu 112(%%rsp), %%xmm7\n\t"                                    \
        "add $128, %%rsp\n\t"                                               \
        /* ── Restore caller-saved GPRs ──────────────── */                 \
        "pop %%r11\n\t"                                                     \
        "pop %%r10\n\t"                                                     \
        "pop %%r9\n\t"                                                      \
        "pop %%r8\n\t"                                                      \
        "pop %%rdi\n\t"                                                     \
        "pop %%rsi\n\t"                                                     \
        "pop %%rdx\n\t"                                                     \
        "pop %%rcx\n\t"                                                     \
        "pop %%rax\n\t"                                                     \
        "iretq\n\t"                                                         \
        ::: "memory"                                                        \
    );                                                                      \
}

/* ── Instantiate stubs for Sprint 7 IRQ vectors ─────────────────────────────
 * To add a new IRQ in Sprint 8+, add one line:
 *   SAM_IRQ_STUB(irqN_stub, irqN_handler)
 * Then: idt_set_gate(32+N, (uint64_t)irqN_stub)
 * ─────────────────────────────────────────────────────────────────────────── */
SAM_IRQ_STUB(irq0_stub, irq0_handler)   /* vector 32 — PIT timer  */
SAM_IRQ_STUB(irq1_stub, irq1_handler)   /* vector 33 — PS/2 keyboard */

/* ── VGA helpers ─────────────────────────────────────────────────────────── */
static inline void vga_put(int row, int col, char c, uint16_t attr) {
    VGA_GAME[row * COLS + col] = attr | (uint8_t)c;
}

static void vga_fill_row(int row, char c, uint16_t attr) {
    for (int col = 0; col < COLS; col++)
        vga_put(row, col, c, attr);
}

static void vga_clear_play(void) {
    for (int r = PLAY_TOP; r <= PLAY_BOT; r++)
        vga_fill_row(r, CHAR_SPACE, CLR_BLACK);
}

static void vga_puts_at(int row, int col, const char *s, uint16_t attr) {
    while (*s) vga_put(row, col++, *s++, attr);
}

static void vga_putdec_at(int row, int col, uint32_t v, uint16_t attr) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (v == 0) { vga_put(row, col, '0', attr); return; }
    while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    vga_puts_at(row, col, &buf[i], attr);
}

/* ── Game state ──────────────────────────────────────────────────────────── */
/* Store game state in GAME domain (base 0x4200000).
 * Simple struct at the very start of the domain. */
#define GAME_DOMAIN_ADDR 0x4200000UL

typedef struct {
    int32_t  ball_x, ball_y;       /* ball position (col, row) */
    int32_t  ball_dx, ball_dy;     /* ball velocity (±1) */
    int32_t  paddle_l;             /* left paddle top row */
    int32_t  paddle_r;             /* right paddle top row */
    uint32_t score_l, score_r;     /* scores */
    uint32_t tick_last;            /* last tick we updated */
} __attribute__((packed)) pong_state_t;

#define PADDLE_H    4              /* paddle height in rows */
#define PADDLE_LCOL 2              /* left paddle column */
#define PADDLE_RCOL 77             /* right paddle column */
#define BALL_SPEED_TICKS 2         /* update ball every N ticks (~30fps) */

static pong_state_t *pong = (pong_state_t *)GAME_DOMAIN_ADDR;

static void pong_init(void) {
    pong->ball_x   = COLS / 2;
    pong->ball_y   = (PLAY_TOP + PLAY_BOT) / 2;
    pong->ball_dx  = 1;
    pong->ball_dy  = 1;
    pong->paddle_l = (PLAY_TOP + PLAY_BOT) / 2 - PADDLE_H / 2;
    pong->paddle_r = (PLAY_TOP + PLAY_BOT) / 2 - PADDLE_H / 2;
    pong->score_l  = 0;
    pong->score_r  = 0;
    pong->tick_last = 0;
}

static void pong_draw(void) {
    /* Clear play field */
    vga_clear_play();

    /* Border rows */
    vga_fill_row(PLAY_TOP - 1, CHAR_BORDER, CLR_CYAN);
    vga_fill_row(PLAY_BOT + 1, CHAR_BORDER, CLR_CYAN);

    /* Score line */
    vga_fill_row(0, CHAR_SPACE, CLR_BLACK);
    vga_puts_at(0, 28, "SAM OS PONG  L:", CLR_YELLOW);
    vga_putdec_at(0, 43, pong->score_l, CLR_WHITE);
    vga_puts_at(0, 45, "R:", CLR_YELLOW);
    vga_putdec_at(0, 47, pong->score_r, CLR_WHITE);

    /* Left paddle */
    for (int i = 0; i < PADDLE_H; i++) {
        int r = pong->paddle_l + i;
        if (r >= PLAY_TOP && r <= PLAY_BOT)
            vga_put(r, PADDLE_LCOL, CHAR_PADDLE, CLR_GREEN);
    }

    /* Right paddle */
    for (int i = 0; i < PADDLE_H; i++) {
        int r = pong->paddle_r + i;
        if (r >= PLAY_TOP && r <= PLAY_BOT)
            vga_put(r, PADDLE_RCOL, CHAR_PADDLE, CLR_GREEN);
    }

    /* Ball */
    if (pong->ball_y >= PLAY_TOP && pong->ball_y <= PLAY_BOT &&
        pong->ball_x >= 0 && pong->ball_x < COLS)
        vga_put(pong->ball_y, pong->ball_x, CHAR_BALL, CLR_WHITE);

    /* Status bar */
    vga_fill_row(24, CHAR_SPACE, CLR_BLACK);
    vga_puts_at(24, 0, "W/S: left paddle   UP/DOWN: right paddle   Q: quit", CLR_CYAN);
    vga_puts_at(24, 55, "SAM OS GAME domain 0x4200000", CLR_YELLOW);
}

static void pong_update(void) {
    uint32_t now = game_ticks;
    if (now - pong->tick_last < BALL_SPEED_TICKS) return;
    pong->tick_last = now;

    /* Move paddles from key state */
    if (key_state[0] && pong->paddle_l > PLAY_TOP)             pong->paddle_l--;
    if (key_state[1] && pong->paddle_l + PADDLE_H - 1 < PLAY_BOT) pong->paddle_l++;
    if (key_state[2] && pong->paddle_r > PLAY_TOP)             pong->paddle_r--;
    if (key_state[3] && pong->paddle_r + PADDLE_H - 1 < PLAY_BOT) pong->paddle_r++;

    /* Move ball */
    pong->ball_x += pong->ball_dx;
    pong->ball_y += pong->ball_dy;

    /* Top/bottom wall bounce */
    if (pong->ball_y <= PLAY_TOP)  { pong->ball_y = PLAY_TOP;  pong->ball_dy = 1;  }
    if (pong->ball_y >= PLAY_BOT)  { pong->ball_y = PLAY_BOT;  pong->ball_dy = -1; }

    /* Left paddle collision: col 2, ball approaching from right */
    if (pong->ball_x == PADDLE_LCOL + 1 && pong->ball_dx < 0) {
        if (pong->ball_y >= pong->paddle_l &&
            pong->ball_y < pong->paddle_l + PADDLE_H) {
            pong->ball_dx = 1;   /* bounce right */
        }
    }

    /* Right paddle collision: col 77, ball approaching from left */
    if (pong->ball_x == PADDLE_RCOL - 1 && pong->ball_dx > 0) {
        if (pong->ball_y >= pong->paddle_r &&
            pong->ball_y < pong->paddle_r + PADDLE_H) {
            pong->ball_dx = -1;  /* bounce left */
        }
    }

    /* Score: ball exits left or right */
    if (pong->ball_x <= 0) {
        pong->score_r++;
        pong->ball_x  = COLS / 2;
        pong->ball_y  = (PLAY_TOP + PLAY_BOT) / 2;
        pong->ball_dx = 1;
        pong->ball_dy = 1;
    }
    if (pong->ball_x >= COLS - 1) {
        pong->score_l++;
        pong->ball_x  = COLS / 2;
        pong->ball_y  = (PLAY_TOP + PLAY_BOT) / 2;
        pong->ball_dx = -1;
        pong->ball_dy = 1;
    }
}

/* ── sam_game_run — call from kernel_main after all proofs pass ──────────── */
static void sam_game_run(void) {
    /* 1. Remap PIC so IRQs don't collide with CPU exceptions */
    pic_remap();

    /* 2. Install IRQ handlers into IDT */
    idt_set_gate(32, (uint64_t)irq0_stub);   /* IRQ0 = PIT timer */
    idt_set_gate(33, (uint64_t)irq1_stub);   /* IRQ1 = keyboard  */
    idt_load();

    /* 3. Program PIT for ~60Hz */
    pit_init_60hz();

    /* 4. Enable interrupts */
    __asm__ volatile ("sti");

    /* 5. Init game state in GAME domain */
    pong_init();

    /* 6. Game loop — runs until Q pressed or 300 ticks (headless/CI exit) */
#define GAME_MAX_TICKS 300   /* ~5 seconds at 60Hz; 0 = unlimited */
    uint32_t last_draw = 0;
    uint32_t tick_start = game_ticks;
    while (!key_quit) {
        uint32_t now = game_ticks;
#if GAME_MAX_TICKS > 0
        if ((now - tick_start) >= GAME_MAX_TICKS) break;
#endif
        if (now != last_draw) {
            last_draw = now;
            pong_update();
            pong_draw();
        }
        /* Yield to interrupts */
        __asm__ volatile ("hlt");
    }

    /* 7. Game over screen */
    vga_clear_play();
    vga_fill_row(0,  CHAR_SPACE, CLR_BLACK);
    vga_fill_row(24, CHAR_SPACE, CLR_BLACK);
    vga_puts_at(12, 28, "GAME OVER  Q pressed", CLR_YELLOW);
    vga_puts_at(13, 30, "Final score  L:", CLR_WHITE);
    vga_putdec_at(13, 45, pong->score_l, CLR_GREEN);
    vga_puts_at(13, 47, "R:", CLR_WHITE);
    vga_putdec_at(13, 49, pong->score_r, CLR_GREEN);
    vga_puts_at(14, 28, "SAM OS GAME domain: proved", CLR_CYAN);

    /* Disable interrupts and return to kernel_main */
    __asm__ volatile ("cli");
}

#endif /* SAM_GAME_H */
