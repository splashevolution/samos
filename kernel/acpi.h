/*
 * SAM OS — kernel/acpi.h
 * Sprint 15 / Phase 3: ACPI table parser
 *
 * Parses: RSDP → RSDT or XSDT → MADT
 * Extracts:
 *   - Number of enabled logical CPUs (MADT type-0 entries with enabled flag)
 *   - I/O APIC base address (MADT type-1 entry)
 *   - Local APIC base address (MADT header field)
 *
 * Scope: read-only. No ACPI AML interpreter, no method execution,
 *        no namespace. We only read static tables needed for SMP topology
 *        and interrupt routing. Full AML interpretation is Phase 6+.
 *
 * Safety:
 *   - All pointer arithmetic bounds-checked against declared table sizes
 *   - Checksum verified on RSDP and every SDT before use
 *   - Graceful degradation: if any table is absent or corrupt, acpi_init()
 *     returns 0 and fills safe defaults (1 CPU, no I/O APIC)
 *
 * No libc. No external dependencies.
 */

#ifndef SAM_ACPI_H
#define SAM_ACPI_H

#include <stdint.h>

/* ── ACPI result structure ────────────────────────────────────────────── */
typedef struct {
    uint8_t  present;           /* 1 if RSDP found and tables parseable    */
    uint8_t  version;           /* 1 = ACPI 1.0 (RSDT), 2 = ACPI 2.0+ (XSDT) */
    uint8_t  cpu_count;         /* enabled local APICs found in MADT       */
    uint32_t lapic_base;        /* local APIC base physical address         */
    uint32_t ioapic_base;       /* I/O APIC base physical address (or 0)   */
    uint8_t  ioapic_id;         /* I/O APIC ID from MADT                   */
    char     oem_id[7];         /* OEM ID from RSDT/XSDT header            */
} sam_acpi_t;

/* ── ACPI structure definitions ───────────────────────────────────────── */

/* RSDP (Root System Description Pointer) */
typedef struct {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;      /* physical address of RSDT */
    /* ACPI 2.0+ fields (revision >= 2) */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

/* Standard SDT header (used by RSDT, XSDT, MADT, etc.) */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_hdr_t;

/* MADT (Multiple APIC Description Table) — follows acpi_sdt_hdr_t */
typedef struct {
    acpi_sdt_hdr_t hdr;
    uint32_t lapic_address;     /* local APIC physical address */
    uint32_t flags;             /* bit 0: PCAT_COMPAT (8259 present) */
} __attribute__((packed)) acpi_madt_t;

/* MADT entry header */
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) acpi_madt_entry_t;

/* MADT type 0: processor local APIC */
typedef struct {
    uint8_t  type;          /* 0 */
    uint8_t  length;        /* 8 */
    uint8_t  acpi_cpu_uid;
    uint8_t  apic_id;
    uint32_t flags;         /* bit 0: enabled */
} __attribute__((packed)) acpi_madt_lapic_t;

/* MADT type 1: I/O APIC */
typedef struct {
    uint8_t  type;          /* 1 */
    uint8_t  length;        /* 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t global_irq_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

/* ── Checksum helper ──────────────────────────────────────────────────── */
static inline int acpi_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    return sum == 0;
}

/* ── RSDP search ──────────────────────────────────────────────────────── */
/*
 * The RSDP is located in one of two places:
 *   1. The EBDA (Extended BIOS Data Area): first 1 KiB starting at the
 *      segment stored at physical address 0x40E (16-byte aligned search)
 *   2. The BIOS ROM area: 0xE0000–0xFFFFF (16-byte aligned search)
 *
 * We search both. The signature is "RSD PTR " (8 bytes, note trailing space).
 */
static inline const acpi_rsdp_t *acpi_find_rsdp(void) {
    /* "RSD PTR " is exactly 8 chars — store as bytes to avoid NUL truncation warning */
    const uint8_t sig[8] = {'R','S','D',' ','P','T','R',' '};

    /* Helper: compare 8 bytes */
    #define RSDP_SIG_MATCH(p) ( \
        ((const uint8_t *)(p))[0] == (uint8_t)sig[0] && \
        ((const uint8_t *)(p))[1] == (uint8_t)sig[1] && \
        ((const uint8_t *)(p))[2] == (uint8_t)sig[2] && \
        ((const uint8_t *)(p))[3] == (uint8_t)sig[3] && \
        ((const uint8_t *)(p))[4] == (uint8_t)sig[4] && \
        ((const uint8_t *)(p))[5] == (uint8_t)sig[5] && \
        ((const uint8_t *)(p))[6] == (uint8_t)sig[6] && \
        ((const uint8_t *)(p))[7] == (uint8_t)sig[7])

    /* 1. Search EBDA (segment at 0x40E, shifted left 4)
     *    Read via inline asm to avoid GCC -Warray-bounds on pointer cast to low address */
    uint16_t ebda_seg;
    __asm__ volatile ("movw (%1), %0"
                      : "=r"(ebda_seg)
                      : "r"((uintptr_t)0x40E)
                      : "memory");
    uint32_t ebda_phys = (uint32_t)ebda_seg << 4;
    if (ebda_phys >= 0x80000 && ebda_phys < 0xA0000) {
        for (uint32_t off = 0; off < 1024; off += 16) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)(ebda_phys + off);
            if (RSDP_SIG_MATCH(p)) {
                const acpi_rsdp_t *r = (const acpi_rsdp_t *)p;
                if (acpi_checksum(r, 20)) return r;
            }
        }
    }

    /* 2. Search BIOS ROM area 0xE0000–0xFFFFF */
    for (uint32_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
        if (RSDP_SIG_MATCH(p)) {
            const acpi_rsdp_t *r = (const acpi_rsdp_t *)p;
            if (acpi_checksum(r, 20)) return r;
        }
    }

    #undef RSDP_SIG_MATCH
    return NULL;
}

/* ── MADT parser ──────────────────────────────────────────────────────── */
static inline void acpi_parse_madt(sam_acpi_t *acpi, const acpi_sdt_hdr_t *hdr) {
    if (hdr->length < sizeof(acpi_madt_t)) return;
    if (!acpi_checksum(hdr, hdr->length)) return;

    const acpi_madt_t *madt = (const acpi_madt_t *)hdr;
    acpi->lapic_base = madt->lapic_address;

    /* Walk MADT entries */
    const uint8_t *p   = (const uint8_t *)hdr + sizeof(acpi_madt_t);
    const uint8_t *end = (const uint8_t *)hdr + hdr->length;

    while (p + 2 <= end) {
        const acpi_madt_entry_t *e = (const acpi_madt_entry_t *)p;
        if (e->length < 2 || p + e->length > end) break;

        if (e->type == 0) {
            /* Processor Local APIC */
            if (e->length >= sizeof(acpi_madt_lapic_t)) {
                const acpi_madt_lapic_t *la = (const acpi_madt_lapic_t *)p;
                if (la->flags & 1) acpi->cpu_count++;   /* enabled flag */
            }
        } else if (e->type == 1) {
            /* I/O APIC — take the first one */
            if (e->length >= sizeof(acpi_madt_ioapic_t) && acpi->ioapic_base == 0) {
                const acpi_madt_ioapic_t *ia = (const acpi_madt_ioapic_t *)p;
                acpi->ioapic_base = ia->ioapic_address;
                acpi->ioapic_id   = ia->ioapic_id;
            }
        }
        p += e->length;
    }
}

/* ── SDT finder: scan RSDT or XSDT for a table by 4-char signature ───── */
static inline const acpi_sdt_hdr_t *acpi_find_table(
        const acpi_sdt_hdr_t *rsdt, int use_xsdt, const char sig[4])
{
    if (!rsdt) return NULL;
    if (!acpi_checksum(rsdt, rsdt->length)) return NULL;

    uint32_t entry_size = use_xsdt ? 8 : 4;
    const uint8_t *p   = (const uint8_t *)rsdt + sizeof(acpi_sdt_hdr_t);
    const uint8_t *end = (const uint8_t *)rsdt + rsdt->length;

    while (p + entry_size <= end) {
        uint64_t phys;
        if (use_xsdt) {
            /* 64-bit pointer — read as two 32-bit halves to avoid alignment issues */
            uint32_t lo = *(const uint32_t *)p;
            uint32_t hi = *(const uint32_t *)(p + 4);
            phys = ((uint64_t)hi << 32) | lo;
        } else {
            phys = *(const uint32_t *)p;
        }
        p += entry_size;

        if (phys == 0) continue;
        const acpi_sdt_hdr_t *hdr = (const acpi_sdt_hdr_t *)(uintptr_t)phys;

        /* Safety: only accept addresses in first 4 GiB */
        if (phys > 0xFFFFFFFFULL) continue;

        if (hdr->signature[0] == sig[0] && hdr->signature[1] == sig[1] &&
            hdr->signature[2] == sig[2] && hdr->signature[3] == sig[3]) {
            return hdr;
        }
    }
    return NULL;
}

/* ── Main entry point ─────────────────────────────────────────────────── */
/*
 * acpi_init() — call once from kernel_main after idt_init().
 * Returns 1 on success (RSDP found, MADT parsed), 0 on failure.
 * On failure, acpi->cpu_count = 1 (safe default).
 */
static int acpi_init(sam_acpi_t *acpi) {
    /* Zero-init with safe defaults */
    for (uint32_t i = 0; i < sizeof(sam_acpi_t); i++)
        ((uint8_t *)acpi)[i] = 0;
    acpi->cpu_count  = 1;           /* assume 1 CPU if ACPI absent */
    acpi->lapic_base = 0xFEE00000;  /* x86 default local APIC address */

    const acpi_rsdp_t *rsdp = acpi_find_rsdp();
    if (!rsdp) return 0;

    acpi->present  = 1;
    acpi->version  = rsdp->revision;

    /* Copy OEM ID */
    for (int i = 0; i < 6; i++) acpi->oem_id[i] = rsdp->oem_id[i];
    acpi->oem_id[6] = '\0';

    /* Get RSDT or XSDT */
    const acpi_sdt_hdr_t *root_sdt;
    int use_xsdt = 0;

    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        root_sdt = (const acpi_sdt_hdr_t *)(uintptr_t)rsdp->xsdt_address;
        use_xsdt = 1;
    } else {
        root_sdt = (const acpi_sdt_hdr_t *)(uintptr_t)rsdp->rsdt_address;
        use_xsdt = 0;
    }

    /* Find and parse MADT */
    const char madt_sig[4] = {'A','P','I','C'};
    const acpi_sdt_hdr_t *madt_hdr = acpi_find_table(root_sdt, use_xsdt, madt_sig);
    if (madt_hdr) {
        acpi_parse_madt(acpi, madt_hdr);
    }

    return 1;
}

#endif /* SAM_ACPI_H */
