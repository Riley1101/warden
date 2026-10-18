#pragma once

#include <abi/wait.h>
#include <sys/types.h>

/* A dead child's pid, its status (WIFEXITED & co.) stored if `status`. */
pid_t wait(int *status);
