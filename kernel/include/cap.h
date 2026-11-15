#pragma once

#include <abi/cap.h>
#include <path.h>
#include <stdint.h>

/*
 * The provenance label every process carries. fork copies it, exec keeps
 * it, and it can only narrow: nothing a process does widens its own.
 *
 * A zeroed label is a human process with no limits, so the boot thread
 * and everything the kernel starts need no setup.
 *
 * Must stay free of hardware code: it is compiled into the kernel AND
 * into host unit tests with sanitizers.
 */

enum origin {
  ORIGIN_HUMAN = 0,
  ORIGIN_AGENT,
};

struct label {
  enum origin origin;
  uint32_t session;     /* agent only: which agent session it belongs to */
  uint32_t caps;        /* agent only: CAP_* bits it holds */
  char scope[PATH_MAX]; /* agent only: normalized; writes stay under it */
};

/* What a syscall table entry needs, beyond the CAP_* bits: CAP_ANY lets
 * every process through. 0 is left undeclared, so dispatch refuses it. */
#define CAP_UNDECLARED 0u
#define CAP_ANY        (1u << 31)
/* Humans only. Never in CAP_ALL, so label_narrow can't hand it out. */
#define CAP_HUMAN      (1u << 30)

/* 1 if `path` is `scope` or below it, by whole components: "/home/work"
 * holds "/home/work/a" but not "/home/workevil". Both normalized. */
int in_scope(const char *scope, const char *path);

/* 0 if `l` holds `need` (CAP_ANY, or CAP_* bits), else -ECAPDENIED. */
int cap_allows(const struct label *l, uint32_t need);

/*
 * 0 if `l` may touch `path` with `need` (CAP_FS_READ, CAP_FS_WRITE, or
 * both): the capabilities, and for a write, the path inside its scope.
 * `path` is normalized here first, so "/home/work/../../bin" is judged
 * as "/bin". The agent channel is never an agent's, whatever it holds:
 * it could forge replies to the host. A human passes untouched.
 * -ECAPDENIED, or the error the fs would give the path (-ENOENT for a
 * relative one, -ENAMETOOLONG).
 */
int cap_path(const struct label *l, const char *path, uint32_t need);

/*
 * Make `l` an agent label for `session`, holding `caps` within `scope`,
 * but never more than it holds now: the caps are intersected with the
 * current ones, and the scope must sit inside the current one. An agent
 * can't move to another session. 0, -EPERM, or path_normalize's error;
 * `l` is unchanged on failure.
 */
int label_narrow(struct label *l, uint32_t session, uint32_t caps,
                 const char *scope);
