#pragma once

#include <abi/proc.h> /* struct procinfo, P_RUNNING & co. */
#include <stddef.h>

/* Describe up to `max` processes, starting with this one; returns how
 * many were written, or -1 and errno. */
int ps(struct procinfo *buf, size_t max);
