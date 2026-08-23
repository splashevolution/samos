/*
 * SAM OS — kernel/mcp.h
 * ======================
 * Sprint 10: Machine Capability Profile (MCP) scanner
 *
 * Hardware-agnostic capability detection. Reads the MB2 memory map,
 * CPUID feature bits, and PCI class codes to produce a sam_mcp_t struct
 * that the boot configurator uses to compute domain sizes.
 *
 * Design principles:
 *   - Never references specific device names (no "Intel HD", no "Radeon")
 *   - Uses PCI class codes (0x0300 = display) not vendor/device IDs
 *   - Vendor/device IDs are recorded for the driver layer only
 *   - Works on any x86-64 machine with GRUB2 multiboot2
 *   - No libc, no external dependencies
 *
 * Supported architectures: x86-64 (current), ARM64/RISC-V (future ports)
 */

#ifndef SAM_MCP_H
#define SAM_MCP_H

#include <stdint.h>
#include <stddef.h>

/* ── CPU architecture constants ─────────────────────────────────────────── */
#define MCP_ARCH_X86_64    0x01
#define MCP_ARCH_ARM64     0x02   /* future */
#define MCP_ARCH_RISCV64   0x03   /* future */

/* ── SIMD level constants (matches simd.h SAM_SIMD_* values) ───────────── */
#define MCP_SIMD_SCALAR    0x00
#define MCP_SIMD_SSE42     0x01
#define MCP_SIMD_AVX2      0x02
#define MCP_SIMD_NEON      0x10   /* ARM future */
#define MCP_SIMD_SVE       0x11   /* ARM future */

/* ── GPU class constants (PCI class-based, not vendor-based) ────────────── */
#define MCP_GPU_NONE       0x00   /* no display controller found */
#define MCP_GPU_INTEGRATED 0x01   /* PCI class 0x0300, shares system RAM */
#define MCP_GPU_DISCRETE   0x02   /* PCI class 0x0302, has own VRAM */
#define MCP_GPU_MULTI      0x03   /* both integrated + discrete present */

/* ── PCI vendor IDs (recorded for driver layer, not used by kernel) ─────── */
#define MCP_PCI_VENDOR_INTEL  0x8086
#define MCP_PCI_VENDOR_AMD    0x1002
#define MCP_PCI_VENDOR_NVIDIA 0x10DE
#define MCP_PCI_VENDOR_QEMU   0x1234   /* QEMU stdvga */

/* ── PCI class codes used during scan ──────────────────────────────────── */
#define PCI_CLASS_DISPLAY_VGA      0x0300
#define PCI_CLASS_DISPLAY_XGA      0x0301
#define PCI_CLASS_DISPLAY_3D       0x0302
#define PCI_CLASS_DISPLAY_OTHER    0x0380
#define PCI_CLASS_STORAGE_AHCI     0x0106
#define PCI_CLASS_STORAGE_NVME     0x0108
#define PCI_CLASS_NETWORK_ETHERNET 0x0200
#define PCI_CLASS_NETWORK_WIRELESS 0x0280
#define PCI_CLASS_SERIAL_USB2      0x0C03   /* EHCI */
#define PCI_CLASS_SERIAL_USB3      0x0C03   /* xHCI — subclass distinguishes */
#define PCI_CLASS_AUDIO_HDA        0x0403

/* ── Machine Capability Profile ─────────────────────────────────────────── */
typedef struct {
    /* Memory */
    uint32_t  ram_mb;           /* total usable RAM in MiB (from MB2 mmap) */
    uint32_t  ram_mb_raw;       /* physical RAM before firmware reservations */

    /* CPU */
    uint8_t   cpu_arch;         /* MCP_ARCH_* */
    uint8_t   simd_level;       /* MCP_SIMD_* */
    uint8_t   cpu_cores;        /* physical cores */
    uint8_t   cpu_threads;      /* logical threads (with HyperThreading) */
    uint32_t  cpu_mhz;          /* from PIT calibration (already done) */
    char      cpu_brand[48];    /* CPUID brand string, null-terminated */

    /* GPU — class-based for kernel, vendor/device for driver layer */
    uint8_t   gpu_class;        /* MCP_GPU_* */
    uint16_t  gpu_pci_vendor;   /* e.g. 0x8086 — driver layer only */
    uint16_t  gpu_pci_device;   /* e.g. 0x0046 — driver layer only */
    uint8_t   fb_available;     /* 1 if MB2 framebuffer tag present */
    uint32_t  fb_width;         /* framebuffer width in pixels */
    uint32_t  fb_height;        /* framebuffer height in pixels */
    uint8_t   fb_bpp;           /* bits per pixel */
    uint8_t   fb_type_raw;      /* raw fb_type from MB2 tag */
    uint64_t  fb_addr_raw;      /* raw fb_addr from MB2 tag */

    /* Storage */
    uint8_t   storage_count;    /* number of AHCI/NVMe devices found */
    uint8_t   has_nvme;         /* 1 if NVMe controller present */
    uint8_t   has_ahci;         /* 1 if AHCI SATA controller present */

    /* Connectivity */
    uint8_t   has_ethernet;     /* 1 if PCIe/PCI NIC found */
    uint8_t   has_wireless;     /* 1 if WLAN controller found */
    uint8_t   has_usb2;         /* 1 if EHCI controller found */
    uint8_t   has_usb3;         /* 1 if xHCI controller found */
    uint8_t   has_hda_audio;    /* 1 if Intel HDA or compatible found */

    /* Scan status */
    uint8_t   pci_scanned;      /* 1 if PCI bus scan completed */
    uint8_t   mmap_valid;       /* 1 if MB2 memory map was parsed */
} sam_mcp_t;

/* ── MB2 tag types needed for MCP scan ─────────────────────────────────── */
#define MB2_TAG_END         0
#define MB2_TAG_MMAP        6
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_TAG_MODULE      3

/* MB2 memory map entry types */
#define MB2_MMAP_AVAILABLE  1
#define MB2_MMAP_RESERVED   2
#define MB2_MMAP_ACPI       3
#define MB2_MMAP_NVS        4
#define MB2_MMAP_BADRAM     5

typedef struct { uint32_t type; uint32_t size; } __attribute__((packed)) mb2_tag_t;

typedef struct {
    uint32_t type; uint32_t size;
    uint32_t entry_size; uint32_t entry_version;
} __attribute__((packed)) mb2_mmap_tag_t;

typedef struct {
    uint64_t base_addr; uint64_t length; uint32_t type; uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

typedef struct {
    uint32_t type; uint32_t size;
    uint64_t fb_addr;
    uint32_t fb_pitch; uint32_t fb_width; uint32_t fb_height;
    uint8_t  fb_bpp;   uint8_t  fb_type;  uint16_t reserved;
} __attribute__((packed)) mb2_fb_tag_t;

/* ── PCI access (x86 port I/O) ──────────────────────────────────────────── */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static inline void mcp_outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t mcp_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev,
                                   uint8_t fn, uint8_t reg) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (reg & 0xFC);
    mcp_outl(PCI_CONFIG_ADDR, addr);
    return mcp_inl(PCI_CONFIG_DATA);
}

/* ── CPUID helper ───────────────────────────────────────────────────────── */
typedef struct { uint32_t eax, ebx, ecx, edx; } mcp_cpuid_t;
static inline mcp_cpuid_t mcp_cpuid(uint32_t leaf, uint32_t subleaf) {
    mcp_cpuid_t r;
    __asm__ volatile ("cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(subleaf));
    return r;
}

/* ── String helpers (no libc) ───────────────────────────────────────────── */
static inline void mcp_memcpy(void *dst, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static inline void mcp_memset(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

/* ── MCP scan: RAM from MB2 memory map ──────────────────────────────────── */
static inline void mcp_scan_ram(sam_mcp_t *mcp, uint64_t multiboot_info) {
    uint8_t *p = (uint8_t *)(uintptr_t)(multiboot_info + 8);
    uint64_t usable = 0, raw = 0;

    for (;;) {
        mb2_tag_t *tag = (mb2_tag_t *)p;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_MMAP) {
            mb2_mmap_tag_t *mt = (mb2_mmap_tag_t *)p;
            uint8_t *ep = p + sizeof(mb2_mmap_tag_t);
            uint8_t *end = p + tag->size;
            while (ep + mt->entry_size <= end) {
                mb2_mmap_entry_t *e = (mb2_mmap_entry_t *)ep;
                raw += e->length;
                if (e->type == MB2_MMAP_AVAILABLE) usable += e->length;
                ep += mt->entry_size;
            }
            mcp->mmap_valid = 1;
        }
        uint32_t next = (tag->size + 7) & ~7u;
        if (next == 0) break;
        p += next;
    }

    /* Round up to nearest MiB so 512 MiB machines don't display as 511 */
    mcp->ram_mb     = (uint32_t)((usable + ((1ULL<<20)-1)) >> 20);
    mcp->ram_mb_raw = (uint32_t)((raw    + ((1ULL<<20)-1)) >> 20);
}

/* ── MCP scan: framebuffer from MB2 tag ─────────────────────────────────── */
static inline void mcp_scan_fb(sam_mcp_t *mcp, uint64_t multiboot_info) {
    uint8_t *p = (uint8_t *)(uintptr_t)(multiboot_info + 8);
    for (;;) {
        mb2_tag_t *tag = (mb2_tag_t *)p;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            mb2_fb_tag_t *fb = (mb2_fb_tag_t *)p;
            mcp->fb_available = 1;
            mcp->fb_width     = fb->fb_width;
            mcp->fb_height    = fb->fb_height;
            mcp->fb_bpp       = fb->fb_bpp;
            break;
        }
        uint32_t next = (tag->size + 7) & ~7u;
        if (next == 0) break;
        p += next;
    }
}

/* ── MCP scan: CPU via CPUID ────────────────────────────────────────────── */
static inline void mcp_scan_cpu(sam_mcp_t *mcp) {
    mcp->cpu_arch = MCP_ARCH_X86_64;

    /* Brand string: leaves 0x80000002–0x80000004, 12 DWORDs = 48 bytes */
    uint32_t max_ext = mcp_cpuid(0x80000000, 0).eax;
    if (max_ext >= 0x80000004) {
        uint32_t *dst = (uint32_t *)(void *)mcp->cpu_brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            mcp_cpuid_t r = mcp_cpuid(leaf, 0);
            *dst++ = r.eax; *dst++ = r.ebx;
            *dst++ = r.ecx; *dst++ = r.edx;
        }
        mcp->cpu_brand[47] = '\0';
        /* Strip leading spaces (CPUID brand strings often start with spaces) */
        int si = 0;
        while (mcp->cpu_brand[si] == ' ') si++;
        if (si > 0) {
            int di = 0;
            while (mcp->cpu_brand[si]) mcp->cpu_brand[di++] = mcp->cpu_brand[si++];
            mcp->cpu_brand[di] = '\0';
        }
    } else {
        /* fallback label */
        const char *fb = "x86-64 processor";
        for (int i = 0; fb[i] && i < 47; i++) mcp->cpu_brand[i] = fb[i];
        mcp->cpu_brand[47] = '\0';
    }

    /* Core/thread count: leaf 1 EBX[23:16] = logical processors */
    mcp_cpuid_t l1 = mcp_cpuid(1, 0);
    uint8_t logical = (uint8_t)((l1.ebx >> 16) & 0xFF);
    if (logical == 0) logical = 1;

    /* Physical cores: leaf 4 EAX[31:26]+1 (Intel) */
    mcp_cpuid_t l4 = mcp_cpuid(4, 0);
    uint8_t cores = (uint8_t)(((l4.eax >> 26) & 0x3F) + 1);
    if (cores == 0 || cores > logical) cores = logical;

    mcp->cpu_cores   = cores;
    mcp->cpu_threads = logical;

    /* SIMD: SSE4.2 = ECX bit 20, AVX = ECX bit 28 */
    int has_sse42 = (l1.ecx >> 20) & 1;
    int has_avx   = (l1.ecx >> 28) & 1;
    /* AVX2: leaf 7 EBX bit 5 */
    mcp_cpuid_t l7 = mcp_cpuid(7, 0);
    int has_avx2 = (l7.ebx >> 5) & 1;

    if (has_avx2)       mcp->simd_level = MCP_SIMD_AVX2;
    else if (has_avx)   mcp->simd_level = MCP_SIMD_SSE42;  /* AVX w/o AVX2 */
    else if (has_sse42) mcp->simd_level = MCP_SIMD_SSE42;
    else                mcp->simd_level = MCP_SIMD_SCALAR;
}

/* ── MCP scan: PCI bus — full recursive multifunction + bridge scan ─────── */
/*
 * Sprint 15: Proper PCI enumeration.
 *
 * Algorithm (standard recursive PCI scan, PCI Local Bus Spec §6.1):
 *   For each bus in [0..255]:
 *     For each device in [0..31]:
 *       Read function 0. If vendor == 0xFFFF → no device, skip.
 *       Check header type byte (offset 0x0E):
 *         bit 7 set → multifunction: enumerate functions 1-7 as well.
 *         bits 6:0 == 0x01 → PCI-PCI bridge: read secondary bus (offset 0x19)
 *                             and recurse into it.
 *       Classify every valid function by class+subclass.
 *
 * We track visited buses in a 256-bit bitmap to avoid infinite loops on
 * broken BIOS tables that create bus cycles (rare but possible on VMs).
 *
 * Additional fields added to sam_mcp_t:
 *   ahci_bar5  — BAR5 physical address of first AHCI controller (ABAR)
 *   ata_iobase — I/O base of first legacy ATA controller (for ATA PIO)
 *   pci_dev_count — total PCI functions found
 */

/* Additional MCP fields for Phase 3 — appended via a separate struct.
 * We add them as a sub-struct so the existing sam_mcp_t ABI is unchanged. */
#ifndef SAM_MCP_PCI3_FIELDS
#define SAM_MCP_PCI3_FIELDS
/* These fields are accessed as mcp->pci3.field */
typedef struct {
    uint32_t ahci_bar5;     /* AHCI ABAR (BAR5) physical address, 0 if none */
    uint16_t ata_iobase;    /* primary ATA I/O base (0x1F0 standard, or from BAR) */
    uint16_t pci_dev_count; /* total PCI functions enumerated */
} sam_mcp_pci3_t;
#endif

static uint8_t _pci_bus_visited[32]; /* 256-bit bitmap: bit N = bus N visited */

static inline int _pci_bus_mark(uint8_t bus) {
    uint8_t byte = bus >> 3, bit = bus & 7;
    if (_pci_bus_visited[byte] & (1u << bit)) return 0; /* already visited */
    _pci_bus_visited[byte] |= (1u << bit);
    return 1;
}

static sam_mcp_pci3_t _pci3;   /* filled during scan, copied into mcp after */

static inline void _pci_classify(sam_mcp_t *mcp,
                                  uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t id = pci_read32(bus, dev, fn, 0x00);
    if (id == 0xFFFFFFFF || id == 0x00000000) return;

    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    uint16_t device = (uint16_t)(id >> 16);

    uint32_t cc       = pci_read32(bus, dev, fn, 0x08);
    uint16_t pci_class = (uint16_t)(cc >> 16);
    uint8_t  prog_if   = (uint8_t)((cc >> 8) & 0xFF);

    _pci3.pci_dev_count++;

    /* Display */
    if (pci_class == PCI_CLASS_DISPLAY_VGA ||
        pci_class == PCI_CLASS_DISPLAY_XGA ||
        pci_class == PCI_CLASS_DISPLAY_3D  ||
        pci_class == PCI_CLASS_DISPLAY_OTHER) {
        if (mcp->gpu_class == MCP_GPU_NONE) {
            mcp->gpu_class      = MCP_GPU_INTEGRATED;
            mcp->gpu_pci_vendor = vendor;
            mcp->gpu_pci_device = device;
        } else {
            mcp->gpu_class = MCP_GPU_MULTI;
        }
    }

    /* AHCI: class 0x0106 — read BAR5 (ABAR) for Phase 3 disk driver */
    if (pci_class == PCI_CLASS_STORAGE_AHCI) {
        mcp->has_ahci = 1;
        mcp->storage_count++;
        if (_pci3.ahci_bar5 == 0) {
            uint32_t bar5 = pci_read32(bus, dev, fn, 0x24);
            _pci3.ahci_bar5 = bar5 & ~0xFu;  /* strip lower 4 attribute bits */
        }
    }

    /* Legacy ATA / IDE: class 0x0101 — record primary I/O base */
    if ((pci_class & 0xFF00) == 0x0100 && (pci_class & 0x00FF) == 0x01) {
        if (_pci3.ata_iobase == 0) {
            /* prog_if bit 0: 0 = legacy 0x1F0, 1 = native BAR0 */
            if ((prog_if & 0x01) == 0) {
                _pci3.ata_iobase = 0x1F0;
            } else {
                uint32_t bar0 = pci_read32(bus, dev, fn, 0x10);
                if (bar0 & 1) _pci3.ata_iobase = (uint16_t)(bar0 & ~3u);
            }
        }
    }

    /* NVMe: class 0x0108 */
    if (pci_class == PCI_CLASS_STORAGE_NVME) {
        mcp->has_nvme = 1;
        mcp->storage_count++;
    }

    /* Network */
    if (pci_class == PCI_CLASS_NETWORK_ETHERNET) mcp->has_ethernet = 1;
    if (pci_class == PCI_CLASS_NETWORK_WIRELESS) mcp->has_wireless  = 1;

    /* USB: class=0x0C subclass=0x03, prog_if distinguishes EHCI/xHCI */
    if ((pci_class & 0xFF00) == 0x0C00 && (pci_class & 0x00FF) == 0x03) {
        if (prog_if == 0x30) mcp->has_usb3 = 1;
        else                 mcp->has_usb2 = 1;
    }

    /* Audio HDA: class 0x0403 */
    if (pci_class == PCI_CLASS_AUDIO_HDA) mcp->has_hda_audio = 1;

    (void)vendor; (void)device;
}

/* Forward declaration for mutual recursion */
static inline void _pci_scan_bus(sam_mcp_t *mcp, uint8_t bus);

static inline void _pci_scan_device(sam_mcp_t *mcp, uint8_t bus, uint8_t dev) {
    uint32_t id0 = pci_read32(bus, dev, 0, 0x00);
    if (id0 == 0xFFFFFFFF || id0 == 0x00000000) return;

    /* Read header type from offset 0x0E (byte 2 of the 4-byte word at 0x0C) */
    uint32_t hdr_dword = pci_read32(bus, dev, 0, 0x0C);
    uint8_t  hdr_type  = (uint8_t)((hdr_dword >> 16) & 0xFF);
    uint8_t  multifunction = (hdr_type & 0x80) != 0;
    uint8_t  type_bits     = hdr_type & 0x7F;

    /* Classify function 0 */
    _pci_classify(mcp, bus, dev, 0);

    /* PCI-PCI bridge (header type 1): follow secondary bus */
    if (type_bits == 0x01) {
        uint32_t bus_reg = pci_read32(bus, dev, 0, 0x18);
        uint8_t secondary = (uint8_t)((bus_reg >> 8) & 0xFF);
        if (secondary != 0 && secondary != bus)
            _pci_scan_bus(mcp, secondary);
    }

    /* Multifunction: enumerate functions 1-7 */
    if (multifunction) {
        for (uint8_t fn = 1; fn < 8; fn++) {
            uint32_t idfn = pci_read32(bus, dev, fn, 0x00);
            if (idfn == 0xFFFFFFFF || idfn == 0x00000000) continue;

            _pci_classify(mcp, bus, dev, fn);

            /* Bridge in a non-zero function */
            uint32_t hfn = pci_read32(bus, dev, fn, 0x0C);
            uint8_t  htype_fn = (uint8_t)((hfn >> 16) & 0x7F);
            if (htype_fn == 0x01) {
                uint32_t br = pci_read32(bus, dev, fn, 0x18);
                uint8_t sec = (uint8_t)((br >> 8) & 0xFF);
                if (sec != 0 && sec != bus) _pci_scan_bus(mcp, sec);
            }
        }
    }
}

static inline void _pci_scan_bus(sam_mcp_t *mcp, uint8_t bus) {
    if (!_pci_bus_mark(bus)) return;   /* already visited */
    for (uint8_t dev = 0; dev < 32; dev++)
        _pci_scan_device(mcp, bus, dev);
}

static inline void mcp_scan_pci(sam_mcp_t *mcp) {
    /* Zero the visited bitmap and scratch struct */
    for (int i = 0; i < 32; i++) _pci_bus_visited[i] = 0;
    for (uint32_t i = 0; i < sizeof(sam_mcp_pci3_t); i++)
        ((uint8_t *)&_pci3)[i] = 0;

    /* Default ATA I/O base -- overridden by PCI scan if native mode */
    _pci3.ata_iobase = 0x1F0;

    /* Start recursive scan from bus 0 */
    _pci_scan_bus(mcp, 0);

    mcp->pci_scanned = 1;
}

/* Main entry point: run all scans */
static inline void sam_mcp_scan(sam_mcp_t *mcp,
                                 uint64_t multiboot_info,
                                 uint32_t cpu_mhz_pit) {
    mcp_memset(mcp, 0, sizeof(sam_mcp_t));
    mcp->cpu_mhz = cpu_mhz_pit;

    mcp_scan_ram(mcp, multiboot_info);
    mcp_scan_fb (mcp, multiboot_info);
    mcp_scan_cpu(mcp);
    mcp_scan_pci(mcp);
}

#endif /* SAM_MCP_H */
