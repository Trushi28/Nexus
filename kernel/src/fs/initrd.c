#include "fs/initrd.h"
#include "fs/vfs.h"
#include "debug/log.h"

/* POSIX ustar header, 512 bytes exactly. Numeric fields are ASCII
 * octal, NUL- or space-terminated; text fields are NOT guaranteed
 * NUL-terminated if they exactly fill their field width, so every
 * field gets copied into a bounded local buffer before use -- see
 * field_copy() below. Reference: POSIX.1-2001, "ustar interchange
 * format". */
struct PACKED ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

#define TAR_TYPE_FILE  '0'
#define TAR_TYPE_AFILE '\0' /* old tar's "regular file", pre-POSIX */
#define TAR_TYPE_DIR   '5'

static uint64_t parse_octal(const char *field, size_t len) {
    uint64_t v = 0;
    for (size_t i = 0; i < len && field[i] >= '0' && field[i] <= '7'; i++) {
        v = (v << 3) | (uint64_t)(field[i] - '0');
    }
    return v;
}

/* Copies a possibly-not-NUL-terminated fixed-width tar field into a
 * NUL-terminated buffer. kvsnprintf() has no %.Ns precision support
 * (see klib/printf.c's header comment on what it does and doesn't
 * implement), so this -- not a "%.100s"-style format string -- is how
 * every name/prefix field gets bounded before it's used with "%s". */
static void field_copy(char *dst, size_t dst_size, const char *src, size_t src_len) {
    size_t max = dst_size > 0 ? dst_size - 1 : 0;
    size_t n = MIN(max, src_len);
    size_t real = 0;
    while (real < n && src[real] != '\0') {
        real++;
    }
    memcpy(dst, src, real);
    dst[real] = '\0';
}

uint32_t initrd_unpack(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t files = 0;
    uint64_t off = 0;

    while (off + sizeof(struct ustar_header) <= size) {
        const struct ustar_header *h = (const struct ustar_header *)(p + off);

        if (h->name[0] == '\0') {
            break; /* end-of-archive marker: a zeroed header block */
        }
        if (memcmp(h->magic, "ustar", 5) != 0) {
            kprintf("[initrd] not a ustar archive at offset %lu, stopping\n", off);
            break;
        }

        char name_buf[101], prefix_buf[156], path[256];
        field_copy(name_buf, sizeof(name_buf), h->name, sizeof(h->name));
        field_copy(prefix_buf, sizeof(prefix_buf), h->prefix, sizeof(h->prefix));
        if (prefix_buf[0] != '\0') {
            ksnprintf(path, sizeof(path), "%s/%s", prefix_buf, name_buf);
        } else {
            ksnprintf(path, sizeof(path), "%s", name_buf);
        }

        uint64_t fsize = parse_octal(h->size, sizeof(h->size));
        off += sizeof(struct ustar_header);

        if (h->typeflag == TAR_TYPE_DIR) {
            vfs_lookup_or_create(path, VNODE_DIR);
        } else if (h->typeflag == TAR_TYPE_FILE || h->typeflag == TAR_TYPE_AFILE) {
            struct vnode *n = vfs_lookup_or_create(path, VNODE_FILE);
            if (n != NULL && n->ops->write != NULL && off + fsize <= size) {
                n->ops->write(n, 0, p + off, fsize);
            }
            files++;
        }
        /* Any other typeflag (symlink, device node, ...) is silently
         * skipped -- tmpfs has no concept of them yet. */

        off += ALIGN_UP(fsize, 512);
    }

    kprintf("[initrd] unpacked %u file(s)\n", files);
    return files;
}
