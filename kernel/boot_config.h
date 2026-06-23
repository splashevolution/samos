/*
 * SAM OS -- kernel/boot_config.h
 * Sprint 12: Boot Configuration — structs, domain math, config read/write
 *
 * The first-boot OOBE wizard lives in wizard.h (Sprint 12).
 * This file keeps only the shared data types and helper functions
 * that both wizard.h and kernel_main need.
 *
 * NO rendering code here. No TUI. No framebuffer.
 * Hardware-agnostic: all sizing derived from sam_mcp_t.
 */

#ifndef SAM_BOOT_CONFIG_H
#define SAM_BOOT_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include "mcp.h"

/* ── Fixed memory address for config handoff ────────────────────────── */
#define SAM_BOOT_CFG_ADDR   0x7000UL
#define SAM_BOOT_CFG_MAGIC  0xB007C0FFUL

/* ── Boot modes ─────────────────────────────────────────────────────── */
#define BOOT_MODE_AI       0
#define BOOT_MODE_GAME     1
#define BOOT_MODE_GENERAL  2
#define BOOT_MODE_COUNT    3   /* Sprint 12: removed CUSTOM — wizard handles it */

/* ── Domain layout struct (written to 0x7000, read by kernel_main) ─── */
typedef struct {
    uint32_t magic;
    uint8_t  mode;
    uint8_t  _pad[3];
    uint64_t ai_base;
    uint64_t game_base;
    uint64_t general_base;
    uint64_t ai_size;
    uint64_t game_size;
    uint64_t general_size;
    char     mode_name[16];
    uint32_t ram_mb;
    uint8_t  simd_level;
    uint8_t  gpu_class;
    uint8_t  has_nvme;
    uint8_t  has_wireless;
    char     hostname[32];   /* set by OOBE wizard */
    char     ssid[32];       /* set by OOBE wizard (WiFi network name) */
} sam_boot_config_t;

/* ── PS/2 scancodes (minimal set used by wizard + shell) ────────────── */
#define SC_UP      0x48
#define SC_DOWN    0x50
#ifndef SC_ENTER
#define SC_ENTER   0x1C
#endif

/* ─────────────────────────────────────────────────────────────────────
 *  Domain size policy  (hardware-agnostic, tiered by RAM)
 * ───────────────────────────────────────────────────────────────────── */
#define MiB(n) ((uint64_t)(n) * 1024ULL * 1024ULL)

typedef struct {
    uint64_t ai_size;
    uint64_t game_size;
    uint64_t general_size;
    uint64_t ai_base;
    uint64_t game_base;
    uint64_t general_base;
} domain_sizes_t;

#define DOMAIN_AI_BASE  0x0200000ULL

static domain_sizes_t __attribute__((unused))
compute_domain_sizes(uint32_t ram_mb, uint8_t mode) {
    domain_sizes_t s;

    /* Base sizes by tier */
    if (ram_mb < 512) {
        s.ai_size = MiB(64); s.game_size = MiB(32); s.general_size = MiB(32);
    } else if (ram_mb < 1024) {
        s.ai_size = MiB(128); s.game_size = MiB(64); s.general_size = MiB(64);
    } else if (ram_mb < 2048) {
        s.ai_size = MiB(256); s.game_size = MiB(96); s.general_size = MiB(96);
    } else {
        s.ai_size = MiB(512); s.game_size = MiB(128); s.general_size = MiB(128);
    }

    /* Mode boost: double the primary domain when RAM allows */
    if (ram_mb >= 512) {
        if      (mode == BOOT_MODE_AI)      s.ai_size      *= 2;
        else if (mode == BOOT_MODE_GAME)    s.game_size    *= 2;
        else if (mode == BOOT_MODE_GENERAL) s.general_size *= 2;
    }

    /* Compute bases sequentially from DOMAIN_AI_BASE */
    s.ai_base      = DOMAIN_AI_BASE;
    s.game_base    = s.ai_base   + s.ai_size;
    s.general_base = s.game_base + s.game_size;

    return s;
}

/* ─────────────────────────────────────────────────────────────────────
 *  Read config written by wizard (called from kernel_main)
 * ───────────────────────────────────────────────────────────────────── */
static int __attribute__((unused))
sam_boot_config_read(sam_boot_config_t *out) {
    const sam_boot_config_t *src =
        (const sam_boot_config_t *)(uintptr_t)SAM_BOOT_CFG_ADDR;
    if (src->magic != SAM_BOOT_CFG_MAGIC) return -1;
    /* manual copy — no libc memcpy */
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(sam_boot_config_t); i++) d[i] = s[i];
    return 0;
}

#endif /* SAM_BOOT_CONFIG_H */
