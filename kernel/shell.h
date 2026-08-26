/*
 * SAM OS -- kernel/shell.h
 * Sprint 12: Interactive Kernel Shell (updated)
 *
 * Bare-metal read-eval loop over PS/2 keyboard + VGA text output.
 * No heap, no libc, no filesystem. All state is stack-allocated.
 *
 * Sprint 12 additions:
 *   res     -- hardware resource summary (full MCP snapshot)
 *   setup   -- re-enter the first-boot configuration wizard
 *
 * Commands:
 *   help    -- list available commands
 *   cpu     -- CPU brand, SIMD level, RAM
 *   mem     -- domain layout (base, size)
 *   mode    -- current boot mode from 0x7000
 *   res     -- hardware resource summary
 *   setup   -- re-run first-boot wizard
 *   clear   -- clear the screen
 *   reboot  -- system reset via keyboard controller
 */

#ifndef SAM_SHELL_H
#define SAM_SHELL_H

#include <stdint.h>
#include <stddef.h>
#include "mcp.h"
/* boot_config.h and wizard.h already included by main.c before shell.h */

/* ── VGA helpers ──────────────────────────────────────────────────────────── */
#define SH_VGA_BASE  ((volatile uint16_t *)0xB8000)
#define SH_COLS      80
#define SH_ROWS      25
#define SH_WHITE     0x0F00
#define SH_GREEN     0x0A00
#define SH_CYAN      0x0B00
#define SH_YELLOW    0x0E00
#define SH_RED       0x0C00
#define SH_GREY      0x0700
#define SH_PROMPT    0x0B00

/* ── PS/2 scancodes (Set 1) ───────────────────────────────────────────────── */
#define SC_BACK    0x0E
#ifndef SC_ENTER
#define SC_ENTER   0x1C
#endif
#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_CAPS    0x3A
#define SC_E0      0xE0

/* Scancode-to-ASCII table (unshifted, US layout) */
static const char sc_ascii[58] = {
    0,  0,  '1','2','3','4','5','6','7','8','9','0','-','=', 0,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' '
};

static const char sc_ascii_shift[58] = {
    0,  0,  '!','@','#','$','%','^','&','*','(',')','_','+', 0,
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' '
};

/* ── Shell state ──────────────────────────────────────────────────────────── */
#define SH_CMD_MAX   79
#define SH_HIST_MAX   8

typedef struct {
    int  row, col;
    int  shift;
    int  caps;
    char cmd[SH_CMD_MAX + 1];
    int  cmd_len;
    char hist[SH_HIST_MAX][SH_CMD_MAX + 1];
    int  hist_count;
    int  hist_idx;
} sam_shell_t;

/* ── Low-level I/O ────────────────────────────────────────────────────────── */
static inline uint8_t sh_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1,%0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void sh_outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port));
}

/* ── Framebuffer terminal (used when g_fb.ready) ──────────────────────────── */
/*
 * When the wizard runs, VBE is active and VGA text at 0xB8000 is hidden.
 * Rather than trying to switch back to VGA text mode (unreliable on VBoxVGA),
 * we render the shell directly into the pixel framebuffer using fb.h's 8×16
 * bitmap font.  SH_COLS / SH_ROWS stay the same; pixel coords are computed
 * from (col * 8, row * 16).  Scroll = memmove the pixel rows up.
 */

/* Map VGA attribute nibble (foreground colour 0-15) → 32bpp XRGB */
static uint32_t sh_vga_to_rgb(uint16_t attr) {
    /* extract fg nibble (bits 8-11) */
    uint8_t fg = (uint8_t)((attr >> 8) & 0x0F);
    static const uint32_t pal[16] = {
        0x00000000, /* 0  black        */
        0x000000AA, /* 1  dark blue    */
        0x0000AA00, /* 2  dark green   */
        0x0000AAAA, /* 3  dark cyan    */
        0x00AA0000, /* 4  dark red     */
        0x00AA00AA, /* 5  dark magenta */
        0x00AA5500, /* 6  brown        */
        0x00AAAAAA, /* 7  light grey   */
        0x00555555, /* 8  dark grey    */
        0x005555FF, /* 9  blue         */
        0x0055FF55, /* 10 green        */
        0x0055FFFF, /* 11 cyan         */
        0x00FF5555, /* 12 red          */
        0x00FF55FF, /* 13 magenta      */
        0x00FFFF55, /* 14 yellow       */
        0x00FFFFFF, /* 15 white        */
    };
    return pal[fg & 0x0F];
}

static void sh_fb_scroll(void) {
    /* Scroll up by one text row (16 pixels) */
    uint32_t row_px = (uint32_t)SH_ROWS * 16 - 16;   /* pixel height - 1 row */
    uint32_t stride = g_fb.pitch_px;
    /* move rows 1..(SH_ROWS-1) up by 16 pixels */
    for (uint32_t y = 0; y < row_px; y++) {
        uint32_t *dst = g_fb.addr + y * stride;
        uint32_t *src = g_fb.addr + (y + 16) * stride;
        for (uint32_t x = 0; x < (uint32_t)SH_COLS * 8; x++)
            dst[x] = src[x];
    }
    /* clear bottom row */
    for (uint32_t y = row_px; y < row_px + 16; y++) {
        uint32_t *line = g_fb.addr + y * stride;
        for (uint32_t x = 0; x < (uint32_t)SH_COLS * 8; x++)
            line[x] = 0x00000000;
    }
}

static void sh_fb_putchar(sam_shell_t *sh, char c, uint16_t attr) {
    if (c == '\n') {
        sh->col = 0;
        if (++sh->row >= SH_ROWS) {
            sh_fb_scroll();
            sh->row = SH_ROWS - 1;
        }
        return;
    }
    uint32_t px = (uint32_t)sh->col * 8;
    uint32_t py = (uint32_t)sh->row * 16;
    uint32_t fg = sh_vga_to_rgb(attr);
    /* draw glyph directly into framebuffer */
    uint8_t idx = (uint8_t)c;
    if (idx < 32 || idx > 127) idx = 32;
    const uint8_t *glyph = FB_FONT_8X16[idx - 32];
    for (uint32_t row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        uint32_t *line = g_fb.addr + (py + row) * g_fb.pitch_px + px;
        for (uint32_t col2 = 0; col2 < 8; col2++)
            line[col2] = (bits & (0x80 >> col2)) ? fg : 0x00000000;
    }
    if (++sh->col >= SH_COLS) { sh->col = 0; sh_fb_putchar(sh, '\n', attr); }
}

/* ── VGA primitives (used when g_fb is not ready) ────────────────────────── */
static void sh_putchar(sam_shell_t *sh, char c, uint16_t attr) {
    if (g_fb.ready) { sh_fb_putchar(sh, c, attr); return; }
    volatile uint16_t *vga = SH_VGA_BASE;
    if (c == '\n') {
        sh->col = 0;
        if (++sh->row >= SH_ROWS) {
            for (int r = 0; r < SH_ROWS - 1; r++)
                for (int c2 = 0; c2 < SH_COLS; c2++)
                    vga[r * SH_COLS + c2] = vga[(r+1) * SH_COLS + c2];
            for (int c2 = 0; c2 < SH_COLS; c2++)
                vga[(SH_ROWS-1)*SH_COLS + c2] = SH_GREY | ' ';
            sh->row = SH_ROWS - 1;
        }
        return;
    }
    vga[sh->row * SH_COLS + sh->col] = attr | (uint8_t)c;
    if (++sh->col >= SH_COLS) { sh->col = 0; sh_putchar(sh, '\n', attr); }
}

static void sh_puts(sam_shell_t *sh, const char *s, uint16_t attr) {
    while (*s) sh_putchar(sh, *s++, attr);
}

static void sh_puthex(sam_shell_t *sh, uint64_t v, uint16_t attr) {
    const char *h = "0123456789ABCDEF";
    sh_puts(sh, "0x", attr);
    for (int i = 60; i >= 0; i -= 4)
        sh_putchar(sh, h[(v >> i) & 0xF], attr);
}

static void sh_putdec(sam_shell_t *sh, uint64_t v, uint16_t attr) {
    char buf[21]; int i = 20; buf[i] = '\0';
    if (v == 0) { sh_putchar(sh, '0', attr); return; }
    while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    sh_puts(sh, &buf[i], attr);
}

static void sh_clear(sam_shell_t *sh) {
    if (g_fb.ready) {
        fb_fill_rect(0, 0, (uint32_t)SH_COLS * 8, (uint32_t)SH_ROWS * 16, 0x00000000);
        sh->row = sh->col = 0;
        return;
    }
    volatile uint16_t *vga = SH_VGA_BASE;
    for (int i = 0; i < SH_COLS * SH_ROWS; i++) vga[i] = SH_GREY | ' ';
    sh->row = sh->col = 0;
}

static void sh_cursor_on(sam_shell_t *sh) {
    if (g_fb.ready) return;   /* no hardware cursor in FB mode */
    volatile uint16_t *vga = SH_VGA_BASE;
    uint16_t cell = vga[sh->row * SH_COLS + sh->col];
    vga[sh->row * SH_COLS + sh->col] = (uint16_t)((cell & 0x00FF) | 0x7000);
}
static void sh_cursor_off(sam_shell_t *sh) {
    if (g_fb.ready) return;
    volatile uint16_t *vga = SH_VGA_BASE;
    uint16_t cell = vga[sh->row * SH_COLS + sh->col];
    vga[sh->row * SH_COLS + sh->col] = (uint16_t)((cell & 0x00FF) | SH_GREY);
}

/* ── String helpers ───────────────────────────────────────────────────────── */
static int sh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
static int sh_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }

/* ── Prompt ───────────────────────────────────────────────────────────────── */
static void sh_prompt(sam_shell_t *sh) {
    sh_puts(sh, "\nSAM", SH_GREEN);
    sh_puts(sh, "> ", SH_PROMPT);
}

/* ── Redraw current command line ─────────────────────────────────────────── */
static void sh_redraw_cmd(sam_shell_t *sh) {
    volatile uint16_t *vga = SH_VGA_BASE;
    int base = sh->row * SH_COLS;
    for (int c = sh->col; c < SH_COLS; c++) vga[base + c] = SH_GREY | ' ';
    for (int i = 0; i < sh->cmd_len; i++)
        sh_putchar(sh, sh->cmd[i], SH_WHITE);
}

/* ── Command handlers ─────────────────────────────────────────────────────── */
static void cmd_help(sam_shell_t *sh) {
    sh_puts(sh, "\n  help    -- this list\n",              SH_CYAN);
    sh_puts(sh,   "  cpu     -- processor info\n",         SH_CYAN);
    sh_puts(sh,   "  mem     -- memory domain layout\n",   SH_CYAN);
    sh_puts(sh,   "  mode    -- current boot mode\n",      SH_CYAN);
    sh_puts(sh,   "  res     -- hardware resource summary\n", SH_CYAN);
    sh_puts(sh,   "  run <n> -- run ELF program from initrd (Sprint 19)\n", SH_CYAN);
    sh_puts(sh,   "  setup   -- re-run configuration wizard\n", SH_CYAN);
    sh_puts(sh,   "  clear   -- clear screen\n",           SH_CYAN);
    sh_puts(sh,   "  reboot  -- system reset\n",           SH_CYAN);
}

static void cmd_cpu(sam_shell_t *sh, const sam_mcp_t *mcp) {
    sh_puts(sh, "\n  CPU   : ", SH_WHITE);
    sh_puts(sh, mcp->cpu_brand, SH_CYAN);
    sh_puts(sh, "\n  SIMD  : ", SH_WHITE);
    sh_puts(sh,
        mcp->simd_level >= MCP_SIMD_AVX2  ? "AVX2" :
        mcp->simd_level >= MCP_SIMD_SSE42 ? "SSE4.2" : "SSE2/scalar",
        SH_GREEN);
    sh_puts(sh, "\n  RAM   : ", SH_WHITE);
    sh_putdec(sh, mcp->ram_mb, SH_CYAN);
    sh_puts(sh, " MB\n", SH_WHITE);
    sh_puts(sh, "  GPU   : ", SH_WHITE);
    sh_puts(sh,
        mcp->gpu_class == MCP_GPU_INTEGRATED ? "Integrated" :
        mcp->gpu_class == MCP_GPU_DISCRETE   ? "Discrete"   : "None/Unknown",
        SH_CYAN);
    sh_puts(sh, "\n", SH_WHITE);
}

static void cmd_mem(sam_shell_t *sh, const sam_boot_config_t *bcfg) {
    sh_puts(sh, "\n  Domain layout:\n", SH_WHITE);
    sh_puts(sh, "  AI      base=", SH_WHITE);
    sh_puthex(sh, bcfg->ai_base, SH_CYAN);
    sh_puts(sh, "  size=", SH_WHITE);
    sh_putdec(sh, bcfg->ai_size / (1024*1024), SH_CYAN);
    sh_puts(sh, " MiB\n", SH_WHITE);
    sh_puts(sh, "  GAME    base=", SH_WHITE);
    sh_puthex(sh, bcfg->game_base, SH_CYAN);
    sh_puts(sh, "  size=", SH_WHITE);
    sh_putdec(sh, bcfg->game_size / (1024*1024), SH_CYAN);
    sh_puts(sh, " MiB\n", SH_WHITE);
    sh_puts(sh, "  GENERAL base=", SH_WHITE);
    sh_puthex(sh, bcfg->general_base, SH_CYAN);
    sh_puts(sh, "  size=", SH_WHITE);
    sh_putdec(sh, bcfg->general_size / (1024*1024), SH_CYAN);
    sh_puts(sh, " MiB\n", SH_WHITE);
}

static void cmd_mode(sam_shell_t *sh, const sam_boot_config_t *bcfg) {
    sh_puts(sh, "\n  Boot mode : ", SH_WHITE);
    sh_puts(sh, bcfg->mode_name, SH_GREEN);
    sh_puts(sh, "\n  Config at : 0x7000\n", SH_GREY);
}

static void cmd_res(sam_shell_t *sh, const sam_mcp_t *mcp) {
    sh_puts(sh, "\n  -- Hardware Resource Summary --\n", SH_CYAN);
    sh_puts(sh, "  CPU    : ", SH_WHITE); sh_puts(sh, mcp->cpu_brand, SH_CYAN);
    sh_puts(sh, "\n  Cores  : ", SH_WHITE); sh_putdec(sh, mcp->cpu_cores,   SH_CYAN);
    sh_puts(sh, " phys / ",               SH_GREY);  sh_putdec(sh, mcp->cpu_threads, SH_CYAN);
    sh_puts(sh, " logical\n",             SH_GREY);
    sh_puts(sh, "  MHz    : ", SH_WHITE); sh_putdec(sh, mcp->cpu_mhz,    SH_CYAN);
    sh_puts(sh, "\n  SIMD   : ", SH_WHITE);
    sh_puts(sh,
        mcp->simd_level >= MCP_SIMD_AVX2  ? "AVX2" :
        mcp->simd_level >= MCP_SIMD_SSE42 ? "SSE4.2" : "SSE2/scalar",
        SH_GREEN);
    sh_puts(sh, "\n  RAM    : ", SH_WHITE); sh_putdec(sh, mcp->ram_mb, SH_CYAN);
    sh_puts(sh, " MiB\n",                 SH_WHITE);
    sh_puts(sh, "  GPU    : ", SH_WHITE);
    sh_puts(sh,
        mcp->gpu_class == MCP_GPU_INTEGRATED ? "Integrated" :
        mcp->gpu_class == MCP_GPU_DISCRETE   ? "Discrete"   :
        mcp->gpu_class == MCP_GPU_MULTI      ? "Multi-GPU"  : "None",
        SH_CYAN);
    sh_puts(sh, "\n  NVMe   : ", SH_WHITE); sh_puts(sh, mcp->has_nvme     ? "yes" : "no", SH_CYAN);
    sh_puts(sh, "   AHCI : ",   SH_WHITE); sh_puts(sh, mcp->has_ahci     ? "yes" : "no", SH_CYAN);
    sh_puts(sh, "\n  ETH    : ", SH_WHITE); sh_puts(sh, mcp->has_ethernet ? "yes" : "no", SH_CYAN);
    sh_puts(sh, "   WiFi : ",   SH_WHITE); sh_puts(sh, mcp->has_wireless ? "yes" : "no", SH_CYAN);
    sh_puts(sh, "\n  USB2   : ", SH_WHITE); sh_puts(sh, mcp->has_usb2     ? "yes" : "no", SH_CYAN);
    sh_puts(sh, "   USB3 : ",   SH_WHITE); sh_puts(sh, mcp->has_usb3     ? "yes" : "no", SH_CYAN);
    sh_puts(sh, "\n  Audio  : ", SH_WHITE); sh_puts(sh, mcp->has_hda_audio? "yes" : "no", SH_CYAN);
    sh_puts(sh, "\n", SH_WHITE);
}

static void cmd_setup(sam_shell_t *sh, const sam_mcp_t *mcp) {
    sh_puts(sh, "\n  Re-entering configuration wizard...\n", SH_YELLOW);
    sam_wizard_run(mcp);
    /* Wizard clears and redraws the screen; re-print shell header on return */
    sh_clear(sh);
    sh_puts(sh, "============================================================\n", SH_CYAN);
    sh_puts(sh, "  SAM OS  v0.1.0  |  Kernel Shell  |  Sprint 16\n", SH_WHITE);
    sh_puts(sh, "============================================================\n", SH_CYAN);
    sh_puts(sh, "  Wizard complete. Type 'help' for commands.\n", SH_GREY);
}

static void cmd_reboot(sam_shell_t *sh) {
    sh_puts(sh, "\n  Rebooting...\n", SH_YELLOW);
    while (sh_inb(0x64) & 0x02) __asm__ volatile ("pause");
    sh_outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_unknown(sam_shell_t *sh, const char *cmd) {
    sh_puts(sh, "\n  Unknown: ", SH_RED);
    sh_puts(sh, cmd, SH_RED);
    sh_puts(sh, "  (type 'help')\n", SH_GREY);
}

/* Sprint 19/22/23: `run <name> [args...]` — load an ELF from the initrd and
 * run it in ring 3 with argv; kernel runs all resident tasks round-robin.
 * Defined in syscall.h (included later in main.c's TU) — static fwd decls
 * here match that internal linkage, same pattern as the old sam_run_task. */
static int sam_task_create(const char *name, char **argv, int argc);
static int sam_task_run_loop(void);
extern uint64_t g_exit_code;   /* defined in syscall.h (main.c TU) */

static void cmd_run(sam_shell_t *sh, const char *args) {
    /* Tokenize args into av[] (in-place on a local copy) */
    char buf[SH_CMD_MAX + 1];
    int n = 0;
    while (args[n]) { buf[n] = args[n]; n++; }
    buf[n] = '\0';

    char *av[8];
    int ac = 0;
    int i2 = 0;
    while (buf[i2] && ac < 8) {
        while (buf[i2] == ' ') i2++;
        if (!buf[i2]) break;
        av[ac++] = &buf[i2];
        while (buf[i2] && buf[i2] != ' ') i2++;
        if (buf[i2]) { buf[i2] = '\0'; i2++; }
    }

    if (ac == 0) {
        sh_puts(sh, "\n  Usage: run <name> [args...]\n", SH_YELLOW);
        return;
    }

    sh_puts(sh, "\n  [run] creating task ", SH_CYAN);
    sh_puts(sh, av[0], SH_CYAN);
    for (int a = 1; a < ac; a++) {
        sh_puts(sh, " ", SH_CYAN);
        sh_puts(sh, av[a], SH_CYAN);
    }
    sh_puts(sh, "\n", SH_CYAN);

    int slot = sam_task_create(av[0], av, ac);
    if (slot < 0) {
        sh_puts(sh, "  [run] task failed to create\n", SH_RED);
        return;
    }

    sh_puts(sh, "  [run] task created in slot ", SH_GREEN);
    sh_putdec(sh, slot, SH_GREEN);
    sh_puts(sh, ", running...\n", SH_GREEN);

    int clean = sam_task_run_loop();
    if (clean > 0) {
        sh_puts(sh, "  [run] ", SH_GREEN);
        sh_putdec(sh, clean, SH_GREEN);
        sh_puts(sh, " task(s) exited cleanly, last exit code ", SH_GREEN);
        sh_putdec(sh, g_exit_code, SH_GREEN);
        sh_puts(sh, "\n", SH_GREEN);
    } else {
        sh_puts(sh, "  [run] all tasks faulted or failed\n", SH_YELLOW);
    }
}

/* ── Eval ─────────────────────────────────────────────────────────────────── */
static void sh_eval(sam_shell_t *sh, const sam_mcp_t *mcp,
                    const sam_boot_config_t *bcfg)
{
    int start = 0;
    while (sh->cmd[start] == ' ') start++;
    const char *cmd = &sh->cmd[start];

    /* Sprint 19: "run <name>" prefix command */
    if (cmd[0]=='r' && cmd[1]=='u' && cmd[2]=='n' &&
        (cmd[3]==' ' || cmd[3]=='\0')) {
        cmd_run(sh, &cmd[3]);
        /* save to history */
        if (sh->hist_count < SH_HIST_MAX) {
            for (int i = 0; i <= sh->cmd_len; i++)
                sh->hist[sh->hist_count][i] = sh->cmd[i];
            sh->hist_count++;
        }
        return;
    }

    if (sh_strlen(cmd) == 0)              { /* empty */ }
    else if (!sh_strcmp(cmd, "help"))   cmd_help(sh);
    else if (!sh_strcmp(cmd, "cpu"))    cmd_cpu(sh, mcp);
    else if (!sh_strcmp(cmd, "mem"))    cmd_mem(sh, bcfg);
    else if (!sh_strcmp(cmd, "mode"))   cmd_mode(sh, bcfg);
    else if (!sh_strcmp(cmd, "res"))    cmd_res(sh, mcp);
    else if (!sh_strcmp(cmd, "setup"))  { cmd_setup(sh, mcp); return; }
    else if (!sh_strcmp(cmd, "clear"))  { sh_clear(sh); return; }
    else if (!sh_strcmp(cmd, "reboot")) cmd_reboot(sh);
    else                                cmd_unknown(sh, cmd);

    /* save to history */
    if (sh_strlen(cmd) > 0 && sh->hist_count < SH_HIST_MAX) {
        for (int i = 0; i <= sh->cmd_len; i++)
            sh->hist[sh->hist_count][i] = sh->cmd[i];
        sh->hist_count++;
    }
}

/* ── Main shell entry point ───────────────────────────────────────────────── */
static void __attribute__((unused))
sam_shell_run(const sam_mcp_t *mcp, const sam_boot_config_t *bcfg)
{
    sam_shell_t sh = {0};
    sh.hist_idx = -1;

    sh_clear(&sh);
    sh_puts(&sh, "============================================================\n", SH_CYAN);
    sh_puts(&sh, "  SAM OS  v0.1.0  |  Kernel Shell  |  Sprint 16\n", SH_WHITE);
    sh_puts(&sh, "============================================================\n", SH_CYAN);
    sh_puts(&sh, "  Type 'help' for available commands.\n", SH_GREY);
    sh_prompt(&sh);

    uint32_t blink = 0;
    int cursor_visible = 0;

    for (;;) {
        /* blink cursor ~4 Hz */
        blink++;
        if (blink >= 500000) {
            blink = 0;
            if (cursor_visible) { sh_cursor_off(&sh); cursor_visible = 0; }
            else                { sh_cursor_on(&sh);  cursor_visible = 1; }
        }

        if (!(sh_inb(0x64) & 0x01)) continue;

        uint8_t sc = sh_inb(0x60);
        if (cursor_visible) { sh_cursor_off(&sh); cursor_visible = 0; }

        /* extended prefix (arrow keys) */
        if (sc == SC_E0) {
            while (!(sh_inb(0x64) & 0x01)) __asm__ volatile ("pause");
            uint8_t sc2 = sh_inb(0x60);
            if (sc2 == 0x48) {
                /* up arrow: history older */
                if (sh.hist_count > 0 && sh.hist_idx < sh.hist_count - 1) {
                    sh.hist_idx++;
                    int hi = sh.hist_count - 1 - sh.hist_idx;
                    sh.cmd_len = 0;
                    while (sh.hist[hi][sh.cmd_len]) {
                        sh.cmd[sh.cmd_len] = sh.hist[hi][sh.cmd_len];
                        sh.cmd_len++;
                    }
                    sh.cmd[sh.cmd_len] = '\0';
                    sh_redraw_cmd(&sh);
                }
            } else if (sc2 == 0x50) {
                /* down arrow: history newer */
                if (sh.hist_idx > 0) {
                    sh.hist_idx--;
                    int hi = sh.hist_count - 1 - sh.hist_idx;
                    sh.cmd_len = 0;
                    while (sh.hist[hi][sh.cmd_len]) {
                        sh.cmd[sh.cmd_len] = sh.hist[hi][sh.cmd_len];
                        sh.cmd_len++;
                    }
                    sh.cmd[sh.cmd_len] = '\0';
                    sh_redraw_cmd(&sh);
                } else if (sh.hist_idx == 0) {
                    sh.hist_idx = -1;
                    sh.cmd_len = 0;
                    sh.cmd[0] = '\0';
                    sh_redraw_cmd(&sh);
                }
            }
            continue;
        }

        /* break codes (key release) */
        if (sc & 0x80) {
            uint8_t make = sc & 0x7F;
            if (make == SC_LSHIFT || make == SC_RSHIFT) sh.shift = 0;
            continue;
        }

        if (sc == SC_LSHIFT || sc == SC_RSHIFT) { sh.shift = 1; continue; }
        if (sc == SC_CAPS) { sh.caps ^= 1; continue; }

        /* backspace */
        if (sc == SC_BACK) {
            if (sh.cmd_len > 0) {
                sh.cmd_len--;
                sh.cmd[sh.cmd_len] = '\0';
                if (sh.col > 0) sh.col--;
                if (g_fb.ready) {
                    fb_fill_rect((uint32_t)sh.col * 8,
                                 (uint32_t)sh.row * 16,
                                 8, 16, 0x00000000);
                } else {
                    volatile uint16_t *vga = SH_VGA_BASE;
                    vga[sh.row * SH_COLS + sh.col] = SH_GREY | ' ';
                }
            }
            continue;
        }

        /* enter */
        if (sc == SC_ENTER) {
            sh.cmd[sh.cmd_len] = '\0';
            sh_putchar(&sh, '\n', SH_WHITE);
            sh_eval(&sh, mcp, bcfg);
            sh.cmd_len = 0;
            sh.cmd[0] = '\0';
            sh.hist_idx = -1;
            sh_prompt(&sh);
            continue;
        }

        /* regular printable key */
        if (sc < 58) {
            char c = sh.shift ? sc_ascii_shift[sc] : sc_ascii[sc];
            if (sh.caps && c >= 'a' && c <= 'z') c -= 32;
            if (sh.caps && c >= 'A' && c <= 'Z' && !sh.shift) c += 32;
            if (c && sh.cmd_len < SH_CMD_MAX) {
                sh.cmd[sh.cmd_len++] = c;
                sh.cmd[sh.cmd_len]   = '\0';
                sh_putchar(&sh, c, SH_WHITE);
            }
        }
    }
}

#endif /* SAM_SHELL_H */
