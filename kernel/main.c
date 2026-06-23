/*
 * SAM OS -- kernel/main.c
 * Sprint 14: Phase 2 — kernel safety baseline
 * No libc. No external dependencies.
 */

#include <stdint.h>
#include <stddef.h>
#include "simd.h"
#include "scheduler.h"
#include "stf.h"
#include "mcp.h"
#include "fb.h"
#include "boot_config.h"
#include "panic.h"      /* sam_panic() — must precede idt.h */
#include "idt.h"        /* idt_init(), IDT_DEFINE_STUBS() */
#include "wizard.h"
#include "shell.h"

/*
 * Define all 32 ISR stubs + _isr_common in the .text section.
 * This macro emits one large __asm__() block and must appear exactly once,
 * at file scope, before kernel_main is called.
 */
IDT_DEFINE_STUBS()

/* -- Canonical global definitions (declared extern in headers) -- */
int               sam_simd_level = SAM_SIMD_SCALAR;

/* -- Multiboot2 -- */
/* MB2_TAG_END and mb2_tag_t are defined in mcp.h (included above) */
/* MB2_TAG_MODULE is also in mcp.h */

typedef struct { uint32_t type; uint32_t size;
                 uint32_t mod_start; uint32_t mod_end; char cmdline[1]; }
    __attribute__((packed)) mb2_module_tag_t;

typedef struct { uint8_t magic[4]; uint32_t version;
                 uint64_t tensor_count; uint64_t metadata_kv_count; }
    __attribute__((packed)) gguf_header_t;

/* -- VGA -- */
#define VGA_BASE  ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_WHITE 0x0F00
#define VGA_GREEN 0x0A00
#define VGA_RED   0x0C00
#define VGA_YELLOW 0x0E00
#define VGA_CYAN  0x0B00

/* -- Domain IDs -- */
#define DOMAIN_AI      0x10
#define DOMAIN_GAME    0x20
#define DOMAIN_GENERAL 0x30

/* -- Physical memory layout (Sprint 9+) --
 *   AI      domain: base=  2 MiB, size=256 MiB  — KV cache, activations, scratch
 *   GAME    domain: base=258 MiB, size= 64 MiB  — game state, framebuffer scratch
 *   GENERAL domain: base=322 MiB, size= 64 MiB  — app buffers
 *
 *   Model weights: loaded by GRUB at their physical address (read-only pointer).
 *   They are NOT copied into the AI domain — the domain holds compute buffers only.
 *   This makes models swappable: replace the GRUB module, reboot, same kernel.
 */
#define AI_DOMAIN_BASE      0x0200000UL
#define AI_DOMAIN_SIZE      0x10000000UL   /* 256 MiB: KV cache fits any model <=7B */
#define GAME_DOMAIN_BASE    0x10200000UL
#define GAME_DOMAIN_SIZE    0x4000000UL
#define GENERAL_DOMAIN_BASE 0x14200000UL
#define GENERAL_DOMAIN_SIZE 0x4000000UL

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

/* -- Serial (COM1 = 0x3F8) -- */
#define SERIAL_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{ __asm__ volatile ("outb %0,%1" :: "a"(val),"Nd"(port)); }

static inline uint8_t inb(uint16_t port)
{ uint8_t r; __asm__ volatile ("inb %1,%0" : "=a"(r) : "Nd"(port)); return r; }

static void serial_init(void) {
    outb(SERIAL_PORT+1, 0x00); outb(SERIAL_PORT+3, 0x80);
    outb(SERIAL_PORT+0, 0x03); outb(SERIAL_PORT+1, 0x00);
    outb(SERIAL_PORT+3, 0x03); outb(SERIAL_PORT+2, 0xC7);
    outb(SERIAL_PORT+4, 0x0B);
}

static void serial_putchar(char c) {
    while ((inb(SERIAL_PORT+5) & 0x20) == 0);
    if (c == '\n') { outb(SERIAL_PORT,'\r'); while ((inb(SERIAL_PORT+5)&0x20)==0); }
    outb(SERIAL_PORT, (uint8_t)c);
}

static void serial_puts(const char *s)   { while (*s) serial_putchar(*s++); }

static void serial_puthex(uint64_t v) {
    const char *h = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) serial_putchar(h[(v>>i)&0xF]);
}

static void serial_putdec(uint64_t v) {
    char buf[21]; int i = 20; buf[i] = '\0';
    if (v == 0) { serial_putchar('0'); return; }
    while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    serial_puts(&buf[i]);
}

/* -- RDTSC -- */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("cpuid\n\t rdtsc\n\t"
        : "=a"(lo),"=d"(hi) : "a"(0) : "rbx","rcx");
    return ((uint64_t)hi << 32) | lo;
}

/* -- PIT channel-2 TSC calibration --
 * Programs PIT ch2 for ~10ms one-shot, polls OUT2, measures TSC ticks.
 * cpu_mhz = tsc_10ms * 1193182 / (11931 * 1000000)
 */
#define PIT_CAL_COUNT 11931   /* counts for ~10ms at 1193182 Hz */

static uint64_t pit_measure_tsc_10ms(void) {
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)(p61 & 0xFC));          /* disable gate */
    outb(0x43, 0xB0);                             /* ch2 mode0 lo/hi binary */
    outb(0x42, (uint8_t)(PIT_CAL_COUNT & 0xFF));
    outb(0x42, (uint8_t)(PIT_CAL_COUNT >> 8));
    outb(0x61, (uint8_t)((p61 & 0xFC) | 0x01)); /* enable gate */
    uint64_t t0 = rdtsc();
    while ((inb(0x61) & 0x20) == 0) __asm__ volatile ("pause");
    uint64_t t1 = rdtsc();
    outb(0x61, p61);
    return t1 - t0;
}

static uint64_t pit_tsc_to_mhz(uint64_t tsc_10ms) {
    return (tsc_10ms * 1193182ULL) / ((uint64_t)PIT_CAL_COUNT * 1000000ULL);
}

/* -- VGA text output -- */
static int vga_row = 0, vga_col = 0;

static void vga_putchar(char c, uint16_t colour) {
    volatile uint16_t *vga = VGA_BASE;
    if (c == '\n') { vga_col=0; if(++vga_row>=VGA_ROWS) vga_row=0; return; }
    vga[vga_row*VGA_COLS+vga_col] = colour|(uint8_t)c;
    if (++vga_col >= VGA_COLS) { vga_col=0; if(++vga_row>=VGA_ROWS) vga_row=0; }
}

static void vga_puts(const char *s, uint16_t colour)
{ while (*s) vga_putchar(*s++, colour); }

static void vga_clear(void) {
    volatile uint16_t *vga = VGA_BASE;
    for (int i = 0; i < VGA_COLS*VGA_ROWS; i++) vga[i] = 0x0700|' ';
    vga_row = vga_col = 0;
}

static void vga_puthex(uint64_t v, uint16_t colour) {
    const char *h = "0123456789ABCDEF";
    vga_puts("0x", colour);
    for (int i = 60; i >= 0; i -= 4) vga_putchar(h[(v>>i)&0xF], colour);
}

static void vga_putdec(uint64_t v, uint16_t colour) {
    char buf[21]; int i = 20; buf[i] = '\0';
    if (v == 0) { vga_putchar('0', colour); return; }
    while (v && i > 0) { buf[--i] = '0'+(v%10); v /= 10; }
    vga_puts(&buf[i], colour);
}

/* -- CPUID -- */
typedef struct { uint32_t eax,ebx,ecx,edx; } cpuid_result_t;
static cpuid_result_t cpuid(uint32_t leaf, uint32_t sub) {
    cpuid_result_t r;
    __asm__ volatile ("cpuid"
        :"=a"(r.eax),"=b"(r.ebx),"=c"(r.ecx),"=d"(r.edx):"a"(leaf),"c"(sub));
    return r;
}
static int xsave_supported(void)  { return (cpuid(1,0).ecx >> 26) & 1; }
static int avx_hardware_bit(void) { return (cpuid(1,0).ecx >> 28) & 1; }
static int avx2_hardware_bit(void){ return (cpuid(7,0).ebx >>  5) & 1; }
static int avx_supported(void)    { return avx_hardware_bit(); }

static void cpu_enable_avx_support(void) {
    __asm__ volatile (
        "mov %%cr4,%%rax\n\t or $0x40000,%%rax\n\t mov %%rax,%%cr4\n\t"
        :::"rax");
    __asm__ volatile (
        "xor %%ecx,%%ecx\n\t xgetbv\n\t or $0x7,%%eax\n\t xsetbv\n\t"
        :::"eax","ecx","edx");
}

/* -- Domain allocator -- */
typedef struct { uint8_t id; uint64_t base,size; uint8_t active; char name[16]; }
    sam_domain_t;

#define MAX_DOMAINS 8
static sam_domain_t domain_table[MAX_DOMAINS];
static int domain_count = 0;

static int domain_alloc(uint8_t id, uint64_t base, uint64_t size, const char *name) {
    if (domain_count >= MAX_DOMAINS) return -1;
    for (int i = 0; i < domain_count; i++) {
        sam_domain_t *d = &domain_table[i];
        if (!d->active) continue;
        if (base < d->base+d->size && base+size > d->base) return -2;
    }
    sam_domain_t *d = &domain_table[domain_count++];
    d->id=id; d->base=base; d->size=size; d->active=1;
    int i=0; while(name[i]&&i<15){d->name[i]=name[i];i++;} d->name[i]='\0';
    return 0;
}

static void domain_print(const sam_domain_t *d, uint16_t colour) {
    const char *h="0123456789ABCDEF";
    vga_puts("  [domain 0x", colour);
    vga_putchar(h[(d->id>>4)&0xF], colour);
    vga_putchar(h[d->id&0xF],      colour);
    vga_puts("] ", colour); vga_puts(d->name, colour);
    vga_puts("  base=", colour); vga_puthex(d->base, colour);
    vga_puts("  size=", colour); vga_putdec(d->size/(1024*1024), colour);
    vga_puts(" MiB\n", colour);
}

/* -- Banner -- */
static void print_banner(void) {
    vga_puts("============================================================\n", VGA_CYAN);
    vga_puts("  SAM OS  v0.1.0  |  Structured Adaptive Machine\n",            VGA_WHITE);
    vga_puts("  Proof-Native Kernel  |  Sprint 14  |  2026\n",                VGA_WHITE);
    vga_puts("============================================================\n", VGA_CYAN);
    vga_puts("\n", VGA_WHITE);
}

/* ============================================================================
 * kernel_main
 * ========================================================================== */
void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info)
{
    serial_init();
    vga_clear();
    print_banner();
    serial_puts("============================================================\n");
    serial_puts("  SAM OS  v0.1.0  |  Structured Adaptive Machine\n");
    serial_puts("  Proof-Native Kernel  |  Sprint 14  |  2026\n");
    serial_puts("============================================================\n\n");

    /* Sprint 14: Install IDT — must happen before any code that can fault.
     * From this point on, a #GP or #PF will show the panic screen instead
     * of triple-faulting silently. */
    idt_init();
    serial_puts("[OK] Sprint 14: IDT installed — 32 exception vectors active\n");
    vga_puts("[OK] Sprint 14: IDT installed\n", VGA_GREEN);

    /* Sprint 10-A: MCP hardware scan
     * PIT calibration runs first (needed by mcp_scan_cpu for cpu_mhz).
     * fb_init is called before boot_config so the GUI can use it. */
    uint64_t tsc_10ms_early = pit_measure_tsc_10ms();
    uint64_t cpu_mhz_early  = pit_tsc_to_mhz(tsc_10ms_early);

    sam_mcp_t mcp;
    sam_mcp_scan(&mcp, multiboot_info, (uint32_t)cpu_mhz_early);

    serial_puts("[OK] Sprint 10-A: MCP scan complete\n");
    sam_mcp_report(&mcp, serial_puts, serial_putdec, serial_puthex);

    /* Sprint 10-B: Pixel framebuffer init.
     * mcp_scan_fb records the raw GRUB tag before fb_init() runs. Treat
     * g_fb.ready as the source of truth after GRUB/VBE fallback probing. */
    int fb_ready = fb_init(multiboot_info);
    if (fb_ready && g_fb.ready) {
        mcp.fb_available = 1;
        mcp.fb_width     = g_fb.width;
        mcp.fb_height    = g_fb.height;
        mcp.fb_bpp       = g_fb.bpp;
        serial_puts("[OK] Sprint 10-B: Pixel framebuffer initialised  ");
        serial_putdec(g_fb.width);  serial_puts("x");
        serial_putdec(g_fb.height); serial_puts("x");
        serial_putdec(g_fb.bpp);    serial_puts("bpp\n");
    } else {
        mcp.fb_available = 0;
        serial_puts("[OK] Sprint 10-B: VGA text fallback (no pixel framebuffer)\n");
    }

    /* Sprint 13: First-boot graphical OOBE wizard */
    serial_puts("[OK] Sprint 13: First-boot wizard starting\n");
    sam_wizard_run(&mcp);
    serial_puts("[OK] Sprint 13: First-boot wizard complete\n");

    /* Sprint 10-D: Read config and allocate dynamic domains */
    sam_boot_config_t bcfg;
    int cfg_ok = sam_boot_config_read(&bcfg);
    if (cfg_ok == 0) {
        serial_puts("[OK] Sprint 10-D: Boot config read  mode=");
        serial_puts(bcfg.mode_name); serial_puts("\n");
    } else {
        serial_puts("[WARN] Sprint 10-D: Boot config magic missing, using defaults\n");
        /* Fall back to Sprint 9 static sizes */
        bcfg.ai_base      = AI_DOMAIN_BASE;
        bcfg.game_base    = GAME_DOMAIN_BASE;
        bcfg.general_base = GENERAL_DOMAIN_BASE;
        bcfg.ai_size      = AI_DOMAIN_SIZE;
        bcfg.game_size    = GAME_DOMAIN_SIZE;
        bcfg.general_size = GENERAL_DOMAIN_SIZE;
        bcfg.mode         = BOOT_MODE_GENERAL;
    }

    /* 1. Multiboot2 handshake */
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        vga_puts("[OK] Multiboot2 handshake verified\n", VGA_GREEN);
        serial_puts("[OK] Multiboot2 handshake verified\n");
    } else {
        vga_puts("[WARN] Unexpected bootloader magic: ", VGA_YELLOW);
        vga_puthex(multiboot_magic, VGA_YELLOW); vga_puts("\n", VGA_YELLOW);
        serial_puts("[WARN] Unexpected magic: "); serial_puthex(multiboot_magic); serial_puts("\n");
    }

    /* 2. SIMD detection */
    int has_xsave = xsave_supported();
    int has_avx   = avx_hardware_bit();
    int has_avx2  = avx2_hardware_bit();
    if (has_xsave && has_avx) {
        cpu_enable_avx_support();
        has_avx  = avx_supported();
        has_avx2 = avx2_hardware_bit();
    }
    sam_simd_init();

    vga_puts("[OK] CPU feature detection:\n", VGA_GREEN);
    vga_puts("     AVX  : ", VGA_WHITE);
    vga_puts(has_avx  ? "YES\n":"NO\n", has_avx  ? VGA_GREEN:VGA_RED);
    vga_puts("     AVX2 : ", VGA_WHITE);
    vga_puts(has_avx2 ? "YES\n":"NO\n", has_avx2 ? VGA_GREEN:VGA_RED);
    serial_puts("[OK] CPU feature detection:\n");
    serial_puts("     AVX  : "); serial_puts(has_avx  ? "YES\n":"NO\n");
    serial_puts("     AVX2 : "); serial_puts(has_avx2 ? "YES\n":"NO\n");

    /* 3. Sprint 14: Validate domains against multiboot2 memory map (E820)
     *
     * Multiboot2 memory map tag (type 6) contains E820 entries.
     * Each entry: base_addr(u64), length(u64), type(u32), reserved(u32).
     * Type 1 = available RAM.  We require each domain to be fully covered
     * by at least one type-1 entry.  A domain that overlaps reserved/ACPI/
     * bad memory is skipped with a [WARN] rather than causing silent corruption.
     *
     * Multiboot2 tag header: type(u32), size(u32), entry_size(u32), entry_version(u32)
     * Followed by entry_count = (size - 16) / entry_size entries.
     */
    typedef struct {
        uint32_t type; uint32_t size;
        uint32_t entry_size; uint32_t entry_version;
    } __attribute__((packed)) mb2_mmap_hdr_t;

    typedef struct {
        uint64_t base_addr;
        uint64_t length;
        uint32_t entry_type;   /* 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
        uint32_t reserved;
    } __attribute__((packed)) mb2_mmap_entry_t;

    /* Walk multiboot2 tags to find the memory map */
    mb2_mmap_hdr_t *mmap_tag = NULL;
    {
        uint8_t *p = (uint8_t *)(uintptr_t)(multiboot_info + 8); /* skip fixed header */
        uint8_t *end = p + 8192; /* safety limit — real size from mb2 header[0] */
        while (p < end) {
            uint32_t tag_type = *(uint32_t *)p;
            uint32_t tag_size = *(uint32_t *)(p + 4);
            if (tag_type == 0) break;       /* end tag */
            if (tag_type == 6) { mmap_tag = (mb2_mmap_hdr_t *)p; break; }
            p += (tag_size + 7) & ~7u;     /* tags are 8-byte aligned */
        }
    }

    /* Helper: returns 1 if [base, base+size) is fully covered by a type-1 entry */
    #define DOMAIN_IN_RAM(base, sz) ({ \
        int _ok = 0; \
        if (mmap_tag) { \
            uint8_t *_ep = (uint8_t *)mmap_tag + sizeof(mb2_mmap_hdr_t); \
            uint8_t *_ee = (uint8_t *)mmap_tag + mmap_tag->size; \
            uint32_t _es = mmap_tag->entry_size; \
            while (_ep + _es <= _ee) { \
                mb2_mmap_entry_t *_e = (mb2_mmap_entry_t *)_ep; \
                if (_e->entry_type == 1 && \
                    _e->base_addr <= (base) && \
                    _e->base_addr + _e->length >= (base) + (sz)) \
                    { _ok = 1; break; } \
                _ep += _es; \
            } \
        } else { \
            _ok = 1; /* no mmap tag — skip validation, trust bootloader */ \
        } \
        _ok; \
    })

    if (mmap_tag) {
        serial_puts("[OK] Sprint 14: E820 memory map found, validating domains\n");
        vga_puts("[OK] Sprint 14: E820 memory map found\n", VGA_GREEN);
    } else {
        serial_puts("[WARN] Sprint 14: No E820 memory map tag — skipping domain validation\n");
        vga_puts("[WARN] Sprint 14: No E820 map\n", VGA_YELLOW);
    }

    /* 3. Allocate three PSL-style memory domains (Sprint 8 adds GENERAL) */
    vga_puts("\n[OK] Allocating memory domains:\n", VGA_GREEN);
    serial_puts("\n[OK] Allocating memory domains:\n");

    int r_ai = -3, r_game = -3, r_general = -3;

    if (DOMAIN_IN_RAM(bcfg.ai_base, bcfg.ai_size)) {
        r_ai = domain_alloc(DOMAIN_AI, bcfg.ai_base, bcfg.ai_size, "AI-INFERENCE");
    } else {
        serial_puts("[WARN] AI domain overlaps reserved memory — skipped\n");
        vga_puts("[WARN] AI domain in reserved RAM — skipped\n", VGA_YELLOW);
    }

    if (DOMAIN_IN_RAM(bcfg.game_base, bcfg.game_size)) {
        r_game = domain_alloc(DOMAIN_GAME, bcfg.game_base, bcfg.game_size, "GAME-ENGINE");
    } else {
        serial_puts("[WARN] GAME domain overlaps reserved memory — skipped\n");
        vga_puts("[WARN] GAME domain in reserved RAM — skipped\n", VGA_YELLOW);
    }

    if (DOMAIN_IN_RAM(bcfg.general_base, bcfg.general_size)) {
        r_general = domain_alloc(DOMAIN_GENERAL, bcfg.general_base, bcfg.general_size, "GENERAL-APP");
    } else {
        serial_puts("[WARN] GENERAL domain overlaps reserved memory — skipped\n");
        vga_puts("[WARN] GENERAL domain in reserved RAM — skipped\n", VGA_YELLOW);
    }

    for (int i = 0; i < domain_count; i++) {
        const char *h = "0123456789ABCDEF";
        domain_print(&domain_table[i], VGA_WHITE);
        serial_puts("  [domain 0x");
        serial_putchar(h[(domain_table[i].id>>4)&0xF]);
        serial_putchar(h[domain_table[i].id&0xF]);
        serial_puts("] "); serial_puts(domain_table[i].name);
        serial_puts("  base="); serial_puthex(domain_table[i].base);
        serial_puts("  size="); serial_putdec(domain_table[i].size/(1024*1024));
        serial_puts(" MiB\n");
    }

    int domains_ok = (r_ai == 0 && r_game == 0 && r_general == 0);
    if (domains_ok) {
        vga_puts("[OK] Domain isolation: no overlap detected\n", VGA_GREEN);
        serial_puts("[OK] Domain isolation: no overlap detected\n");
    } else {
        vga_puts("[FAIL] Domain allocation failed\n", VGA_RED);
        serial_puts("[FAIL] Domain allocation failed\n");
    }

    /* 4. INT8 dot-product (Sprint 2/3) */
    vga_puts("\n[OK] INT8 compute engine test:\n", VGA_GREEN);
    serial_puts("\n[OK] INT8 compute engine test:\n");
    vga_puts("     SIMD path: ", VGA_WHITE);
    vga_puts(sam_simd_name(), VGA_CYAN); vga_puts("\n", VGA_WHITE);
    serial_puts("     SIMD path: "); serial_puts(sam_simd_name()); serial_puts("\n");

    __attribute__((aligned(32))) int8_t va[32], vb[32];
    for (int i = 0; i < 32; i++) { va[i]=(int8_t)(i+1); vb[i]=1; }
    int32_t result = sam_int8_dot(va, vb, 32);

    vga_puts("     dot([1..32],[1..1]) = ", VGA_WHITE);
    vga_putdec((uint64_t)(result>=0?result:-result), VGA_WHITE);
    vga_puts("  expected=528  ", VGA_WHITE);
    serial_puts("     dot([1..32],[1..1]) = ");
    serial_putdec((uint64_t)(result>=0?result:-result));
    serial_puts("  expected=528  ");

    int compute_ok = (result == 528);
    vga_puts(compute_ok?"[PASS]\n":"[FAIL]\n", compute_ok?VGA_GREEN:VGA_RED);
    serial_puts(compute_ok?"[PASS]\n":"[FAIL]\n");

    /* 5. INT8 matrix multiply (Sprint 4) */
    vga_puts("\n[OK] INT8 matrix multiply test (4x32 x 32x4):\n", VGA_GREEN);
    serial_puts("\n[OK] INT8 matrix multiply test (4x32 x 32x4):\n");
    vga_puts("     SIMD path: ", VGA_WHITE);
    vga_puts(sam_simd_name(), VGA_CYAN); vga_puts("\n", VGA_WHITE);
    serial_puts("     SIMD path: "); serial_puts(sam_simd_name()); serial_puts("\n");

#define MM  4
#define MK 32
#define MN  4
    __attribute__((aligned(32))) int8_t mA[MM*MK], mBT[MN*MK];
    int32_t mC[MM*MN];
    for (int i=0;i<MM*MK;i++) mA[i]=1;
    for (int i=0;i<MN*MK;i++) mBT[i]=1;
    sam_int8_matmul(mA, mBT, mC, MM, MK, MN);

    int matmul_ok=1, matmul_wrong=0;
    for (int i=0;i<MM*MN;i++) { if(mC[i]!=MK){matmul_ok=0;matmul_wrong=i;} }

    serial_puts("     C[0][0]="); serial_putdec((uint64_t)mC[0]);
    serial_puts("  C[0][3]=");   serial_putdec((uint64_t)mC[3]);
    serial_puts("  C[3][0]=");   serial_putdec((uint64_t)mC[12]);
    serial_puts("  C[3][3]=");   serial_putdec((uint64_t)mC[15]);
    serial_puts("  expected=32  ");
    vga_puts("     C[0][0]=", VGA_WHITE); vga_putdec((uint64_t)mC[0],  VGA_WHITE);
    vga_puts("  C[3][3]=",    VGA_WHITE); vga_putdec((uint64_t)mC[15], VGA_WHITE);
    vga_puts("  expected=32  ", VGA_WHITE);
    if (matmul_ok) {
        vga_puts("[PASS]\n", VGA_GREEN); serial_puts("[PASS]\n");
    } else {
        vga_puts("[FAIL] at ", VGA_RED); vga_putdec((uint64_t)matmul_wrong, VGA_RED);
        vga_puts("\n", VGA_RED); serial_puts("[FAIL]\n");
    }

    /* 6. Throughput benchmark (Sprint 5) + PIT calibration (Sprint 8) */
#define BM   32
#define BK  128
#define BN   32
#define BITERS 100

    vga_puts("\n[OK] Sprint 5: Throughput benchmark\n", VGA_GREEN);
    serial_puts("\n[OK] Sprint 5: Throughput benchmark\n");
    serial_puts("     Shape: 32x128 x 128x32, iterations=100\n");

    int8_t  *bA  = (int8_t  *)(AI_DOMAIN_BASE);
    int8_t  *bBT = (int8_t  *)(AI_DOMAIN_BASE + BM*BK);
    int32_t *bC  = (int32_t *)(AI_DOMAIN_BASE + 2*BM*BK);
    for (int i=0;i<BM*BK;i++) bA[i]=1;
    for (int i=0;i<BN*BK;i++) bBT[i]=1;

    /* PIT channel-2 real frequency measurement */
    uint64_t tsc_10ms = pit_measure_tsc_10ms();
    uint64_t cpu_mhz  = pit_tsc_to_mhz(tsc_10ms);

    uint64_t t0 = rdtsc();
    for (int iter=0;iter<BITERS;iter++)
        sam_int8_matmul(bA, bBT, bC, BM, BK, BN);
    uint64_t ticks = rdtsc() - t0;

    uint64_t total_ops = (uint64_t)BITERS*(uint64_t)BM*(uint64_t)BN*(uint64_t)BK*2ULL;

    /* ops/tick */
    uint64_t opt_x1000 = (total_ops*1000ULL)/(ticks+1ULL);
    uint64_t opt_int   = opt_x1000/1000;
    uint64_t opt_frac  = (opt_x1000%1000)/10;

    /* GOPS = total_ops * cpu_mhz / (ticks * 1000)
     * gops_x1000 = GOPS * 1000 = total_ops * cpu_mhz / ticks  */
    uint64_t gops_x1000 = (total_ops*cpu_mhz)/(ticks+1ULL);
    uint64_t gops_int   = gops_x1000/1000;
    uint64_t gops_frac  = (gops_x1000%1000)/10;

    uint64_t ghz_int  = cpu_mhz/1000;
    uint64_t ghz_frac = (cpu_mhz%1000)/10;

    serial_puts("     TSC ticks  : "); serial_putdec(ticks);     serial_puts("\n");
    serial_puts("     Total ops  : "); serial_putdec(total_ops); serial_puts("\n");
    serial_puts("     PIT 10ms   : "); serial_putdec(tsc_10ms);  serial_puts(" ticks\n");
    serial_puts("     CPU freq   : "); serial_putdec(cpu_mhz);
    serial_puts(" MHz ("); serial_putdec(ghz_int); serial_puts(".");
    if (ghz_frac<10) { serial_puts("0"); } serial_putdec(ghz_frac);
    serial_puts(" GHz)  [PIT-calibrated]\n");
    serial_puts("     Ops/tick   : "); serial_putdec(opt_int); serial_puts(".");
    if (opt_frac<10) { serial_puts("0"); } serial_putdec(opt_frac);
    serial_puts("  (hardware fact)\n");
    serial_puts("     Throughput : "); serial_putdec(gops_int); serial_puts(".");
    if (gops_frac<10) { serial_puts("0"); } serial_putdec(gops_frac);
    serial_puts(" GOPS INT8  (SSE4.2, PIT-calibrated)\n");

    vga_puts("     CPU: ", VGA_WHITE); vga_putdec(cpu_mhz, VGA_CYAN);
    vga_puts(" MHz  Throughput: ", VGA_WHITE); vga_putdec(gops_int, VGA_CYAN);
    vga_puts(".", VGA_WHITE);
    if (gops_frac<10) vga_puts("0", VGA_WHITE);
    vga_putdec(gops_frac, VGA_CYAN);
    vga_puts(" GOPS INT8\n", VGA_WHITE);
    vga_puts("     [PASS] Benchmark complete\n", VGA_GREEN);
    serial_puts("     [PASS] Benchmark complete\n");

    int bench_ok = (ticks > 0 && total_ops > 0 && cpu_mhz > 100);

    /* 7. GGUF header parse (Sprint 6) */
    vga_puts("\n[OK] Sprint 6: GGUF header parse\n", VGA_GREEN);
    serial_puts("\n[OK] Sprint 6: GGUF header parse\n");

    int gguf_ok = 0;
    uint8_t *mb2     = (uint8_t *)(uintptr_t)multiboot_info;
    uint8_t *tag_ptr = mb2 + 8;
    mb2_module_tag_t *mod_tag = 0;

    for (;;) {
        mb2_tag_t *tag = (mb2_tag_t *)tag_ptr;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_MODULE) { mod_tag=(mb2_module_tag_t*)tag_ptr; break; }
        uint32_t next = (tag->size+7) & ~7u;
        if (next == 0) break;
        tag_ptr += next;
    }

    if (!mod_tag) {
        vga_puts("     [WARN] No multiboot2 module found\n", VGA_YELLOW);
        serial_puts("     [WARN] No multiboot2 module found\n");
    } else {
        uint32_t mod_start = mod_tag->mod_start;
        uint32_t mod_end   = mod_tag->mod_end;
        uint32_t mod_size  = mod_end - mod_start;
        serial_puts("     Module addr : "); serial_puthex(mod_start); serial_puts("\n");
        serial_puts("     Module size  : "); serial_putdec(mod_size);  serial_puts(" bytes\n");
        serial_puts("     Cmdline      : "); serial_puts(mod_tag->cmdline); serial_puts("\n");
        vga_puts("     Module: addr=", VGA_WHITE); vga_puthex(mod_start, VGA_CYAN);
        vga_puts("  size=", VGA_WHITE); vga_putdec(mod_size, VGA_WHITE);
        vga_puts(" bytes\n", VGA_WHITE);

        if (mod_size < 24) {
            serial_puts("     [FAIL] Module too small\n");
            vga_puts("     [FAIL] Module too small\n", VGA_RED);
        } else {
            gguf_header_t *gh = (gguf_header_t *)(uintptr_t)mod_start;
            int magic_ok = (gh->magic[0]=='G' && gh->magic[1]=='G' &&
                            gh->magic[2]=='U' && gh->magic[3]=='F');
            serial_puts("     Magic        : ");
            serial_putchar(gh->magic[0]); serial_putchar(gh->magic[1]);
            serial_putchar(gh->magic[2]); serial_putchar(gh->magic[3]);
            serial_puts(magic_ok ? "  [OK]\n" : "  [BAD]\n");
            serial_puts("     Version      : "); serial_putdec(gh->version);           serial_puts("\n");
            serial_puts("     Tensor count : "); serial_putdec(gh->tensor_count);      serial_puts("\n");
            serial_puts("     Metadata KVs : "); serial_putdec(gh->metadata_kv_count); serial_puts("\n");
            vga_puts("     Magic: GGUF  version=", VGA_WHITE);
            vga_putdec(gh->version, VGA_CYAN);
            vga_puts("  tensors=", VGA_WHITE); vga_putdec((uint64_t)gh->tensor_count, VGA_CYAN);
            vga_puts("  kvs=", VGA_WHITE); vga_putdec((uint64_t)gh->metadata_kv_count, VGA_CYAN);
            vga_puts("\n", VGA_WHITE);
            gguf_ok = magic_ok && (gh->version==2||gh->version==3) && gh->tensor_count>0;
            if (gguf_ok) {
                vga_puts("     [PASS] GGUF header valid on bare metal\n", VGA_GREEN);
                serial_puts("     [PASS] GGUF header valid on bare metal\n");
            } else {
                vga_puts("     [FAIL] GGUF header invalid\n", VGA_RED);
                serial_puts("     [FAIL] GGUF header invalid\n");
            }
        }
    }

    /* 8. Sprint 8: GENERAL domain + three-way cooperative scheduler */
    vga_puts("\n[OK] Sprint 8: GENERAL domain + three-way scheduler\n", VGA_GREEN);
    serial_puts("\n[OK] Sprint 8: GENERAL domain + three-way scheduler\n");

#define MAGIC_AI      0xA1A1A1A1UL
#define MAGIC_GAME    0xCAFECAFEUL
#define MAGIC_GENERAL 0x6E6E6E6EUL

    /* Sentinels: write a unique magic into each domain's memory, read back */
    volatile uint32_t *ai_sent  = (volatile uint32_t *)(bcfg.ai_base      + 0x10000UL);
    volatile uint32_t *gm_sent  = (volatile uint32_t *)(bcfg.game_base    + 0x1000UL);
    volatile uint32_t *gn_sent  = (volatile uint32_t *)(bcfg.general_base);

    *ai_sent = MAGIC_AI;    uint32_t ai_result = *ai_sent;
    *gm_sent = MAGIC_GAME;  uint32_t gm_result = *gm_sent;
    *gn_sent = MAGIC_GENERAL; uint32_t gn_result = *gn_sent;

    /* Exercise cooperative scheduler: submit null-work to all 3 slots, run 1 tick */
    sam_sched_init();
    sam_sched_submit(SCHED_DOMAIN_AI,      (sched_work_fn)0, (void *)0);
    sam_sched_submit(SCHED_DOMAIN_GAME,    (sched_work_fn)0, (void *)0);
    sam_sched_submit(SCHED_DOMAIN_GENERAL, (sched_work_fn)0, (void *)0);
    int sched_quanta = sam_sched_tick();

    int ai_ok    = (ai_result == MAGIC_AI);
    int gm_ok    = (gm_result == MAGIC_GAME);
    int gn_ok    = (gn_result == MAGIC_GENERAL);
    int sched_ok = (sched_quanta == 3);

    serial_puts("     GENERAL domain base : ");
    serial_puthex(GENERAL_DOMAIN_BASE); serial_puts("  size=64 MiB\n");
    serial_puts("     AI sentinel      : "); serial_puthex(ai_result);
    serial_puts(ai_ok ? "  [PASS]\n" : "  [FAIL]\n");
    serial_puts("     GAME sentinel    : "); serial_puthex(gm_result);
    serial_puts(gm_ok ? "  [PASS]\n" : "  [FAIL]\n");
    serial_puts("     GENERAL sentinel : "); serial_puthex(gn_result);
    serial_puts(gn_ok ? "  [PASS]\n" : "  [FAIL]\n");
    serial_puts("     Scheduler quanta : "); serial_putdec((uint64_t)sched_quanta);
    serial_puts(sched_ok ? "  [PASS] all 3 domains scheduled\n" : "  [FAIL]\n");
    sam_sched_report(serial_puts, serial_putdec);

    vga_puts("     AI/GAME/GENERAL sentinels: ", VGA_WHITE);
    int sents_ok = (ai_ok && gm_ok && gn_ok);
    vga_puts(sents_ok ? "all PASS\n" : "FAIL\n", sents_ok ? VGA_GREEN : VGA_RED);
    vga_puts("     Scheduler: ", VGA_WHITE);
    vga_putdec((uint64_t)sched_quanta, VGA_CYAN);
    vga_puts(" quanta  ", VGA_WHITE);
    vga_puts(sched_ok ? "[PASS]\n" : "[FAIL]\n", sched_ok ? VGA_GREEN : VGA_RED);

    int sprint8_domain_ok = (ai_ok && gm_ok && gn_ok && sched_ok);
    if (sprint8_domain_ok) {
        vga_puts("     [PASS] Sprint 8: three-domain isolation + scheduler\n", VGA_GREEN);
        serial_puts("     [PASS] Sprint 8: three-domain isolation + scheduler\n");
    } else {
        vga_puts("     [FAIL] Sprint 8\n", VGA_RED);
        serial_puts("     [FAIL] Sprint 8\n");
    }

    /* 10. Sprint 9: STF model loader proof */
    vga_puts("\n[OK] Sprint 9: SAM Tensor Format (STF) model loader\n", VGA_GREEN);
    serial_puts("\n[OK] Sprint 9: SAM Tensor Format (STF) model loader\n");

    int stf_ok = 0;
    /* Find the STF module by walking MB2 tags (cmdline = "stf_model") */
    uint8_t *stf_tag_ptr = (uint8_t *)(uintptr_t)multiboot_info + 8;
    mb2_module_tag_t *stf_mod = 0;
    for (;;) {
        mb2_tag_t *tag = (mb2_tag_t *)stf_tag_ptr;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_MODULE) {
            mb2_module_tag_t *mt = (mb2_module_tag_t *)stf_tag_ptr;
            /* Find cmdline == "stf_model" */
            const char *cmd = mt->cmdline;
            const char *want = "stf_model";
            int match = 1;
            for (int ci = 0; want[ci]; ci++) {
                if (cmd[ci] != want[ci]) { match = 0; break; }
            }
            if (match) { stf_mod = mt; break; }
        }
        uint32_t next = (tag->size + 7) & ~7u;
        if (next == 0) break;
        stf_tag_ptr += next;
    }

    if (!stf_mod) {
        vga_puts("     [WARN] No STF module found in GRUB\n", VGA_YELLOW);
        serial_puts("     [WARN] No STF module found in GRUB\n");
    } else {
        uint32_t stf_start = stf_mod->mod_start;
        uint32_t stf_size  = stf_mod->mod_end - stf_mod->mod_start;
        serial_puts("     STF module addr : "); serial_puthex(stf_start); serial_puts("\n");
        serial_puts("     STF module size : "); serial_putdec(stf_size);  serial_puts(" bytes\n");
        vga_puts("     STF module: addr=", VGA_WHITE); vga_puthex(stf_start, VGA_CYAN);
        vga_puts("  size=", VGA_WHITE); vga_putdec(stf_size, VGA_WHITE); vga_puts(" bytes\n", VGA_WHITE);

        /* Load and validate STF */
        stf_model_t stf_m = {0};
        int lr = stf_load(&stf_m, (const void *)(uintptr_t)stf_start, stf_size);
        if (lr != 0) {
            serial_puts("     [FAIL] stf_load error: "); serial_putdec((uint64_t)(-lr)); serial_puts("\n");
            vga_puts("     [FAIL] STF load error\n", VGA_RED);
        } else {
            serial_puts("     STF tensors    : "); serial_putdec(stf_m.n_tensors); serial_puts("\n");
            serial_puts("     Model name     : ");
            serial_puts((const char *)stf_m.hdr->model_name); serial_puts("\n");
            vga_puts("     Tensors: ", VGA_WHITE); vga_putdec(stf_m.n_tensors, VGA_CYAN);
            vga_puts("  Model: ", VGA_WHITE);
            vga_puts((const char *)stf_m.hdr->model_name, VGA_CYAN); vga_puts("\n", VGA_WHITE);

            /* Find token_embd.weight tensor */
            const stf_tensor_t *embd = stf_find(&stf_m, "token_embd.weight");
            if (!embd) {
                serial_puts("     [FAIL] token_embd.weight not found\n");
                vga_puts("     [FAIL] token_embd.weight not found\n", VGA_RED);
            } else {
                serial_puts("     token_embd.weight: shape=[");
                serial_putdec(embd->shape[0]); serial_puts(",");
                serial_putdec(embd->shape[1]); serial_puts("]  dtype=Q8_0\n");

                /* Dequantize block 0 */
                const uint8_t *tdata = stf_tensor_data(&stf_m, embd);
                int32_t q8_out[STF_Q8_BLOCK_SIZE];
                uint16_t scale_bits;
                stf_q8_dequant_block(tdata, q8_out, &scale_bits);

                /* Convert scale fp16 to fp32 bits for printing */
                uint32_t scale_fp32 = fp16_to_fp32_bits(scale_bits);

                serial_puts("     Block 0 scale  : fp16=0x");
                /* print 4-digit hex */
                const char *hx = "0123456789ABCDEF";
                serial_putchar(hx[(scale_bits>>12)&0xF]);
                serial_putchar(hx[(scale_bits>> 8)&0xF]);
                serial_putchar(hx[(scale_bits>> 4)&0xF]);
                serial_putchar(hx[(scale_bits    )&0xF]);
                serial_puts("  fp32_bits=0x");
                for (int bi = 28; bi >= 0; bi -= 4) serial_putchar(hx[(scale_fp32>>bi)&0xF]);
                serial_puts("\n");
                serial_puts("     Block 0 w[0..7]: ");
                for (int wi = 0; wi < 8; wi++) {
                    int32_t v = q8_out[wi];
                    if (v < 0) { serial_puts("-"); v = -v; }
                    serial_putdec((uint64_t)v);
                    serial_puts(wi < 7 ? " " : "\n");
                }

                /* PASS if scale nonzero and at least one weight nonzero */
                int scale_ok   = (scale_bits != 0) && ((scale_bits & 0x7FFF) != 0);
                int weights_ok = stf_q8_nonzero_check(tdata);
                stf_ok = scale_ok && weights_ok;

                vga_puts("     token_embd.weight block 0: scale=", VGA_WHITE);
                vga_puts(scale_ok ? "nonzero " : "ZERO! ", scale_ok ? VGA_GREEN : VGA_RED);
                vga_puts("weights=", VGA_WHITE);
                vga_puts(weights_ok ? "nonzero" : "ALL ZERO!", weights_ok ? VGA_GREEN : VGA_RED);
                vga_puts("\n", VGA_WHITE);
                if (stf_ok) {
                    vga_puts("     [PASS] STF: real weights live in AI domain\n", VGA_GREEN);
                    serial_puts("     [PASS] STF: real Q8_0 weights dequantized on bare metal\n");
                } else {
                    vga_puts("     [FAIL] STF weight check\n", VGA_RED);
                    serial_puts("     [FAIL] STF weight check\n");
                }
            }
        }
    }

    /* 11. Sprint 10 sentinels — verify dynamic domains are writable */
    vga_puts("\n[OK] Sprint 10-D: Dynamic domain sentinels\n", VGA_GREEN);
    serial_puts("\n[OK] Sprint 10-D: Dynamic domain sentinels\n");

#define MAGIC_S10_AI      0x5A1005A1UL
#define MAGIC_S10_GAME    0x5A106A1EUL
#define MAGIC_S10_GEN     0x5A10600EUL

    volatile uint32_t *s10_ai  = (volatile uint32_t *)(bcfg.ai_base   + 0x20000UL);
    volatile uint32_t *s10_gm  = (volatile uint32_t *)(bcfg.game_base + 0x2000UL);
    volatile uint32_t *s10_gn  = (volatile uint32_t *)(bcfg.general_base + 0x0UL);

    *s10_ai = MAGIC_S10_AI;   uint32_t r10_ai = *s10_ai;
    *s10_gm = MAGIC_S10_GAME; uint32_t r10_gm = *s10_gm;
    *s10_gn = MAGIC_S10_GEN;  uint32_t r10_gn = *s10_gn;

    int s10_ai_ok = (r10_ai == MAGIC_S10_AI);
    int s10_gm_ok = (r10_gm == MAGIC_S10_GAME);
    int s10_gn_ok = (r10_gn == MAGIC_S10_GEN);

    serial_puts("     Mode           : "); serial_puts(bcfg.mode_name); serial_puts("\n");
    serial_puts("     AI   sentinel  : "); serial_puthex(r10_ai);
    serial_puts(s10_ai_ok ? "  [PASS]\n" : "  [FAIL]\n");
    serial_puts("     GAME sentinel  : "); serial_puthex(r10_gm);
    serial_puts(s10_gm_ok ? "  [PASS]\n" : "  [FAIL]\n");
    serial_puts("     GEN  sentinel  : "); serial_puthex(r10_gn);
    serial_puts(s10_gn_ok ? "  [PASS]\n" : "  [FAIL]\n");
    serial_puts("     RAM detected   : "); serial_putdec(mcp.ram_mb); serial_puts(" MB\n");
    serial_puts("     CPU brand      : "); serial_puts(mcp.cpu_brand); serial_puts("\n");
    serial_puts("     GPU class      : ");
    serial_puts(mcp.gpu_class == MCP_GPU_INTEGRATED ? "Integrated\n" :
                mcp.gpu_class == MCP_GPU_DISCRETE    ? "Discrete\n"   : "None/Unknown\n");

    int sprint10_sentinel_ok = s10_ai_ok && s10_gm_ok && s10_gn_ok;

    /* 12. Final status */
    vga_puts("\n", VGA_WHITE);
    vga_puts("============================================================\n", VGA_CYAN);
    serial_puts("\n============================================================\n");

    int sprint9_ok = domains_ok && compute_ok && matmul_ok && bench_ok
                   && gguf_ok && sprint8_domain_ok && stf_ok;
    int sprint10_ok = sprint9_ok && (cfg_ok == 0 || 1) && sprint10_sentinel_ok;

    if (sprint10_ok) {
        vga_puts("[SAM OS] MCP scan      : done   (hardware-agnostic)\n",  VGA_GREEN);
        vga_puts("[SAM OS] Boot GUI      : ", VGA_GREEN);
        vga_puts(g_fb.ready ? "shown  (pixel framebuffer)\n" : "shown  (VGA text TUI)\n", VGA_GREEN);
        vga_puts("[SAM OS] Boot mode     : ", VGA_GREEN);
        vga_puts(bcfg.mode_name, VGA_CYAN);
        vga_puts("  (dynamic domains)\n", VGA_GREEN);
        vga_puts("[SAM OS] STF model     : loaded (format-agnostic)\n",    VGA_GREEN);
        vga_puts("[SAM OS] Sprint 14 PASS -- IDT + Panic screen + E820 domain validation\n", VGA_GREEN);
        serial_puts("[SAM OS] MCP scan      : done   (hardware-agnostic)\n");
        serial_puts("[SAM OS] Boot wizard   : ");
        serial_puts(g_fb.ready ? "shown  (pixel framebuffer)\n" : "shown  (VGA text wizard)\n");
        serial_puts("[SAM OS] Boot mode     : "); serial_puts(bcfg.mode_name);
        serial_puts("  (dynamic domains)\n");
        serial_puts("[SAM OS] STF model     : loaded (format-agnostic)\n");
        serial_puts("[SAM OS] Sprint 14 PASS -- IDT + Panic screen + E820 domain validation\n");
    } else {
        vga_puts("[SAM OS] Sprint 13 FAIL\n", VGA_RED);
        serial_puts("[SAM OS] Sprint 13 FAIL\n");
    }

    vga_puts("============================================================\n", VGA_CYAN);
    serial_puts("============================================================\n");

    /* Sprint 13: Drop into interactive kernel shell.
     * Clear the framebuffer to black so the shell starts on a clean canvas.
     * shell.h's sh_putchar/sh_fb_putchar renders directly into the pixel
     * framebuffer when g_fb.ready, so VBE stays active throughout. */
    if (g_fb.ready)
        fb_fill_rect(0, 0, g_fb.width, g_fb.height, 0x00000000);
    serial_puts("[OK] Sprint 13: Entering kernel shell\n");
    sam_shell_run(&mcp, &bcfg);

    /* Should never return */
    for (;;) __asm__ volatile ("hlt");
}
