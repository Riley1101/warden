#pragma once
#include <stddef.h>

/* Maximum length of a path, including the terminating NUL. */
#define PATH_MAX 256

/*
 * Normalize an absolute path into canonical form:
 *   - collapse repeated slashes        "/a//b"   -> "/a/b"
 *   - drop "." components              "/a/./b"  -> "/a/b"
 *   - resolve ".." components          "/a/../b" -> "/b"
 *   - ".." at the root stays at root   "/../etc" -> "/etc"
 *   - drop the trailing slash          "/a/b/"   -> "/a/b"
 *
 * Purely lexical: does not look at the filesystem.
 * `out` must hold at least `out_size` bytes. `in` and `out` must not overlap.
 *
 * Returns 0 on success, -EINVAL if `in` is empty or not absolute,
 * -ENAMETOOLONG if the result (or an intermediate step) doesn't fit.
 *
 * Must stay free of hardware code: it is compiled into the kernel AND into
 * host unit tests with sanitizers.
 */
int path_normalize(const char *in, char *out, size_t out_size);
