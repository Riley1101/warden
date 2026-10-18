#pragma once

#include <stddef.h>

/*
 * Walk a ustar archive (the initramfs). Pure: reads the archive only, and
 * every header field is checked before it is trusted, so it is host-tested
 * on malformed input. Must stay free of hardware code.
 */

enum tar_type { TAR_FILE, TAR_DIR };

/* Called per member: its absolute path ("./bin/sh" and "bin/sh" are both
 * "/bin/sh"; a directory's trailing slash dropped), and contents (files). */
typedef int (*tar_fn)(void *ctx, const char *path, enum tar_type type,
                      const void *data, size_t size);

/*
 * Call `fn` for each regular file and directory, in archive order; other
 * member types (links, devices) are skipped. Stops at the end-of-archive
 * block. Returns 0; what `fn` returned if not 0 (which stops the walk);
 * or -EINVAL at a malformed header (bad magic or checksum, a size past the
 * end) or a name too long for PATH_MAX, after the members before it.
 */
int tar_each(const void *tar, size_t len, tar_fn fn, void *ctx);
