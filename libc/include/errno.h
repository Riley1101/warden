#pragma once

#include <abi/errno.h>

/* Set by a failing call (which returns -1, or NULL); never cleared. */
extern int errno;
