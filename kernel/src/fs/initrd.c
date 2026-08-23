#include "fs/initrd.h"
#include "debug/log.h"
#include "fs/vfs.h"

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

#define TAR_TYPE_FILE '0'
#define TAR_TYPE_AFILE '\0' /* old tar's "regular file", pre-POSIX */
#define TAR_TYPE_DIR '5'

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
static void field_copy(char *dst, size_t dst_size, const char *src,
                       size_t src_len) {
  size_t max = dst_size > 0 ? dst_size - 1 : 0;
  size_t n = MIN(max, src_len);
  size_t real = 0;
  while (real < n && src[real] != '\0') {
    real++;
  }
  memcpy(dst, src, real);
  dst[real] = '\0';
}

/* Same FNV-1a 32-bit hash fs/graph.c's own disk snapshot uses (see
 * that file's fnv1a32()) -- kept as its own copy here rather than
 * shared, same "small, self-contained routine, not worth threading a
 * shared header through two unrelated subsystems for" reasoning
 * fs/vfs.c already gives for its own split_path() copy. Purely a
 * boot-log diagnostic: printed per unpacked file so a corrupted or
 * truncated initrd.tar (a bad download, a flaky build, a botched
 * `dd`) shows up as a visible number in the boot log instead of
 * manifesting later as a mysterious crash the first time something
 * tries to `run` the affected binary. */
static uint32_t fnv1a32(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 0x01000193u;
  }
  return h;
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

    if (path[0] == '\0') {
      /* A degenerate member: empty name AND empty prefix, but not
       * the real end-of-archive zeroed block (already caught
       * above by h->name[0] == '\0'). Shouldn't happen from any
       * real tar implementation; skip its data region rather
       * than handing vfs_lookup_or_create() an empty path (which
       * resolves to the root itself) and silently doing nothing
       * useful with whatever typeflag/size followed. */
      kprintf("[initrd] skipping archive member with an empty path\n");
    } else if (fsize > size - off) {
      /* Truncated archive: this member claims more data than is
       * actually left. Stop here rather than the old behaviour
       * of silently creating an empty/truncated vnode and still
       * counting it as "unpacked" -- a corrupt initrd.tar should
       * be a loud boot-log failure, not a phantom success that
       * only shows up later as a 0-byte binary refusing to run. */
      kprintf("[initrd] '%s' claims %lu byte(s), more than the %lu "
              "remaining in the archive -- stopping (truncated or "
              "corrupt initrd.tar)\n",
              path, fsize, size - off);
      break;
    } else if (h->typeflag == TAR_TYPE_DIR) {
      if (vfs_lookup_or_create(path, VNODE_DIR) == NULL) {
        kprintf("[initrd] failed to create directory '%s' (out of "
                "memory?) -- anything nested under it will fail too\n",
                path);
      }
    } else if (h->typeflag == TAR_TYPE_FILE || h->typeflag == TAR_TYPE_AFILE) {
      bool pre_existing = vfs_lookup_path(path) != NULL;
      struct vnode *n = vfs_lookup_or_create(path, VNODE_FILE);
      if (n == NULL) {
        kprintf("[initrd] failed to create '%s' (out of memory?) -- "
                "skipping\n",
                path);
      } else if (n->ops->write == NULL) {
        kprintf("[initrd] '%s' resolved to a vnode with no write "
                "operation -- skipping\n",
                path);
      } else {
        size_t put = n->ops->write(n, 0, p + off, fsize);
        if (put != fsize) {
          kprintf("[initrd] short write seeding '%s' (%lu of %lu "
                  "byte(s)) -- binary is likely corrupt\n",
                  path, (uint64_t)put, fsize);
        } else {
          uint32_t sum = fnv1a32(p + off, fsize);
          kprintf("[initrd] seeded '%s' (%lu byte(s), fnv1a32=0x%08x)%s\n",
                  path, fsize, sum,
                  pre_existing ? " -- overwrote a pre-existing entry" : "");
        }
        files++;
      }
    }
    /* Any other typeflag (symlink, device node, ...) is silently
     * skipped -- tmpfs has no concept of them yet. */

    uint64_t aligned = ALIGN_UP(fsize, 512);
    if (off + aligned < off) {
      kprintf("[initrd] archive offset overflow after '%s', stopping\n", path);
      break;
    }
    off += aligned;
  }

  kprintf("[initrd] unpacked %u file(s)\n", files);
  return files;
}
