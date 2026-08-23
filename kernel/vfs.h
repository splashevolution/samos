/*
 * SAM OS — kernel/vfs.h
 * Sprint 16 / Phase 4: minimal in-memory VFS over an initrd ustar archive
 *
 * The initrd is loaded by GRUB as a multiboot2 module (cmdline "initrd").
 * It is an uncompressed USTAR tar archive. vfs_init() parses the archive,
 * building a file table that points directly into the archive memory
 * (no copying). open/read/close are provided over that table.
 *
 * Limitations (honest):
 *   - Read-only, no directories beyond name prefixes, no writeback.
 *   - Max 16 files, names truncated to 100 chars (ustar limit anyway).
 */

#ifndef SAM_VFS_H
#define SAM_VFS_H

#include <stdint.h>

#define VFS_MAX_FILES 16
#define VFS_NAME_MAX  100

typedef struct {
    char            name[VFS_NAME_MAX + 1];
    const uint8_t  *data;
    uint32_t        size;
} vfs_file_t;

typedef struct {
    int             fd;     /* index into vfs_files, -1 = closed */
    uint32_t        pos;
} vfs_file_handle_t;

static vfs_file_t         vfs_files[VFS_MAX_FILES];
static int                vfs_file_count = 0;

/* ── Octal ASCII -> uint64 ("00000000644\0") ─────────────────────────── */
static uint64_t vfs_octal(const char *s, int len) {
    uint64_t v = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c < '0' || c > '7') break;
        v = (v << 3) | (uint64_t)(c - '0');
    }
    return v;
}

/* ── Parse a USTAR archive and populate the file table ────────────────── */
/* Returns number of files registered, or negative error:
 *   -1 null pointer, -2 bad magic on first entry, -3 too many files       */
static int vfs_init(const void *archive, uint32_t archive_size) {
    if (!archive || archive_size < 512) return -1;

    const uint8_t *p   = (const uint8_t *)archive;
    const uint8_t *end = p + archive_size;
    vfs_file_count = 0;

    while (p + 512 <= end) {
        /* All-zero block = end of archive */
        int zero = 1;
        for (int i = 0; i < 512; i++) {
            if (p[i] != 0) { zero = 0; break; }
        }
        if (zero) break;

        /* Validate ustar magic at offset 257 ("ustar") */
        if (!(p[257]=='u' && p[258]=='s' && p[259]=='t' && p[260]=='a' && p[261]=='r'))
            return (vfs_file_count == 0) ? -2 : vfs_file_count;

        /* Regular file: typeflag '0' or NUL */
        char typeflag = (char)p[156];
        if (typeflag == '0' || typeflag == '\0') {
            if (vfs_file_count >= VFS_MAX_FILES) return -3;

            vfs_file_t *f = &vfs_files[vfs_file_count];
            /* Normalise every path to a leading '/' */
            f->name[0] = '/';
            int ni = 1;
            while (ni < VFS_NAME_MAX && p[ni - 1] != '\0') {
                f->name[ni] = (char)p[ni - 1];
                ni++;
            }
            f->name[ni] = '\0';
            f->size = (uint32_t)vfs_octal((const char *)(p + 124), 12);

            /* Bounds check: data must lie inside the archive */
            const uint8_t *data = p + 512;
            if (data + f->size > end) f->size = (uint32_t)(end - data);
            f->data = data;

            vfs_file_count++;
        }

        /* Advance past header + padded data blocks */
        uint32_t sz = (uint32_t)vfs_octal((const char *)(p + 124), 12);
        uint32_t adv = 512 + ((sz + 511) & ~511u);
        if (p + adv <= p) break;    /* overflow guard */
        p += adv;
    }

    return vfs_file_count;
}

/* ── Lookup by exact name ("/hello.bin"). Returns index or -1 ─────────── */
static int vfs_lookup(const char *name) {
    for (int i = 0; i < vfs_file_count; i++) {
        const char *a = vfs_files[i].name;
        const char *b = name;
        int match = 1;
        while (*a || *b) {
            if (*a != *b) { match = 0; break; }
            a++; b++;
        }
        if (match) return i;
    }
    return -1;
}

/* ── Minimal POSIX-ish surface ────────────────────────────────────────── */
static int vfs_open(const char *name, vfs_file_handle_t *h) {
    int idx = vfs_lookup(name);
    if (idx < 0) return -1;
    h->fd  = idx;
    h->pos = 0;
    return 0;
}

static int32_t vfs_read(vfs_file_handle_t *h, void *buf, uint32_t len) {
    if (h->fd < 0 || h->fd >= vfs_file_count) return -1;
    const vfs_file_t *f = &vfs_files[h->fd];
    if (h->pos >= f->size) return 0;
    uint32_t avail = f->size - h->pos;
    if (len > avail) len = avail;
    const uint8_t *src = f->data + h->pos;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
    h->pos += len;
    return (int32_t)len;
}

static int vfs_close(vfs_file_handle_t *h) {
    h->fd = -1;
    h->pos = 0;
    return 0;
}

#endif /* SAM_VFS_H */
