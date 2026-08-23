/*
 * SAM OS — kernel/ata.h
 * Sprint 15 / Phase 3: ATA PIO read-only disk driver
 *
 * Supports:
 *   - Primary ATA channel (I/O base 0x1F0, control 0x3F6)
 *   - IDENTIFY DEVICE command to detect drive presence, model, and size
 *   - LBA28 READ SECTORS (PIO) for sector reads up to 255 sectors
 *   - Read-only: no write, format, or partition ops
 *
 * Not supported (Phase 4+):
 *   - AHCI (SATA) — needs memory-mapped registers from BAR5
 *   - DMA transfers — require PRDT setup
 *   - Secondary channel
 *   - LBA48 (drives > 128 GiB)
 *
 * Usage:
 *   ata_init() — detect drive, fill sam_ata_t
 *   ata_read_sectors(lba, count, buf) — read sectors into buffer
 *
 * No libc. No external dependencies.
 */

#ifndef SAM_ATA_H
#define SAM_ATA_H

#include <stdint.h>

/* ── ATA register offsets from I/O base ──────────────────────────────── */
#define ATA_REG_DATA        0x00  /* R/W: 16-bit data register */
#define ATA_REG_ERROR       0x01  /* R:   error register */
#define ATA_REG_FEATURES    0x01  /* W:   features */
#define ATA_REG_SECCOUNT    0x02  /* R/W: sector count */
#define ATA_REG_LBA0        0x03  /* R/W: LBA bits 0-7 */
#define ATA_REG_LBA1        0x04  /* R/W: LBA bits 8-15 */
#define ATA_REG_LBA2        0x05  /* R/W: LBA bits 16-23 */
#define ATA_REG_HDDEVSEL    0x06  /* R/W: drive select + LBA bits 24-27 */
#define ATA_REG_STATUS      0x07  /* R:   status register */
#define ATA_REG_COMMAND     0x07  /* W:   command register */

/* ATA control register (control base = I/O base + 0x206 for primary) */
#define ATA_REG_CONTROL     0x206 /* W: device control (via control base offset) */
#define ATA_REG_ALTSTATUS   0x206 /* R: alternate status (same port) */

/* ATA status bits */
#define ATA_SR_BSY   0x80   /* busy */
#define ATA_SR_DRDY  0x40   /* drive ready */
#define ATA_SR_DRQ   0x08   /* data request — data ready to transfer */
#define ATA_SR_ERR   0x01   /* error */

/* ATA commands */
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_SECTORS 0x20

/* Primary ATA channel */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

/* ── Drive info ───────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  present;           /* 1 if drive detected */
    uint8_t  lba_supported;     /* 1 if LBA28 supported */
    uint32_t lba28_sectors;     /* total sectors (LBA28 capacity) */
    uint64_t size_mb;           /* drive size in MiB */
    char     model[41];         /* model string from IDENTIFY (null-terminated) */
    char     serial[21];        /* serial number from IDENTIFY */
    uint16_t iobase;            /* I/O base used (0x1F0 or from PCI) */
} sam_ata_t;

/* ── I/O helpers ──────────────────────────────────────────────────────── */
static inline void _ata_outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1" :: "a"(v),"Nd"(port));
}
static inline uint8_t _ata_inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline uint16_t _ata_inw(uint16_t port) {
    uint16_t v; __asm__ volatile ("inw %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void _ata_io_wait(uint16_t ctrl) {
    /* 4 reads of alternate status = ~400ns delay */
    _ata_inb((uint16_t)(ctrl + ATA_REG_ALTSTATUS));
    _ata_inb((uint16_t)(ctrl + ATA_REG_ALTSTATUS));
    _ata_inb((uint16_t)(ctrl + ATA_REG_ALTSTATUS));
    _ata_inb((uint16_t)(ctrl + ATA_REG_ALTSTATUS));
}

/* ── String swap helper: IDENTIFY data is byte-swapped ───────────────── */
static inline void _ata_swap_string(char *dst, const uint16_t *src,
                                     int words, int maxdst) {
    int j = 0;
    for (int i = 0; i < words && j + 1 < maxdst; i++) {
        dst[j++] = (char)(src[i] >> 8);
        dst[j++] = (char)(src[i] & 0xFF);
    }
    dst[j < maxdst ? j : maxdst - 1] = '\0';
    /* Trim trailing spaces */
    int end = j - 1;
    while (end >= 0 && dst[end] == ' ') dst[end--] = '\0';
}

/* ── Wait for BSY=0 (with timeout to avoid infinite loop on no drive) ── */
static inline int _ata_wait_not_busy(uint16_t io, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        if (!(_ata_inb((uint16_t)(io + ATA_REG_STATUS)) & ATA_SR_BSY))
            return 1;
        __asm__ volatile ("pause");
    }
    return 0;  /* timeout */
}

static inline int _ata_wait_drq(uint16_t io, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        uint8_t st = _ata_inb((uint16_t)(io + ATA_REG_STATUS));
        if (st & ATA_SR_ERR) return 0;
        if (st & ATA_SR_DRQ) return 1;
        __asm__ volatile ("pause");
    }
    return 0;
}

/* ── ata_init — detect primary ATA drive ─────────────────────────────── */
static int ata_init(sam_ata_t *ata) {
    /* Zero-init */
    for (uint32_t i = 0; i < sizeof(sam_ata_t); i++) ((uint8_t *)ata)[i] = 0;
    ata->iobase = ATA_PRIMARY_IO;

    uint16_t io   = ATA_PRIMARY_IO;
    uint16_t ctrl = ATA_PRIMARY_CTRL;

    /* Software reset: set SRST bit in device control, then clear */
    _ata_outb(ctrl, 0x04);   /* SRST = 1 */
    _ata_io_wait(ctrl);
    _ata_outb(ctrl, 0x00);   /* SRST = 0, nIEN = 0 */
    _ata_io_wait(ctrl);

    /* Select drive 0 (master), LBA mode */
    _ata_outb((uint16_t)(io + ATA_REG_HDDEVSEL), 0xE0);  /* drive 0, LBA */
    _ata_io_wait(ctrl);

    /* Wait for BSY to clear (up to ~1 million polls ≈ ~1s) */
    if (!_ata_wait_not_busy(io, 1000000)) return 0;

    /* Check if anything responded: status == 0x00 or 0xFF → no drive */
    uint8_t status = _ata_inb((uint16_t)(io + ATA_REG_STATUS));
    if (status == 0x00 || status == 0xFF) return 0;

    /* Send IDENTIFY DEVICE */
    _ata_outb((uint16_t)(io + ATA_REG_SECCOUNT),  0);
    _ata_outb((uint16_t)(io + ATA_REG_LBA0),       0);
    _ata_outb((uint16_t)(io + ATA_REG_LBA1),       0);
    _ata_outb((uint16_t)(io + ATA_REG_LBA2),       0);
    _ata_outb((uint16_t)(io + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
    _ata_io_wait(ctrl);

    status = _ata_inb((uint16_t)(io + ATA_REG_STATUS));
    if (status == 0x00) return 0;  /* no drive */

    /* Wait for DRQ or ERR */
    if (!_ata_wait_drq(io, 1000000)) return 0;

    /* Read 256 words of IDENTIFY data */
    uint16_t ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = _ata_inw((uint16_t)(io + ATA_REG_DATA));

    /* Word 83 bit 10: LBA48 support; word 60-61: LBA28 total sectors */
    ata->lba_supported   = 1;
    uint32_t lba28_lo    = ident[60];
    uint32_t lba28_hi    = ident[61];
    ata->lba28_sectors   = (lba28_hi << 16) | lba28_lo;
    ata->size_mb         = ((uint64_t)ata->lba28_sectors * 512) / (1024 * 1024);

    /* Extract model string (words 27-46, 20 words = 40 chars) */
    _ata_swap_string(ata->model, &ident[27], 20, 41);

    /* Extract serial (words 10-19, 10 words = 20 chars) */
    _ata_swap_string(ata->serial, &ident[10], 10, 21);

    ata->present = 1;
    return 1;
}

/* ── ata_read_sectors — LBA28 PIO read ───────────────────────────────── */
/*
 * Reads `count` sectors starting at `lba` into `buf`.
 * buf must be at least count * 512 bytes.
 * Returns 1 on success, 0 on error.
 * Maximum count = 255 per call (ATA LBA28 limit per command).
 */
static int ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
    __attribute__((unused));
static int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (count == 0) return 1;

    uint16_t io   = ATA_PRIMARY_IO;
    uint16_t ctrl = ATA_PRIMARY_CTRL;

    /* Wait for drive ready */
    if (!_ata_wait_not_busy(io, 1000000)) return 0;

    /* Set up LBA28 registers */
    _ata_outb((uint16_t)(io + ATA_REG_HDDEVSEL),
              (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));  /* drive 0, LBA, bits 24-27 */
    _ata_outb((uint16_t)(io + ATA_REG_FEATURES), 0x00);
    _ata_outb((uint16_t)(io + ATA_REG_SECCOUNT),  count);
    _ata_outb((uint16_t)(io + ATA_REG_LBA0),  (uint8_t)(lba & 0xFF));
    _ata_outb((uint16_t)(io + ATA_REG_LBA1),  (uint8_t)((lba >> 8) & 0xFF));
    _ata_outb((uint16_t)(io + ATA_REG_LBA2),  (uint8_t)((lba >> 16) & 0xFF));
    _ata_outb((uint16_t)(io + ATA_REG_COMMAND), ATA_CMD_READ_SECTORS);

    uint16_t *dst = (uint16_t *)buf;

    for (uint8_t s = 0; s < count; s++) {
        _ata_io_wait(ctrl);
        if (!_ata_wait_drq(io, 1000000)) return 0;

        /* Read 256 words = 512 bytes */
        for (int i = 0; i < 256; i++)
            *dst++ = _ata_inw((uint16_t)(io + ATA_REG_DATA));

        _ata_io_wait(ctrl);
    }

    return 1;
}

#endif /* SAM_ATA_H */
