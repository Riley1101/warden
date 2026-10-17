#pragma once

#include <abi/wait.h> /* the SIG* numbers */
#include <sys/types.h>

/* End process `pid`. There are no handlers: SIGKILL is the only signal
 * that can be sent (anything else fails with EINVAL). */
int kill(pid_t pid, int sig);
