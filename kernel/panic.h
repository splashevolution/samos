/*
 * SAM OS — kernel/panic.h
 * Sprint 14: Kernel panic screen
 *
 * sam_panic() is called by the exception handler in idt.h.
 * It:
 *   1. Disables interrupts (CLI)
 *   2. Prints a panic screen to VGA text mode (always available at 0xB8000)
 *   3. If g_fb.ready, also draws a red panic overlay on the pixel framebuffer
 *   4. Emits the same info to the serial port
 *   5. Halts forever (HLT loop)
 *
 * No return. No recovery. Phase 3 can add a reboot-on-panic option.
 *
 * Include AFTER fb.h so g_fb is visible.
 */

#ifndef SAM_PANIC_H
#define SAM_PANIC_H

#include <stdint.h>
#include "fb.h"          /* g_fb, fb_fill_rect, fb_puts_small */

/* ── VGA text panic helpers ───────────────────────────────────────────── */
#define PANIC_VGA_BASE   ((volatile uint16_t *)0xB8000)
#define PANIC_VGA_COLS   80
#define PANIC_VGA_ROWS   25
#define PANIC_ATTR_RED   0x4F00   /* white on red */
#define PANIC_ATTR_WHITE 0x4700   /* light grey on red */
#define PANIC_ATTR_CYAN  0x4B00   /* cyan on red */

static int  _pnc_col = 0, _pnc_row = 2;   /* start below the border */

static inline void _pnc_vga_char(char c, uint16_t attr) {
    volatile uint16_t *vga = PANIC_VGA_BASE;
    if (c == '\n') {
        _pnc_col = 0;
        if (++_pnc_row >= PANIC_VGA_ROWS) _pnc_row = PANIC_VGA_ROWS - 1;
        return;
    }
    if (_pnc_col >= PANIC_VGA_COLS) {
        _pnc_col = 0;
        if (++_pnc_row >= PANIC_VGA_ROWS) _pnc_row = PANIC_VGA_ROWS - 1;
    }
    vga[_pnc_row * PANIC_VGA_COLS + _pnc_col++] = attr | (uint8_t)c;
}

static inline void _pnc_vga_str(const char *s, uint16_t attr) {
    while (*s) _pnc_vga_char(*s++, attr);
}

static inline void _pnc_vga_hex(uint64_t v, uint16_t attr) {
    const char *h = "0123456789ABCDEF";
    _pnc_vga_str("0x", attr);
    for (int i = 60; i >= 0; i -= 4)
        _pnc_vga_char(h[(v >> i) & 0xF], attr);
}

static inline void _pnc_vga_dec(uint64_t v, uint16_t attr) {
    char buf[21]; int i = 20; buf[i] = '\0';
    if (v == 0) { _pnc_vga_char('0', attr); return; }
    while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    _pnc_vga_str(&buf[i], attr);
}

/* ── Serial helpers (reuse the same port as main.c) ──────────────────── */
#define _PNC_SERIAL 0x3F8

static inline uint8_t _pnc_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void _pnc_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline void _pnc_serial_char(char c) {
    while ((_pnc_inb(_PNC_SERIAL + 5) & 0x20) == 0)
        __asm__ volatile ("pause");
    _pnc_outb(_PNC_SERIAL, (uint8_t)c);
}

static inline void _pnc_serial_str(const char *s) {
    while (*s) {
        if (*s == '\n') _pnc_serial_char('\r');
        _pnc_serial_char(*s++);
    }
}

static inline void _pnc_serial_hex(uint64_t v) {
    const char *h = "0123456789ABCDEF";
    _pnc_serial_str("0x");
    for (int i = 60; i >= 0; i -= 4)
        _pnc_serial_char(h[(v >> i) & 0xF]);
}

static inline void _pnc_serial_dec(uint64_t v) {
    char buf[21]; int i = 20; buf[i] = '\0';
    if (v == 0) { _pnc_serial_char('0'); return; }
    while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    _pnc_serial_str(&buf[i]);
}

/* ── Pixel framebuffer panic overlay ─────────────────────────────────── */
/*
 * If the pixel framebuffer is active we draw a dark-red banner across the
 * top third of the screen with the panic info.
 * We use fb_fill_rect (already in fb.h) for the background and a minimal
 * pixel font for the text.  We reuse fb_draw_char from wizard.h if it's
 * available, but panic.h is included before wizard.h so we use fb.h's own
 * helpers directly to avoid a dependency loop.
 */

/* Simple 8×8 bitmap font for the panic overlay — ASCII 32-127 */
/* We borrow the same tiny font approach as shell.h */
static const uint8_t _pnc_font8[96][8] = {
    /* 32 SPACE */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 !     */ {0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x00},
    /* 34 "     */ {0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 #     */ {0x66,0xFF,0x66,0x66,0xFF,0x66,0x00,0x00},
    /* 36 $     */ {0x18,0x7E,0x18,0x7E,0x18,0x7E,0x18,0x00},
    /* 37 %     */ {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},
    /* 38 &     */ {0x3C,0x66,0x3C,0x38,0x67,0x66,0x3F,0x00},
    /* 39 '     */ {0x06,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 40 (     */ {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    /* 41 )     */ {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    /* 42 *     */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 43 +     */ {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    /* 44 ,     */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    /* 45 -     */ {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    /* 46 .     */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    /* 47 /     */ {0x00,0x03,0x06,0x0C,0x18,0x30,0x60,0x00},
    /* 48 0     */ {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
    /* 49 1     */ {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    /* 50 2     */ {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},
    /* 51 3     */ {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    /* 52 4     */ {0x06,0x0E,0x1E,0x66,0x7F,0x06,0x06,0x00},
    /* 53 5     */ {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    /* 54 6     */ {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00},
    /* 55 7     */ {0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00},
    /* 56 8     */ {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
    /* 57 9     */ {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    /* 58 :     */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    /* 59 ;     */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    /* 60 <     */ {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    /* 61 =     */ {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    /* 62 >     */ {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    /* 63 ?     */ {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},
    /* 64 @     */ {0x3E,0x63,0x6F,0x69,0x6F,0x60,0x3E,0x00},
    /* 65 A     */ {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00},
    /* 66 B     */ {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    /* 67 C     */ {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    /* 68 D     */ {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
    /* 69 E     */ {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    /* 70 F     */ {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00},
    /* 71 G     */ {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00},
    /* 72 H     */ {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    /* 73 I     */ {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 74 J     */ {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00},
    /* 75 K     */ {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    /* 76 L     */ {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
    /* 77 M     */ {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    /* 78 N     */ {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},
    /* 79 O     */ {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    /* 80 P     */ {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},
    /* 81 Q     */ {0x3C,0x66,0x66,0x66,0x66,0x3C,0x0E,0x00},
    /* 82 R     */ {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00},
    /* 83 S     */ {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    /* 84 T     */ {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    /* 85 U     */ {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    /* 86 V     */ {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
    /* 87 W     */ {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    /* 88 X     */ {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
    /* 89 Y     */ {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    /* 90 Z     */ {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},
    /* 91 [     */ {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    /* 92 \     */ {0x00,0x60,0x30,0x18,0x0C,0x06,0x00,0x00},
    /* 93 ]     */ {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    /* 94 ^     */ {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    /* 95 _     */ {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
    /* 96 `     */ {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 97-122: lowercase letters */
    /* 97  a    */ {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    /* 98  b    */ {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},
    /* 99  c    */ {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},
    /* 100 d    */ {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},
    /* 101 e    */ {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    /* 102 f    */ {0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00},
    /* 103 g    */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    /* 104 h    */ {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},
    /* 105 i    */ {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    /* 106 j    */ {0x06,0x00,0x06,0x06,0x06,0x66,0x3C,0x00},
    /* 107 k    */ {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    /* 108 l    */ {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 109 m    */ {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},
    /* 110 n    */ {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
    /* 111 o    */ {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    /* 112 p    */ {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
    /* 113 q    */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    /* 114 r    */ {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00},
    /* 115 s    */ {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    /* 116 t    */ {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00},
    /* 117 u    */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    /* 118 v    */ {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
    /* 119 w    */ {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    /* 120 x    */ {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
    /* 121 y    */ {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},
    /* 122 z    */ {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},
    /* 123-127  */ {0x00},{0x00},{0x00},{0x00},{0x00},
};

/* Draw one character on the pixel framebuffer at pixel position (px, py) */
static void _pnc_fb_char(uint32_t px, uint32_t py, char c,
                          uint32_t fg, uint32_t bg) {
    if (!g_fb.ready) return;
    if ((uint8_t)c < 32 || (uint8_t)c > 127) c = '?';
    const uint8_t *glyph = _pnc_font8[(uint8_t)c - 32];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint32_t color = (glyph[row] & (0x80 >> col)) ? fg : bg;
            uint32_t x = px + (uint32_t)col * 2;   /* 2× scale */
            uint32_t y = py + (uint32_t)row * 2;
            if (x + 1 < g_fb.width && y + 1 < g_fb.height) {
                fb_pixel(x,   y,   color);
                fb_pixel(x+1, y,   color);
                fb_pixel(x,   y+1, color);
                fb_pixel(x+1, y+1, color);
            }
        }
    }
}

static void _pnc_fb_str(uint32_t *px, uint32_t py, const char *s,
                         uint32_t fg, uint32_t bg) {
    while (*s) {
        _pnc_fb_char(*px, py, *s++, fg, bg);
        *px += 16;  /* 8px × 2 scale */
    }
}

static void _pnc_fb_hex(uint32_t *px, uint32_t py, uint64_t v,
                         uint32_t fg, uint32_t bg) {
    const char *h = "0123456789ABCDEF";
    _pnc_fb_str(px, py, "0x", fg, bg);
    for (int i = 60; i >= 0; i -= 4) {
        char c = h[(v >> i) & 0xF];
        _pnc_fb_char(*px, py, c, fg, bg);
        *px += 16;
    }
}

/* ── sam_panic — the main panic entry point ───────────────────────────── */
/*
 * Parameters:
 *   name       — exception name string (e.g. "#GP General Protection")
 *   vector     — exception vector number (0-31)
 *   error_code — CPU error code (or 0 if none)
 *   rip        — instruction pointer at fault
 *   rsp        — stack pointer at fault
 *   cr2        — faulting address (only meaningful for #PF / vector 14)
 */
__attribute__((noreturn))
void sam_panic(const char *name, uint64_t vector, uint64_t error_code,
               uint64_t rip, uint64_t rsp, uint64_t cr2)
{
    /* 1. Freeze — no more interrupts */
    __asm__ volatile ("cli");

    /* 2. VGA text panic screen — always works, even if framebuffer is dead */
    {
        volatile uint16_t *vga = PANIC_VGA_BASE;

        /* Paint entire screen red */
        for (int i = 0; i < PANIC_VGA_COLS * PANIC_VGA_ROWS; i++)
            vga[i] = PANIC_ATTR_RED | ' ';

        /* Top border */
        _pnc_row = 0; _pnc_col = 0;
        for (int i = 0; i < PANIC_VGA_COLS; i++)
            _pnc_vga_char('=', PANIC_ATTR_RED);

        /* Title */
        _pnc_row = 1; _pnc_col = 0;
        _pnc_vga_str("  *** SAM OS KERNEL PANIC ***", PANIC_ATTR_RED);

        /* Exception name */
        _pnc_row = 3; _pnc_col = 2;
        _pnc_vga_str("Exception : ", PANIC_ATTR_WHITE);
        _pnc_vga_str(name, PANIC_ATTR_RED);

        /* Vector */
        _pnc_row = 4; _pnc_col = 2;
        _pnc_vga_str("Vector    : ", PANIC_ATTR_WHITE);
        _pnc_vga_dec(vector, PANIC_ATTR_CYAN);

        /* Error code */
        _pnc_row = 5; _pnc_col = 2;
        _pnc_vga_str("Err code  : ", PANIC_ATTR_WHITE);
        _pnc_vga_hex(error_code, PANIC_ATTR_CYAN);

        /* RIP */
        _pnc_row = 6; _pnc_col = 2;
        _pnc_vga_str("RIP       : ", PANIC_ATTR_WHITE);
        _pnc_vga_hex(rip, PANIC_ATTR_CYAN);

        /* RSP */
        _pnc_row = 7; _pnc_col = 2;
        _pnc_vga_str("RSP       : ", PANIC_ATTR_WHITE);
        _pnc_vga_hex(rsp, PANIC_ATTR_CYAN);

        /* CR2 (only show if page fault) */
        if (vector == 14) {
            _pnc_row = 8; _pnc_col = 2;
            _pnc_vga_str("CR2 (fault addr) : ", PANIC_ATTR_WHITE);
            _pnc_vga_hex(cr2, PANIC_ATTR_CYAN);
        }

        _pnc_row = 10; _pnc_col = 2;
        _pnc_vga_str("System halted. Reset to restart.", PANIC_ATTR_WHITE);

        /* Bottom border */
        _pnc_row = PANIC_VGA_ROWS - 1; _pnc_col = 0;
        for (int i = 0; i < PANIC_VGA_COLS; i++)
            _pnc_vga_char('=', PANIC_ATTR_RED);
    }

    /* 3. Pixel framebuffer panic overlay (if active) */
    if (g_fb.ready) {
        /* Dark red banner across top 220px */
        fb_fill_rect(0, 0, g_fb.width, 220, 0xFF1A0000);

        uint32_t px, py;

        /* Title */
        px = 20; py = 16;
        _pnc_fb_str(&px, py, "*** SAM OS KERNEL PANIC ***",
                    0xFFFF4444, 0xFF1A0000);

        /* Exception name */
        px = 20; py = 48;
        _pnc_fb_str(&px, py, "Exception : ", 0xFFAAAAAA, 0xFF1A0000);
        _pnc_fb_str(&px, py, name,           0xFFFF6666, 0xFF1A0000);

        /* Vector */
        px = 20; py = 72;
        _pnc_fb_str(&px, py, "Vector    : ", 0xFFAAAAAA, 0xFF1A0000);
        /* print decimal vector */
        {
            char vbuf[8]; int vi = 7; vbuf[vi] = '\0';
            uint64_t vv = vector;
            if (vv == 0) { vbuf[--vi] = '0'; }
            else while (vv && vi > 0) { vbuf[--vi] = '0' + (vv % 10); vv /= 10; }
            _pnc_fb_str(&px, py, &vbuf[vi], 0xFF66FFFF, 0xFF1A0000);
        }

        /* Error code */
        px = 20; py = 96;
        _pnc_fb_str(&px, py, "Err code  : ", 0xFFAAAAAA, 0xFF1A0000);
        _pnc_fb_hex(&px, py, error_code,      0xFF66FFFF, 0xFF1A0000);

        /* RIP */
        px = 20; py = 120;
        _pnc_fb_str(&px, py, "RIP       : ", 0xFFAAAAAA, 0xFF1A0000);
        _pnc_fb_hex(&px, py, rip,            0xFF66FFFF, 0xFF1A0000);

        /* RSP */
        px = 20; py = 144;
        _pnc_fb_str(&px, py, "RSP       : ", 0xFFAAAAAA, 0xFF1A0000);
        _pnc_fb_hex(&px, py, rsp,            0xFF66FFFF, 0xFF1A0000);

        /* CR2 */
        if (vector == 14) {
            px = 20; py = 168;
            _pnc_fb_str(&px, py, "CR2       : ", 0xFFAAAAAA, 0xFF1A0000);
            _pnc_fb_hex(&px, py, cr2,           0xFF66FFFF, 0xFF1A0000);
        }

        /* Halt message */
        px = 20; py = 196;
        _pnc_fb_str(&px, py, "System halted.  Reset to restart.",
                    0xFFFFFFFF, 0xFF1A0000);
    }

    /* 4. Serial output */
    _pnc_serial_str("\n\n");
    _pnc_serial_str("============================================================\n");
    _pnc_serial_str("  *** SAM OS KERNEL PANIC ***\n");
    _pnc_serial_str("============================================================\n");
    _pnc_serial_str("  Exception : "); _pnc_serial_str(name); _pnc_serial_str("\n");
    _pnc_serial_str("  Vector    : "); _pnc_serial_dec(vector); _pnc_serial_str("\n");
    _pnc_serial_str("  Err code  : "); _pnc_serial_hex(error_code); _pnc_serial_str("\n");
    _pnc_serial_str("  RIP       : "); _pnc_serial_hex(rip); _pnc_serial_str("\n");
    _pnc_serial_str("  RSP       : "); _pnc_serial_hex(rsp); _pnc_serial_str("\n");
    if (vector == 14) {
        _pnc_serial_str("  CR2       : "); _pnc_serial_hex(cr2); _pnc_serial_str("\n");
    }
    _pnc_serial_str("  System halted.\n");
    _pnc_serial_str("============================================================\n");

    /* 5. Halt forever */
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

#endif /* SAM_PANIC_H */
