#pragma once

/*
 * Capabilities: what an agent process may do. A human process is not
 * limited by these; an agent process may do only what its capset holds.
 * SYS_mark_agent takes a capset built from these bits.
 */

#define CAP_FS_READ  (1u << 0) /* open for reading, stat, list a directory */
#define CAP_FS_WRITE (1u << 1) /* create, write, unlink, rename, mkdir: in scope */
#define CAP_SPAWN    (1u << 2) /* fork and exec */
#define CAP_KILL     (1u << 3) /* kill a process in its own session */

#define CAP_ALL (CAP_FS_READ | CAP_FS_WRITE | CAP_SPAWN | CAP_KILL)
