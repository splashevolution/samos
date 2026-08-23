/*
 * SAM OS — kernel/ps2kbd.h
 * Sprint 15 / Phase 3: Interrupt-driven PS/2 keyboard driver
 *
 * Previously the wizard and shell polled port 0x60 directly via
 * wz_wait_key(). This driver replaces polling with a proper IRQ 1
 * handler so the keyboard works in any context and the shell can
 * use a clean blocking read without tight-loop starvation.
 *
 * Design:
 *   - Initialises the 8259A PIC (master only — we keep slave masked
 *     until we need it). Remaps master IRQs to vectors 0x20-0x27
 *     (just above the 32 CPU exception vectors), slave to 0x28-0x2F.
 *   - Installs _ps2kbd_isr at IDT vector 0x21 (IRQ 1).
 *   - Handler: reads scancode from 0x60, pushes into a 64-byte ring
 *     buffer, sends EOI to PIC.
 *   - US scancode→ASCII lookup (set 1 make codes only; shift support).
 *   - Exposes:
 *       ps2_getchar()   — non-blocking, returns 0 if buffer empty
 *       ps2_wait_char() — blocks until a character is available
 *
 * After ps2kbd_init(), wizard.h and shell.h can call ps2_wait_char()
 * instead of the polling wz_wait_key() — but we keep wz_wait_key()
 * as fallback for the wizard since it was battle-tested.
 *
 * No libc. No external dependencies. Requires idt.h to be included first.
 */

#ifndef SAM_PS2KBD_H
#define SAM_PS2KBD_H

#include <stdint.h>

/* ── 8259A PIC constants ──────────────────────────────────────────────── */
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1
#define PIC_EOI     0x20    /* End-of-interrupt command */

#define PIC1_VECTOR 0x20    /* master IRQs 0-7  → vectors 0x20-0x27 */
#define PIC2_VECTOR 0x28    /* slave  IRQs 8-15 → vectors 0x28-0x2F */

/* ── PS/2 controller ports ────────────────────────────────────────────── */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_OBF     0x01    /* Output Buffer Full */

/* ── Ring buffer ──────────────────────────────────────────────────────── */
#define KBD_BUF_SIZE 64

static volatile uint8_t _kbd_buf[KBD_BUF_SIZE];
static volatile uint8_t _kbd_head = 0;
static volatile uint8_t _kbd_tail = 0;
static volatile uint8_t _kbd_shift = 0;    /* 1 if shift key held */

static inline void _kbd_buf_push(uint8_t c) {
    uint8_t next = (_kbd_tail + 1) % KBD_BUF_SIZE;
    if (next == _kbd_head) return;  /* buffer full — drop */
    _kbd_buf[_kbd_tail] = c;
    _kbd_tail = next;
}

static inline uint8_t _kbd_buf_pop(void) {
    if (_kbd_head == _kbd_tail) return 0;
    uint8_t c = _kbd_buf[_kbd_head];
    _kbd_head = (_kbd_head + 1) % KBD_BUF_SIZE;
    return c;
}

/* ── US scancode set 1 → ASCII (unshifted) ───────────────────────────── */
/* Index = make code (0x01-0x58). 0 = not mapped / non-printable. */
static const uint8_t _kbd_map_lo[89] = {
    0,    /* 0x00 — unused */
    0x1B, /* 0x01 ESC */
    '1','2','3','4','5','6','7','8','9','0','-','=',
    '\b', /* 0x0E backspace */
    '\t', /* 0x0F tab */
    'q','w','e','r','t','y','u','i','o','p','[',']',
    '\n', /* 0x1C enter */
    0,    /* 0x1D left ctrl — not a printable char */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,    /* 0x2A left shift */
    '\\',
    'z','x','c','v','b','n','m',',','.','/',
    0,    /* 0x36 right shift */
    '*',  /* 0x37 numpad * */
    0,    /* 0x38 left alt */
    ' ',  /* 0x39 space */
    0,    /* 0x3A caps lock */
    0,0,0,0,0,0,0,0,0,0, /* F1-F10 */
    0,    /* 0x45 num lock */
    0,    /* 0x46 scroll lock */
    '7','8','9','-','4','5','6','+','1','2','3','0','.', /* numpad */
};

/* Shifted versions of printable keys */
static const uint8_t _kbd_map_hi[89] = {
    0,
    0x1B,
    '!','@','#','$','%','^','&','*','(',')','_','+',
    '\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}',
    '\n',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,
    '|',
    'Z','X','C','V','B','N','M','<','>','?',
    0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
};

/* ── I/O helpers (same pattern as panic.h) ────────────────────────────── */
static inline void _kbd_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t _kbd_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void _kbd_io_wait(void) {
    /* Small I/O delay via writing to port 0x80 (POST port, safe to write) */
    __asm__ volatile ("outb %0, $0x80" :: "a"((uint8_t)0));
}

/* ── 8259A PIC initialisation (ICW1-ICW4) ────────────────────────────── */
static void pic_init(void) {
    /* ICW1: start init, edge triggered, single/cascade mode */
    _kbd_outb(PIC1_CMD, 0x11); _kbd_io_wait();
    _kbd_outb(PIC2_CMD, 0x11); _kbd_io_wait();
    /* ICW2: vector offsets */
    _kbd_outb(PIC1_DATA, PIC1_VECTOR); _kbd_io_wait();
    _kbd_outb(PIC2_DATA, PIC2_VECTOR); _kbd_io_wait();
    /* ICW3: cascade wiring */
    _kbd_outb(PIC1_DATA, 0x04); _kbd_io_wait(); /* master: slave on IRQ 2 */
    _kbd_outb(PIC2_DATA, 0x02); _kbd_io_wait(); /* slave: cascade identity 2 */
    /* ICW4: 8086 mode */
    _kbd_outb(PIC1_DATA, 0x01); _kbd_io_wait();
    _kbd_outb(PIC2_DATA, 0x01); _kbd_io_wait();
    /* Mask all IRQs initially, then unmask only IRQ 1 (keyboard) */
    _kbd_outb(PIC1_DATA, 0xFD); /* 1111 1101 — only IRQ1 unmasked */
    _kbd_outb(PIC2_DATA, 0xFF); /* all slave IRQs masked */
}

/* ── IRQ handler (called from IDT stub at vector 0x21) ───────────────── */
/*
 * C handler called from the generic ISR stub machinery.
 * We install this at vector 0x21 using idt_set_gate().
 * The stub saves registers, calls us, restores, then iretq.
 *
 * We cannot use the cpu_frame_t path here because IRQ handlers don't
 * go through sam_exception_handler. Instead we use a dedicated naked
 * stub defined below via IDT_DEFINE_KBD_STUB().
 */
void ps2kbd_irq_handler(void) {
    /* Read scancode — always drain OBF even if we drop it */
    uint8_t sc = _kbd_inb(PS2_DATA);

    /* Track shift keys: 0x2A = left shift make, 0xAA = release
     *                   0x36 = right shift make, 0xB6 = release */
    if (sc == 0x2A || sc == 0x36) { _kbd_shift = 1; }
    else if (sc == 0xAA || sc == 0xB6) { _kbd_shift = 0; }
    else if (sc < 0x80) {
        /* Make code — convert to ASCII */
        if (sc < 89) {
            uint8_t ch = _kbd_shift ? _kbd_map_hi[sc] : _kbd_map_lo[sc];
            if (ch) _kbd_buf_push(ch);
        }
    }
    /* Break codes (sc >= 0x80, other than shift releases) — ignored */

    /* Send EOI to master PIC */
    _kbd_outb(PIC1_CMD, PIC_EOI);
}

/*
 * IDT_DEFINE_KBD_STUB() — emit the naked IRQ 1 stub in .text.
 * Unlike the exception stubs, IRQ handlers don't get an error code
 * from the CPU. Our stub just saves caller-saved regs, calls the C
 * handler, restores, and iretqs.
 *
 * Call this macro exactly once from main.c (after IDT_DEFINE_STUBS).
 */
#define IDT_DEFINE_KBD_STUB() \
__asm__( \
".text\n\t" \
"_ps2kbd_isr:\n\t" \
    "push %rax\n\t" \
    "push %rcx\n\t" \
    "push %rdx\n\t" \
    "push %rsi\n\t" \
    "push %rdi\n\t" \
    "push %r8\n\t"  \
    "push %r9\n\t"  \
    "push %r10\n\t" \
    "push %r11\n\t" \
    "and $-16, %rsp\n\t" \
    "sub $8, %rsp\n\t"   \
    "call ps2kbd_irq_handler\n\t" \
    "add $8, %rsp\n\t"   \
    "pop %r11\n\t" \
    "pop %r10\n\t" \
    "pop %r9\n\t"  \
    "pop %r8\n\t"  \
    "pop %rdi\n\t" \
    "pop %rsi\n\t" \
    "pop %rdx\n\t" \
    "pop %rcx\n\t" \
    "pop %rax\n\t" \
    "iretq\n\t"    \
);

extern void _ps2kbd_isr(void);

/* ── Public API ───────────────────────────────────────────────────────── */

/*
 * ps2kbd_init() — call after idt_init().
 * Remaps the 8259A PIC, installs the keyboard ISR, enables interrupts.
 */
static void ps2kbd_init(void) {
    pic_init();
    idt_set_gate(0x21, _ps2kbd_isr);   /* vector 33 = IRQ 1 */
    __asm__ volatile ("sti");           /* enable interrupts */
}

/*
 * ps2_getchar() — non-blocking.
 * Returns next ASCII character from ring buffer, or 0 if none.
 */
static inline uint8_t ps2_getchar(void) {
    return _kbd_buf_pop();
}

/*
 * ps2_wait_char() — blocking.
 * Spins (with HLT to yield to hypervisor) until a character is available.
 */
static inline uint8_t ps2_wait_char(void) {
    uint8_t c;
    while ((c = ps2_getchar()) == 0)
        __asm__ volatile ("hlt");
    return c;
}

/*
 * ps2_readline(buf, maxlen) — reads until Enter, echoes to serial.
 * Returns number of characters written (not including null terminator).
 * Used by the kernel shell for command input.
 */
static inline int ps2_readline(char *buf, int maxlen,
                                void (*echo_char)(char)) {
    int len = 0;
    for (;;) {
        uint8_t c = ps2_wait_char();
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            if (echo_char) echo_char('\n');
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                if (echo_char) { echo_char('\b'); echo_char(' '); echo_char('\b'); }
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len < maxlen - 1) {
            buf[len++] = (char)c;
            if (echo_char) echo_char((char)c);
        }
    }
}

#endif /* SAM_PS2KBD_H */
