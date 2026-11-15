#pragma once

#include <abi/cap.h> /* CAP_FS_READ & co. */
#include <stdint.h>

/* Make this process, and all it starts from now on, an agent of
 * `session` holding `caps`, writing only under `scope`, with at most
 * `max_procs` processes in the session (this one included). It can only
 * narrow: asking for more than it holds fails with EPERM, a bigger limit
 * is ignored, and a session already full is EAGAIN. 0, or -1 and errno. */
int mark_agent(uint32_t session, uint32_t caps, const char *scope,
               uint32_t max_procs);

/* Kill every process in agent session `session`: how many, or -1 and
 * errno (ECAPDENIED for an agent). */
int kill_session(uint32_t session);
