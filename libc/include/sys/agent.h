#pragma once

#include <abi/cap.h> /* CAP_FS_READ & co. */
#include <stdint.h>

/* Make this process, and all it starts from now on, an agent of
 * `session` holding `caps`, writing only under `scope`. It can only
 * narrow: asking for more than it holds fails with EPERM. 0, or -1 and
 * errno. */
int mark_agent(uint32_t session, uint32_t caps, const char *scope);
