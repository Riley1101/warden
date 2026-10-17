#pragma once

/* Shared by the ring 3 test programs. Each reports through its exit code:
 * 0 passes, anything else is the failing step. */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The call failed with -1 and set errno to `e`. */
#define FAILS(call, e) ((call) == -1 && errno == (e))
