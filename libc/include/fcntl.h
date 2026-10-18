#pragma once

#include <abi/fs.h> /* O_RDONLY & co. */

/* A descriptor for `path`, or -1 and errno. */
int open(const char *path, int flags);
