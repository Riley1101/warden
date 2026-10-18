#include "tar.h"

#include <abi/errno.h>
#include <kstring.h>
#include <path.h>
#include <stdint.h>

#define BLOCK 512

struct header {
  char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], chksum[8];
  char typeflag, linkname[100], magic[6], version[2], uname[32], gname[32];
  char devmajor[8], devminor[8], prefix[155], pad[12];
};

_Static_assert(sizeof(struct header) == BLOCK, "ustar header");

/* An octal field: digits, optionally padded with spaces/NULs. -1 if bad. */
static int64_t octal(const char *f, size_t n) {
  int64_t v = 0;
  size_t i = 0;
  while (i < n && f[i] == ' ')
    i++;
  size_t start = i;
  for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
    if (v > (INT64_MAX >> 3))
      return -1;
    v = v * 8 + (f[i] - '0');
  }
  if (i == start)
    return -1;
  for (; i < n; i++)
    if (f[i] != ' ' && f[i] != '\0')
      return -1;
  return v;
}

/* The header's checksum: its bytes summed, with chksum read as spaces. */
static int checksum_ok(const struct header *h) {
  const uint8_t *b = (const uint8_t *)h;
  int64_t sum = 0;
  for (size_t i = 0; i < BLOCK; i++)
    sum += (i >= offsetof(struct header, chksum) &&
            i < offsetof(struct header, chksum) + sizeof h->chksum)
               ? ' '
               : b[i];
  return octal(h->chksum, sizeof h->chksum) == sum;
}

/* A fixed-width field as a string: up to n bytes, NUL-terminated or not. */
static size_t field_len(const char *f, size_t n) {
  size_t i = 0;
  while (i < n && f[i] != '\0')
    i++;
  return i;
}

/* "/" + prefix + "/" + name into out, without a leading "./" or a trailing
 * "/". -EINVAL if it doesn't fit. */
static int member_path(const struct header *h, char *out, size_t size) {
  size_t plen = field_len(h->prefix, sizeof h->prefix);
  size_t nlen = field_len(h->name, sizeof h->name);
  const char *name = h->name;
  if (plen == 0 && nlen >= 2 && name[0] == '.' && name[1] == '/') {
    name += 2;
    nlen -= 2;
  }
  while (nlen > 0 && name[nlen - 1] == '/')
    nlen--;
  if (1 + plen + 1 + nlen + 1 > size)
    return -EINVAL;
  size_t n = 0;
  out[n++] = '/';
  if (plen > 0) {
    memcpy(out + n, h->prefix, plen);
    n += plen;
    out[n++] = '/';
  }
  memcpy(out + n, name, nlen);
  n += nlen;
  if (n > 1 && out[n - 1] == '/')
    n--; /* "./" alone: the root */
  out[n] = '\0';
  return 0;
}

int tar_each(const void *tar, size_t len, tar_fn fn, void *ctx) {
  if (tar == NULL)
    return len == 0 ? 0 : -EINVAL;
  const uint8_t *base = tar;
  for (size_t off = 0; len - off >= BLOCK;) {
    const struct header *h = (const struct header *)(base + off);
    if (h->name[0] == '\0')
      return 0; /* end-of-archive block */
    if (memcmp(h->magic, "ustar", 5) != 0 || !checksum_ok(h))
      return -EINVAL;
    int64_t sz = octal(h->size, sizeof h->size);
    if (sz < 0 || (uint64_t)sz > len - off - BLOCK)
      return -EINVAL; /* contents run past the end */

    int file = h->typeflag == '0' || h->typeflag == '\0';
    if (file || h->typeflag == '5') {
      char path[PATH_MAX];
      if (member_path(h, path, sizeof path) != 0)
        return -EINVAL;
      int err = fn(ctx, path, file ? TAR_FILE : TAR_DIR, base + off + BLOCK,
                   file ? (size_t)sz : 0);
      if (err != 0)
        return err;
    }
    /* skip header + contents rounded up to a block, guarding the wrap */
    size_t skip = BLOCK + (((size_t)sz + BLOCK - 1) / BLOCK) * BLOCK;
    if (skip > len - off)
      return 0; /* no end-of-archive block, but nothing more either */
    off += skip;
  }
  return 0;
}
