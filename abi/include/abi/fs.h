#pragma once

#include <stdint.h>

/*
 * The filesystem: one in-memory tree of files and directories, filled from
 * the initramfs at boot. Paths are absolute ("/bin/sh"); there is no
 * working directory, so a relative path is ENOENT. No permissions yet.
 */

#define NAME_MAX 63 /* bytes in one path component */

/* open() flags, as on Linux. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0100  /* create the file if it doesn't exist */
#define O_TRUNC   01000 /* empty it on open (needs write access) */
#define O_APPEND  02000 /* every write goes at the end */

/* What a node is. */
#define FT_FILE 1
#define FT_DIR  2

struct stat {
  uint32_t type;    /* FT_FILE or FT_DIR */
  uint32_t agent;   /* 1: an agent wrote it, and exec runs it as that agent */
  uint64_t ino;     /* unique while the node exists */
  uint64_t size;    /* bytes (files); entries (directories) */
  uint32_t session; /* agent only: the writer's session */
  uint32_t caps;    /* agent only: the caps it runs with (abi/cap.h) */
};

/* One directory entry, as getdent() returns it. */
struct dirent {
  uint32_t type; /* FT_FILE or FT_DIR */
  char name[NAME_MAX + 1];
};
