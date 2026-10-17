#pragma once

/*
 * The status wait() reports, encoded as on Unix: an exit code in bits
 * 8..15, or the signal that killed it in bits 0..6. There are no signal
 * handlers; a "signal" here only names why the process died.
 */
#define SIGILL  4  /* invalid instruction */
#define SIGFPE  8  /* arithmetic fault */
#define SIGKILL 9  /* kill() */
#define SIGSEGV 11 /* bad memory access, or any other fault */

#define W_EXITCODE(code) (((code) & 0xff) << 8)
#define W_SIGNALED(sig)  ((sig) & 0x7f)

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s)    ((s) & 0x7f)
