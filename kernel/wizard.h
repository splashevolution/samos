/*
 * SAM OS — kernel/wizard.h
 * Sprint 13: Graphical First-Boot Configuration Wizard (OOBE)
 *
 * Full-pixel GUI using the VESA framebuffer (kernel/fb.h).
 * Ubuntu/Windows OOBE style: dark sidebar with progress steps on the left,
 * large content panel on the right, pixel-drawn input fields and buttons.
 *
 * Wizard screens (in order):
 *   1. Welcome   — animated welcome, press Enter to begin
 *   2. Hostname  — text input field with live cursor
 *   3. WiFi      — real PCI scan result; SSID entry if adapter found
 *   4. Summary   — confirm hostname + WiFi, writes sam_boot_config_t
 *
 * WiFi detection: scans PCI classes 0x0280, 0x0281, 0x0282 (all wireless
 * subclasses). VirtualBox users: attach a USB WiFi dongle and enable USB
 * passthrough in VirtualBox settings for real detection.
 *
 * Falls back to VGA text mode automatically if framebuffer is unavailable
 * (headless / grub gfxmode not set).
 *
 * Security: no passwords stored, no keys, no credentials.
 * Full 802.11 association deferred to Sprint 17 (network stack).
 *
 * No libc. No heap. No external dependencies.
 */

#ifndef SAM_WIZARD_H
#define SAM_WIZARD_H

#include <stdint.h>
#include <stddef.h>
#include "mcp.h"
#include "fb.h"
#include "boot_config.h"

/* ── PS/2 keyboard ─────────────────────────────────────────────────────── */
#ifndef SC_ENTER
#define SC_ENTER    0x1C
#endif
#define WZ_SC_BACK   0x0E
#define WZ_SC_E0     0xE0
#define WZ_SC_LSHIFT 0x2A
#define WZ_SC_RSHIFT 0x36
#define WZ_SC_TAB    0x0F

static inline uint8_t wz_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1,%0" : "=a"(r) : "Nd"(port));
    return r;
}
static uint8_t wz_wait_key(void) {
    for (;;) {
        /* `pause` is mandatory in a spin-wait loop running inside a VM.
         * Without it the tight poll starves the hypervisor's i8042 emulation:
         * VirtualBox never gets a chance to set OBF in the status register,
         * so the loop spins forever seeing 0x00 on port 0x64.
         * `pause` signals a spin-wait to the CPU/hypervisor and yields
         * enough time for the emulated i8042 to inject keyboard data.     */
        __asm__ volatile ("pause");
        if (wz_inb(0x64) & 0x01) {
            uint8_t sc = wz_inb(0x60);
            if (sc < 0x80) return sc;
        }
    }
}

/* ── String helpers (no libc) ──────────────────────────────────────────── */
static int __attribute__((unused)) wz_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}
static void wz_strcpy(char *d, const char *s, int max) {
    int i = 0; while (i < max && s[i]) { d[i] = s[i]; i++; } d[i] = '\0';
}

/* ── Scancode → ASCII (US layout) ──────────────────────────────────────── */
static const char wz_sc_alpha[58] = {
    0,  0,  '1','2','3','4','5','6','7','8','9','0','-','=', 0,
    0,  'q','w','e','r','t','y','u','i','o','p','[',']', 0,
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' '
};
static const char wz_sc_alpha_sh[58] = {
    0,  0,  '!','@','#','$','%','^','&','*','(',')','_','+', 0,
    0,  'Q','W','E','R','T','Y','U','I','O','P','{','}', 0,
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' '
};

/* ── Wizard state ──────────────────────────────────────────────────────── */
#define WZ_HOSTNAME_MAX  32
#define WZ_SSID_MAX      32

typedef struct {
    char hostname[WZ_HOSTNAME_MAX + 1];
    int  hostname_len;
    int  wifi_present;
    char ssid[WZ_SSID_MAX + 1];
    int  ssid_len;
    int  wifi_skip;
} wz_state_t;

/* ── Colour palette (GUI) ──────────────────────────────────────────────── */
/* Background layers */
#define WZ_BG           0x00121A2B   /* deep navy page background */
#define WZ_SIDEBAR_BG   0x000D1117   /* sidebar almost-black */
#define WZ_PANEL_BG     0x001A2340   /* content card */
#define WZ_PANEL_BORDER 0x00253758   /* card border */
#define WZ_INPUT_BG     0x000F1822   /* text input field background */
#define WZ_INPUT_ACTIVE 0x00162030   /* active input field */
#define WZ_INPUT_BORDER 0x002A4070   /* input border */
#define WZ_INPUT_CURSOR 0x0044AAFF   /* cursor bar */

/* Text colours */
#define WZ_TEXT_H1      0x00FFFFFF   /* main heading */
#define WZ_TEXT_H2      0x00C8E0FF   /* sub-heading */
#define WZ_TEXT_BODY    0x0089A8C8   /* body / description */
#define WZ_TEXT_DIM     0x00445566   /* dimmed label */
#define WZ_TEXT_INPUT   0x00E0F0FF   /* text inside input box */
#define WZ_TEXT_GOOD    0x0044CC88   /* green success / detected */
#define WZ_TEXT_WARN    0x00FFAA33   /* amber warning */
#define WZ_TEXT_KEY     0x0088CCFF   /* keyboard hint */

/* Accent / brand */
#define WZ_ACCENT       0x000088FF   /* SAM OS blue */
#define WZ_ACCENT_LIT   0x0044AAFF   /* lighter blue for highlights */
#define WZ_STEP_DONE    0x0022AA66   /* completed step dot */
#define WZ_STEP_ACTIVE  0x000088FF   /* current step dot */
#define WZ_STEP_TODO    0x00334455   /* pending step dot */

/* Button */
#define WZ_BTN_BG       0x000055BB   /* normal button */
#define WZ_BTN_HOVER    0x000077DD   /* (unused in text-input flow) */
#define WZ_BTN_TEXT     0x00FFFFFF

/* ── Layout constants (1024 × 768) ─────────────────────────────────────── */
#define WZ_W   1024
#define WZ_H   768

/* Sidebar */
#define WZ_SB_W   260   /* sidebar width */
#define WZ_SB_PAD  28   /* sidebar horizontal padding */

/* Content panel */
#define WZ_CP_X   (WZ_SB_W + 1)
#define WZ_CP_W   (WZ_W - WZ_SB_W - 1)
#define WZ_CP_PAD  50   /* content horizontal padding */

/* Font size: fb.h provides 8×16 glyphs; we scale ×2 for headings */
#define WZ_CH_W   8
#define WZ_CH_H   16

/* ── fb.h font rendering (pixel-accurate) ───────────────────────────────
 * fb_putchar / fb_puts are light wrappers that the existing fb.h may or
 * may not provide. We define our own here so wizard.h is self-contained.
 * ─────────────────────────────────────────────────────────────────────── */
static void wz_putchar(uint32_t x, uint32_t y, char c,
                       uint32_t fg, uint32_t bg, int scale) {
    if (!g_fb.ready) return;
    if ((uint8_t)c < 32 || (uint8_t)c > 127) c = '?';
    const uint8_t *glyph = FB_FONT_8X16[(uint8_t)(c - 32)];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t colour = (bits & (0x80 >> col)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    fb_pixel(x + col * scale + sx, y + row * scale + sy, colour);
        }
    }
}

/* Draw string; returns x position after last char */
static uint32_t wz_puts(uint32_t x, uint32_t y, const char *s,
                         uint32_t fg, uint32_t bg, int scale) {
    while (*s) {
        wz_putchar(x, y, *s++, fg, bg, scale);
        x += WZ_CH_W * scale;
    }
    return x;
}

/* Centred string within a pixel region */
static void wz_puts_centre(uint32_t cx, uint32_t y, const char *s,
                            uint32_t fg, uint32_t bg, int scale) {
    int len = 0; while (s[len]) len++;
    uint32_t total_w = (uint32_t)(len * WZ_CH_W * scale);
    uint32_t x = (cx >= total_w / 2) ? (cx - total_w / 2) : 0;
    wz_puts(x, y, s, fg, bg, scale);
}

/* ── Primitive: rounded-corner rectangle (pixel approximation) ──────────── */
static void wz_fill_rect(uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t col) {
    fb_fill_rect(x, y, w, h, col);
}

static void wz_rounded_rect(uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             uint32_t fill, uint32_t border,
                             uint32_t r) {
    /* Fill body */
    fb_fill_rect(x + r, y,     w - 2*r, h,     fill);
    fb_fill_rect(x,     y + r, r,       h-2*r, fill);
    fb_fill_rect(x+w-r, y + r, r,       h-2*r, fill);
    /* Simple corner approximation (octagon) */
    for (uint32_t i = 0; i < r; i++) {
        uint32_t span = r - (r * i / r);
        fb_fill_rect(x + span,     y + i,         w - 2*span, 1, fill);
        fb_fill_rect(x + span,     y + h - 1 - i, w - 2*span, 1, fill);
    }
    /* Border */
    if (border != fill) {
        fb_draw_rect(x, y, w, h, border, 2);
    }
    (void)r;
}

/* ── Step dot (sidebar progress indicator) ──────────────────────────────── */
static void wz_draw_step_dot(uint32_t cx, uint32_t cy, uint32_t r,
                              uint32_t colour) {
    for (uint32_t dy = 0; dy <= r; dy++) {
        for (uint32_t dx = 0; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r) {
                fb_pixel(cx + dx, cy + dy, colour);
                fb_pixel(cx - dx, cy + dy, colour);
                fb_pixel(cx + dx, cy - dy, colour);
                fb_pixel(cx - dx, cy - dy, colour);
            }
        }
    }
}



/* ═══════════════════════════════════════════════════════════════════════════
 *  SHARED CHROME: sidebar + top bar
 * ═══════════════════════════════════════════════════════════════════════════ */
#define WZ_STEPS 3
static const char *WZ_STEP_NAMES[WZ_STEPS] = {
    "Hostname", "Network", "Confirm"
};

static void wz_draw_chrome(int current_step) {
    /* Full background */
    fb_clear(WZ_BG);

    /* ── Sidebar ── */
    fb_fill_rect(0, 0, WZ_SB_W, WZ_H, WZ_SIDEBAR_BG);

    /* SAM OS logo text (large, top of sidebar) */
    wz_puts_centre(WZ_SB_W / 2, 40, "SAM", WZ_ACCENT_LIT, WZ_SIDEBAR_BG, 3);
    wz_puts_centre(WZ_SB_W / 2, 40 + 3*WZ_CH_H + 6, "OS", WZ_ACCENT, WZ_SIDEBAR_BG, 3);

    /* Tagline */
    wz_puts_centre(WZ_SB_W / 2, 40 + 3*WZ_CH_H*2 + 18,
                   "Setup", WZ_TEXT_BODY, WZ_SIDEBAR_BG, 1);

    /* Divider */
    fb_fill_rect(WZ_SB_PAD, 160, WZ_SB_W - 2*WZ_SB_PAD, 1, WZ_PANEL_BORDER);

    /* Step list */
    uint32_t step_y = 200;
    for (int i = 0; i < WZ_STEPS; i++) {
        uint32_t dot_x = WZ_SB_PAD + 10;
        uint32_t dot_y = step_y + 8;

        uint32_t dot_col, text_col;
        if (i < current_step) {
            dot_col  = WZ_STEP_DONE;
            text_col = WZ_TEXT_BODY;
        } else if (i == current_step) {
            dot_col  = WZ_STEP_ACTIVE;
            text_col = WZ_TEXT_H2;
        } else {
            dot_col  = WZ_STEP_TODO;
            text_col = WZ_TEXT_DIM;
        }

        wz_draw_step_dot(dot_x, dot_y, 5, dot_col);

        /* Connector line between steps */
        if (i < WZ_STEPS - 1)
            fb_fill_rect(dot_x, dot_y + 6, 1, step_y + 48 - (dot_y + 6),
                         WZ_STEP_TODO);

        wz_puts(dot_x + 18, step_y, WZ_STEP_NAMES[i],
                text_col, WZ_SIDEBAR_BG, 1);

        step_y += 48;
    }

    /* Bottom: version */
    wz_puts(WZ_SB_PAD, WZ_H - 32, "SAM OS v0.1.0", WZ_TEXT_DIM, WZ_SIDEBAR_BG, 1);
    wz_puts(WZ_SB_PAD, WZ_H - 16, "Sprint 13", WZ_TEXT_DIM, WZ_SIDEBAR_BG, 1);

    /* Sidebar right edge */
    fb_fill_rect(WZ_SB_W, 0, 1, WZ_H, WZ_PANEL_BORDER);

    /* ── Top bar ── */
    fb_fill_rect(WZ_CP_X, 0, WZ_CP_W, 52, WZ_PANEL_BG);
    fb_fill_rect(WZ_CP_X, 52, WZ_CP_W, 1, WZ_ACCENT);

    /* Step indicator in top bar */
    {
        char step_str[24];
        /* build "Step X of Y" */
        step_str[0] = 'S'; step_str[1] = 't'; step_str[2] = 'e';
        step_str[3] = 'p'; step_str[4] = ' ';
        step_str[5] = (char)('1' + current_step);
        step_str[6] = ' '; step_str[7] = 'o'; step_str[8] = 'f';
        step_str[9] = ' '; step_str[10] = (char)('0' + WZ_STEPS);
        step_str[11] = '\0';
        wz_puts(WZ_W - WZ_CP_PAD - 88, 18, step_str,
                WZ_TEXT_BODY, WZ_PANEL_BG, 1);
    }
}

/* ── Content panel helper ────────────────────────────────────────────────── */
static void wz_content_clear(void) {
    fb_fill_rect(WZ_CP_X, 53, WZ_CP_W, WZ_H - 53, WZ_BG);
}

/* Draw a "[ Label ]" button */
static void wz_draw_button(uint32_t x, uint32_t y,
                            const char *label, int active) {
    uint32_t llen = 0; while (label[llen]) llen++;
    uint32_t bw = llen * WZ_CH_W * 2 + 40;
    uint32_t bh = 36;
    uint32_t bg = active ? WZ_ACCENT_LIT : WZ_BTN_BG;
    wz_rounded_rect(x, y, bw, bh, bg, WZ_ACCENT, 6);
    wz_puts(x + 20, y + (bh - WZ_CH_H) / 2, label,
            WZ_BTN_TEXT, bg, 2);
}

/* Draw an input field box, with current text and cursor */
static void wz_draw_input(uint32_t x, uint32_t y, uint32_t w,
                           const char *text, int text_len, int active) {
    uint32_t h = 42;
    uint32_t bg     = active ? WZ_INPUT_ACTIVE : WZ_INPUT_BG;
    uint32_t border = active ? WZ_ACCENT       : WZ_INPUT_BORDER;

    wz_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, border, 2);

    /* Text inside */
    uint32_t tx = x + 14;
    uint32_t ty = y + (h - WZ_CH_H) / 2;
    for (int i = 0; i < text_len; i++) {
        wz_putchar(tx, ty, text[i], WZ_TEXT_INPUT, bg, 1);
        tx += WZ_CH_W;
    }

    /* Blinking cursor (always on — no timer) */
    if (active && text_len < WZ_HOSTNAME_MAX)
        fb_fill_rect(tx + 2, ty + 2, 2, WZ_CH_H - 4, WZ_INPUT_CURSOR);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SCREEN 1 — WELCOME
 * ═══════════════════════════════════════════════════════════════════════════ */
static void wz_screen_welcome(void) {
    /* Draw chrome with no step highlighted yet */
    wz_draw_chrome(0);
    wz_content_clear();

    uint32_t cx  = WZ_CP_X + WZ_CP_W / 2;
    uint32_t top = 120;

    /* Big welcome heading */
    wz_puts_centre(cx, top, "Welcome to", WZ_TEXT_BODY, WZ_BG, 2);
    wz_puts_centre(cx, top + 40, "SAM OS", WZ_TEXT_H1, WZ_BG, 3);

    /* Subtitle */
    wz_puts_centre(cx, top + 110,
        "Let's set up your system in just a few steps.",
        WZ_TEXT_BODY, WZ_BG, 1);

    /* What we'll configure */
    uint32_t lx = WZ_CP_X + WZ_CP_PAD;
    uint32_t ly = top + 160;
    wz_puts(lx, ly,      "1.  Choose a hostname for this machine",
            WZ_TEXT_H2, WZ_BG, 1);
    wz_puts(lx, ly + 28, "2.  Connect to a WiFi network (if available)",
            WZ_TEXT_H2, WZ_BG, 1);
    wz_puts(lx, ly + 56, "3.  Confirm and apply your settings",
            WZ_TEXT_H2, WZ_BG, 1);

    /* Footer hint */
    wz_puts_centre(cx, WZ_H - 80,
        "Press  Enter  to begin",
        WZ_TEXT_KEY, WZ_BG, 1);

    /* Accent line above footer */
    fb_fill_rect(WZ_CP_X + WZ_CP_PAD, WZ_H - 95,
                 WZ_CP_W - 2*WZ_CP_PAD, 1, WZ_PANEL_BORDER);

    for (;;) { if (wz_wait_key() == SC_ENTER) break; }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SCREEN 2 — HOSTNAME
 * ═══════════════════════════════════════════════════════════════════════════ */
static void wz_draw_hostname_screen(wz_state_t *wz) {
    wz_draw_chrome(0);
    wz_content_clear();

    uint32_t lx  = WZ_CP_X + WZ_CP_PAD;
    uint32_t top = 90;

    wz_puts(lx, top, "Choose a hostname", WZ_TEXT_H1, WZ_BG, 2);
    wz_puts(lx, top + 42,
        "This name identifies your machine on the network.",
        WZ_TEXT_BODY, WZ_BG, 1);
    wz_puts(lx, top + 60,
        "Use letters, digits and hyphens only (max 32 chars).",
        WZ_TEXT_DIM, WZ_BG, 1);

    /* Input field */
    uint32_t fw = WZ_CP_W - 2*WZ_CP_PAD;
    wz_draw_input(lx, top + 100, fw, wz->hostname, wz->hostname_len, 1);

    /* Hint */
    wz_puts(lx, top + 160,
        "Default:  samOS-1   |   Backspace to delete",
        WZ_TEXT_DIM, WZ_BG, 1);

    /* Button */
    uint32_t bw = 10 * WZ_CH_W * 2 + 40;  /* "Continue" */
    wz_draw_button(WZ_W - WZ_CP_PAD - bw, WZ_H - 80, "Continue", 1);

    /* Footer hint */
    wz_puts(lx, WZ_H - 72,
        "Press  Enter  to continue",
        WZ_TEXT_KEY, WZ_BG, 1);

    fb_fill_rect(WZ_CP_X + WZ_CP_PAD, WZ_H - 90,
                 WZ_CP_W - 2*WZ_CP_PAD, 1, WZ_PANEL_BORDER);
}

static void wz_screen_hostname(wz_state_t *wz) {
    /* Pre-fill default */
    wz_strcpy(wz->hostname, "samOS-1", WZ_HOSTNAME_MAX);
    wz->hostname_len = 7;

    int shift = 0;
    wz_draw_hostname_screen(wz);

    for (;;) {
        uint8_t sc = wz_wait_key();

        if (sc == WZ_SC_E0) { wz_wait_key(); continue; }
        if (sc == WZ_SC_LSHIFT || sc == WZ_SC_RSHIFT) { shift = 1; continue; }

        if (sc == WZ_SC_BACK) {
            if (wz->hostname_len > 0) {
                wz->hostname_len--;
                wz->hostname[wz->hostname_len] = '\0';
                wz_draw_hostname_screen(wz);
            }
            continue;
        }
        if (sc == SC_ENTER) {
            if (wz->hostname_len == 0) {
                wz_strcpy(wz->hostname, "samOS-1", WZ_HOSTNAME_MAX);
                wz->hostname_len = 7;
            }
            break;
        }
        if (sc < 58) {
            char c = shift ? wz_sc_alpha_sh[sc] : wz_sc_alpha[sc];
            shift = 0;
            if (c && wz->hostname_len < WZ_HOSTNAME_MAX) {
                wz->hostname[wz->hostname_len++] = c;
                wz->hostname[wz->hostname_len]   = '\0';
                wz_draw_hostname_screen(wz);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SCREEN 3 — WIFI
 * ═══════════════════════════════════════════════════════════════════════════ */
static void wz_draw_wifi_screen(wz_state_t *wz, int phase) {
    wz_draw_chrome(1);
    wz_content_clear();

    uint32_t lx  = WZ_CP_X + WZ_CP_PAD;
    uint32_t top = 90;

    wz_puts(lx, top, "WiFi Network", WZ_TEXT_H1, WZ_BG, 2);

    if (!wz->wifi_present) {
        /* ── No adapter ── */
        wz_puts(lx, top + 50,
            "No wireless adapter detected on this system.",
            WZ_TEXT_WARN, WZ_BG, 1);
        wz_puts(lx, top + 70,
            "PCI wireless classes 0x0280-0x0282 not found.",
            WZ_TEXT_DIM, WZ_BG, 1);

        /* Info card */
        wz_rounded_rect(lx, top + 110,
                         WZ_CP_W - 2*WZ_CP_PAD, 140,
                         WZ_PANEL_BG, WZ_PANEL_BORDER, 8);
        wz_puts(lx + 20, top + 130, "What you can do:", WZ_TEXT_H2, WZ_PANEL_BG, 1);
        wz_puts(lx + 20, top + 155,
            "- Connect via Ethernet cable (works immediately)",
            WZ_TEXT_BODY, WZ_PANEL_BG, 1);
        wz_puts(lx + 20, top + 175,
            "- Plug in a USB WiFi adapter, then run: setup",
            WZ_TEXT_BODY, WZ_PANEL_BG, 1);
        wz_puts(lx + 20, top + 195,
            "- VirtualBox: enable USB WiFi passthrough in settings",
            WZ_TEXT_BODY, WZ_PANEL_BG, 1);

        wz_puts(lx, WZ_H - 72,
            "Press  Enter  to skip",
            WZ_TEXT_KEY, WZ_BG, 1);
        fb_fill_rect(WZ_CP_X + WZ_CP_PAD, WZ_H - 90,
                     WZ_CP_W - 2*WZ_CP_PAD, 1, WZ_PANEL_BORDER);
        return;
    }

    /* ── Adapter found ── */
    wz_puts(lx, top + 50,
        "Wireless adapter detected  (PCI class 0x0280).",
        WZ_TEXT_GOOD, WZ_BG, 1);

    if (phase == 0) {
        /* Scanning animation */
        wz_puts(lx, top + 90, "Scanning for networks...", WZ_TEXT_BODY, WZ_BG, 1);
        /* Animated dots */
        fb_fill_rect(lx, top + 120, 8, 8, WZ_ACCENT_LIT);
        fb_fill_rect(lx + 16, top + 120, 8, 8, WZ_PANEL_BORDER);
        fb_fill_rect(lx + 32, top + 120, 8, 8, WZ_PANEL_BORDER);
        return;
    }

    /* Phase 1: SSID input */
    wz_puts(lx, top + 80,
        "Enter your WiFi network name (SSID):",
        WZ_TEXT_BODY, WZ_BG, 1);
    wz_puts(lx, top + 100,
        "Full 802.11 association will complete after boot (Sprint 17).",
        WZ_TEXT_DIM, WZ_BG, 1);

    uint32_t fw = WZ_CP_W - 2*WZ_CP_PAD;
    wz_draw_input(lx, top + 130, fw, wz->ssid, wz->ssid_len, 1);

    wz_puts(lx, top + 190,
        "Backspace to delete  |  Tab to skip WiFi",
        WZ_TEXT_DIM, WZ_BG, 1);

    uint32_t bw = 10 * WZ_CH_W * 2 + 40;
    wz_draw_button(WZ_W - WZ_CP_PAD - bw, WZ_H - 80, "Continue", 1);

    wz_puts(lx, WZ_H - 72,
        "Enter = save  |  Tab = skip WiFi",
        WZ_TEXT_KEY, WZ_BG, 1);

    fb_fill_rect(WZ_CP_X + WZ_CP_PAD, WZ_H - 90,
                 WZ_CP_W - 2*WZ_CP_PAD, 1, WZ_PANEL_BORDER);
}

static void wz_screen_wifi(wz_state_t *wz, const sam_mcp_t *mcp) {
    wz->wifi_present = mcp->has_wireless;
    wz->ssid_len = 0;
    wz->ssid[0]  = '\0';
    wz->wifi_skip = 0;

    wz_draw_wifi_screen(wz, 0);

    if (!wz->wifi_present) {
        for (;;) { if (wz_wait_key() == SC_ENTER) break; }
        wz->wifi_skip = 1;
        return;
    }

    /* Brief scan animation pause (~200ms) */
    for (volatile uint32_t _d = 0; _d < 2000000; _d++)
        __asm__ volatile ("pause");
    wz_draw_wifi_screen(wz, 1);

    int shift = 0;
    for (;;) {
        uint8_t sc = wz_wait_key();

        if (sc == WZ_SC_E0) { wz_wait_key(); continue; }
        if (sc == WZ_SC_LSHIFT || sc == WZ_SC_RSHIFT) { shift = 1; continue; }

        if (sc == WZ_SC_BACK) {
            if (wz->ssid_len > 0) {
                wz->ssid_len--;
                wz->ssid[wz->ssid_len] = '\0';
                wz_draw_wifi_screen(wz, 1);
            }
            continue;
        }
        if (sc == SC_ENTER) break;
        if (sc == WZ_SC_TAB) {
            wz->wifi_skip = 1;
            wz->ssid_len  = 0;
            wz->ssid[0]   = '\0';
            break;
        }
        if (sc < 58) {
            char c = shift ? wz_sc_alpha_sh[sc] : wz_sc_alpha[sc];
            shift = 0;
            if (c && wz->ssid_len < WZ_SSID_MAX) {
                wz->ssid[wz->ssid_len++] = c;
                wz->ssid[wz->ssid_len]   = '\0';
                wz_draw_wifi_screen(wz, 1);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SCREEN 4 — SUMMARY / CONFIRM
 * ═══════════════════════════════════════════════════════════════════════════ */
static void wz_draw_uint(uint32_t x, uint32_t y,
                          uint32_t val, uint32_t fg, uint32_t bg) {
    char buf[12]; int i = 11; buf[i] = '\0';
    if (val == 0) buf[--i] = '0';
    else { while (val && i > 0) { buf[--i] = (char)('0' + val % 10); val /= 10; } }
    wz_puts(x, y, &buf[i], fg, bg, 1);
}

static void wz_screen_summary(const wz_state_t *wz, const sam_mcp_t *mcp) {
    wz_draw_chrome(2);
    wz_content_clear();

    uint32_t lx  = WZ_CP_X + WZ_CP_PAD;
    uint32_t top = 90;

    wz_puts(lx, top, "Confirm your settings", WZ_TEXT_H1, WZ_BG, 2);
    wz_puts(lx, top + 42,
        "Review the configuration below, then press Enter to apply.",
        WZ_TEXT_BODY, WZ_BG, 1);

    /* Settings card */
    uint32_t card_w = WZ_CP_W - 2*WZ_CP_PAD;
    uint32_t card_y = top + 80;
    uint32_t card_h = 110;
    wz_rounded_rect(lx, card_y, card_w, card_h, WZ_PANEL_BG, WZ_PANEL_BORDER, 8);

    uint32_t ky = card_y + 20;
    uint32_t kx = lx + 20;
    uint32_t vx = lx + 160;

    wz_puts(kx, ky,      "Hostname",  WZ_TEXT_DIM, WZ_PANEL_BG, 1);
    wz_puts(vx, ky,      wz->hostname, WZ_TEXT_H2, WZ_PANEL_BG, 1);

    wz_puts(kx, ky + 30, "WiFi SSID", WZ_TEXT_DIM, WZ_PANEL_BG, 1);
    if (!wz->wifi_present)
        wz_puts(vx, ky + 30, "No adapter", WZ_TEXT_WARN, WZ_PANEL_BG, 1);
    else if (wz->wifi_skip || wz->ssid_len == 0)
        wz_puts(vx, ky + 30, "Skipped",    WZ_TEXT_WARN, WZ_PANEL_BG, 1);
    else
        wz_puts(vx, ky + 30, wz->ssid,     WZ_TEXT_H2,  WZ_PANEL_BG, 1);

    wz_puts(kx, ky + 60, "Config at", WZ_TEXT_DIM, WZ_PANEL_BG, 1);
    wz_puts(vx, ky + 60, "0x7000 (physical)", WZ_TEXT_BODY, WZ_PANEL_BG, 1);

    /* Hardware card */
    uint32_t hw_y = card_y + card_h + 20;
    uint32_t hw_h = 130;
    wz_rounded_rect(lx, hw_y, card_w, hw_h, WZ_PANEL_BG, WZ_PANEL_BORDER, 8);

    wz_puts(kx, hw_y + 12, "Detected Hardware", WZ_TEXT_H2, WZ_PANEL_BG, 1);
    fb_fill_rect(lx + 20, hw_y + 32, card_w - 40, 1, WZ_PANEL_BORDER);

    uint32_t hy = hw_y + 44;
    wz_puts(kx, hy,      "CPU",  WZ_TEXT_DIM, WZ_PANEL_BG, 1);
    wz_puts(vx, hy,      mcp->cpu_brand, WZ_TEXT_BODY, WZ_PANEL_BG, 1);

    wz_puts(kx, hy + 22, "RAM",  WZ_TEXT_DIM, WZ_PANEL_BG, 1);
    wz_draw_uint(vx, hy + 22, mcp->ram_mb, WZ_TEXT_BODY, WZ_PANEL_BG);
    wz_puts(vx + 48, hy + 22, " MiB", WZ_TEXT_DIM, WZ_PANEL_BG, 1);

    wz_puts(kx, hy + 44, "WiFi", WZ_TEXT_DIM, WZ_PANEL_BG, 1);
    wz_puts(vx, hy + 44,
        mcp->has_wireless ? "Adapter present" : "Not detected",
        mcp->has_wireless ? WZ_TEXT_GOOD : WZ_TEXT_WARN,
        WZ_PANEL_BG, 1);

    wz_puts(kx, hy + 66, "GPU",  WZ_TEXT_DIM, WZ_PANEL_BG, 1);
    wz_puts(vx, hy + 66,
        mcp->gpu_class == MCP_GPU_INTEGRATED ? "Integrated" :
        mcp->gpu_class == MCP_GPU_DISCRETE   ? "Discrete"   :
        mcp->gpu_class == MCP_GPU_MULTI      ? "Multi-GPU"  : "None",
        WZ_TEXT_BODY, WZ_PANEL_BG, 1);

    /* Apply button + hint */
    uint32_t bw = 16 * WZ_CH_W * 2 + 40;
    wz_draw_button(WZ_W - WZ_CP_PAD - bw, WZ_H - 80, "Apply & Continue", 1);
    wz_puts(lx, WZ_H - 72,
        "Press  Enter  to apply and start kernel shell",
        WZ_TEXT_KEY, WZ_BG, 1);
    fb_fill_rect(WZ_CP_X + WZ_CP_PAD, WZ_H - 90,
                 WZ_CP_W - 2*WZ_CP_PAD, 1, WZ_PANEL_BORDER);

    for (;;) { if (wz_wait_key() == SC_ENTER) break; }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  WRITE CONFIG TO 0x7000
 * ═══════════════════════════════════════════════════════════════════════════ */
static void wz_write_config(const wz_state_t *wz, const sam_mcp_t *mcp) {
    domain_sizes_t sz = compute_domain_sizes(mcp->ram_mb, BOOT_MODE_GENERAL);

    volatile sam_boot_config_t *cfg =
        (volatile sam_boot_config_t *)(uintptr_t)SAM_BOOT_CFG_ADDR;

    cfg->magic        = SAM_BOOT_CFG_MAGIC;
    cfg->mode         = BOOT_MODE_GENERAL;
    cfg->ai_base      = sz.ai_base;
    cfg->ai_size      = sz.ai_size;
    cfg->game_base    = sz.game_base;
    cfg->game_size    = sz.game_size;
    cfg->general_base = sz.general_base;
    cfg->general_size = sz.general_size;
    cfg->ram_mb       = mcp->ram_mb;
    cfg->simd_level   = mcp->simd_level;
    cfg->gpu_class    = mcp->gpu_class;
    cfg->has_nvme     = mcp->has_nvme;
    cfg->has_wireless = mcp->has_wireless;

    const char *nm = "GENERAL";
    int ni = 0;
    while (nm[ni] && ni < 15) { cfg->mode_name[ni] = nm[ni]; ni++; }
    cfg->mode_name[ni] = '\0';

    /* Copy hostname from wizard state */
    int hi = 0;
    while (wz->hostname[hi] && hi < 31) { cfg->hostname[hi] = wz->hostname[hi]; hi++; }
    cfg->hostname[hi] = '\0';

    /* Copy SSID from wizard state */
    int si = 0;
    while (wz->ssid[si] && si < 31) { cfg->ssid[si] = wz->ssid[si]; si++; }
    cfg->ssid[si] = '\0';

    __asm__ volatile ("mfence" ::: "memory");
}

/* ── "Applying..." completion screen ──────────────────────────────────── */
static void wz_screen_done(const wz_state_t *wz) {
    wz_draw_chrome(2);
    wz_content_clear();

    uint32_t cx  = WZ_CP_X + WZ_CP_W / 2;
    uint32_t top = 80;

    wz_rounded_rect(cx - 40, top, 80, 80, WZ_STEP_DONE, WZ_STEP_DONE, 12);
    wz_puts_centre(cx, top + 28, "OK", WZ_TEXT_H1, WZ_STEP_DONE, 3);
    wz_puts_centre(cx, top + 110, "Configuration saved!", WZ_TEXT_GOOD, WZ_BG, 2);

    uint32_t lx = WZ_CP_X + WZ_CP_PAD;
    wz_puts(lx, top + 150, "Hostname : ", WZ_TEXT_DIM, WZ_BG, 1);
    wz_puts(lx + 96, top + 150, wz->hostname, WZ_TEXT_H2, WZ_BG, 1);

    wz_puts_centre(cx, top + 210, "Press Enter to continue", WZ_TEXT_KEY, WZ_BG, 1);

    /* NO drain. wz_wait_key() already ignores break codes (sc >= 0x80).
     * Any drain loop risks consuming the user's Enter make code (0x1C).
     * The summary screen's wz_wait_key() already consumed its own 0x1C;
     * only 0x9C (break) may be in-flight, and wz_wait_key filters it. */
    for (;;) { if (wz_wait_key() == SC_ENTER) break; }
    /* Framebuffer stays active — shell.h's sh_fb_putchar renders into it. */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  VGA TEXT FALLBACK (if framebuffer unavailable)
 * ═══════════════════════════════════════════════════════════════════════════ */
#define VGA_BASE ((volatile uint16_t *)0xB8000)
#define VGA_COLS 80
#define VGA_CELL(ch, fg, bg) ((uint16_t)(((bg)<<12)|((fg)<<8)|(uint8_t)(ch)))

static void wz_vga_puts(int row, int col, const char *s, uint8_t fg, uint8_t bg) {
    volatile uint16_t *v = VGA_BASE;
    while (*s && col < VGA_COLS)
        v[row * VGA_COLS + col++] = VGA_CELL(*s++, fg, bg);
}
static void wz_vga_cls(uint8_t bg) {
    volatile uint16_t *v = VGA_BASE;
    for (int i = 0; i < 80 * 25; i++)
        v[i] = VGA_CELL(' ', 0xF, bg);
}

static void wz_fallback_run(wz_state_t *wz, const sam_mcp_t *mcp) {
    /* Minimal VGA text fallback — same flow, no graphics */
    wz_vga_cls(0x1);
    wz_vga_puts(0, 0, " SAM OS Setup (text mode)          Press Enter to begin ", 0xF, 0x9);
    wz_vga_puts(4, 4, "Welcome to SAM OS Setup Wizard", 0xF, 0x1);
    wz_vga_puts(6, 4, "Hostname:", 0x7, 0x1);
    wz_vga_puts(7, 4, "> samOS-1", 0xF, 0x1);
    wz_vga_puts(9, 4, "Press Enter to use default hostname 'samOS-1'", 0x7, 0x1);
    for (;;) { if (wz_wait_key() == SC_ENTER) break; }
    wz_strcpy(wz->hostname, "samOS-1", WZ_HOSTNAME_MAX);
    wz->hostname_len = 7;

    wz_vga_cls(0x1);
    wz_vga_puts(0, 0, " SAM OS Setup - WiFi                                     ", 0xF, 0x9);
    if (mcp->has_wireless) {
        wz_vga_puts(4, 4, "Wireless adapter detected.", 0xA, 0x1);
        wz_vga_puts(6, 4, "Full 802.11 support coming in Sprint 17.", 0x7, 0x1);
        wz_vga_puts(8, 4, "Press Enter to continue.", 0x7, 0x1);
    } else {
        wz_vga_puts(4, 4, "No wireless adapter detected.", 0xE, 0x1);
        wz_vga_puts(6, 4, "Press Enter to skip.", 0x7, 0x1);
        wz->wifi_skip = 1;
    }
    for (;;) { if (wz_wait_key() == SC_ENTER) break; }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PUBLIC ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════ */
static void __attribute__((unused))
sam_wizard_run(const sam_mcp_t *mcp)
{
    wz_state_t wz;
    uint8_t *p = (uint8_t *)&wz;
    for (uint32_t i = 0; i < sizeof(wz_state_t); i++) p[i] = 0;

    if (!g_fb.ready) {
        /* No framebuffer — run VGA text fallback */
        wz_fallback_run(&wz, mcp);
        wz_write_config(&wz, mcp);
        return;
    }

    wz_screen_welcome();
    wz_screen_hostname(&wz);
    wz_screen_wifi(&wz, mcp);
    wz_screen_summary(&wz, mcp);
    wz_write_config(&wz, mcp);
    wz_screen_done(&wz);
}

#endif /* SAM_WIZARD_H */
