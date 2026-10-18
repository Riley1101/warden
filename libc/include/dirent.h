#pragma once

#include <abi/fs.h> /* struct dirent */

/*
 * The next entry of the directory open (O_RDONLY) at `fd`: 1 and *d
 * filled, 0 after the last, or -1 and errno. Simpler than opendir/readdir:
 * no DIR, just the descriptor.
 */
int getdent(int fd, struct dirent *d);
