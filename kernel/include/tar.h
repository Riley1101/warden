#pragma once

#include <stddef.h>

/*
 * Read-only lookup in a ustar archive (the initramfs). Pure: reads the
 * archive only, and every header field is checked before it is trusted,
 * so it is host-tested on malformed input. Must stay free of hardware code.
 */

/*
 * Find the regular file `path` ("/bin/sh" matches the member "bin/sh" or
 * "./bin/sh"). Sets *data and *size and returns 0; -ENOENT if there is no
 * such file, or the archive is malformed before it is reached.
 */
int tar_find(const void *tar, size_t len, const char *path, const void **data,
             size_t *size);
