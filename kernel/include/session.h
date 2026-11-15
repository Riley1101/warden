#pragma once

#include <stdint.h>

/*
 * Agent sessions: how many processes each has, against its limit. A
 * process counts from when it joins (mark_agent, or fork from an agent)
 * until its thread is freed, zombie time included: a zombie still holds
 * its address space and stack, so a fork bomb that never reaps is bounded
 * too. A session is forgotten when its last process goes; marking into
 * it again starts it afresh.
 *
 * Callers keep interrupts off, so it needs no lock. Pure: compiled into
 * the kernel AND into host unit tests with sanitizers.
 */

#define SESSION_MAX 16 /* sessions alive at once */

/* A process joins session `id` from outside it, by mark_agent: the
 * session is made with limit `max_procs` if new, or its limit lowered to
 * that if lower. 0, -EAGAIN at the limit (the limit stays lowered),
 * -ENOMEM with no free slot, -EINVAL for a limit of 0. */
int session_enter(uint32_t id, uint32_t max_procs);

/* One more process in `id`, by fork: 0, or -EAGAIN at the limit. */
int session_spawn(uint32_t id);

/* Lower `id`'s limit to `max_procs`, never raising it: an agent marking
 * itself again. 0 does nothing. */
void session_lower(uint32_t id, uint32_t max_procs);

/* One process of `id` freed. */
void session_leave(uint32_t id);

/* `id`'s limit; 0 if it doesn't exist. */
uint32_t session_max(uint32_t id);

/* How many processes `id` holds now; 0 if it doesn't exist. */
uint32_t session_nproc(uint32_t id);
