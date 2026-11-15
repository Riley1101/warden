#include "session.h"

#include <abi/errno.h>
#include <stddef.h>

static struct session {
  uint32_t id;
  uint32_t nproc; /* 0: the slot is free */
  uint32_t max_procs;
} sessions[SESSION_MAX];

static struct session *find(uint32_t id) {
  for (size_t i = 0; i < SESSION_MAX; i++)
    if (sessions[i].nproc > 0 && sessions[i].id == id)
      return &sessions[i];
  return NULL;
}

/* Count one more in `s`, if its limit allows. */
static int add(struct session *s) {
  if (s->nproc >= s->max_procs)
    return -EAGAIN;
  s->nproc++;
  return 0;
}

int session_enter(uint32_t id, uint32_t max_procs) {
  if (max_procs == 0)
    return -EINVAL;
  struct session *s = find(id);
  if (s != NULL) {
    session_lower(id, max_procs);
    return add(s);
  }
  for (size_t i = 0; i < SESSION_MAX; i++) {
    if (sessions[i].nproc == 0) {
      sessions[i] = (struct session){id, 1, max_procs};
      return 0;
    }
  }
  return -ENOMEM;
}

int session_spawn(uint32_t id) {
  struct session *s = find(id);
  return s != NULL ? add(s) : -EAGAIN; /* an agent's session always exists */
}

void session_lower(uint32_t id, uint32_t max_procs) {
  struct session *s = find(id);
  if (s != NULL && max_procs != 0 && max_procs < s->max_procs)
    s->max_procs = max_procs;
}

void session_leave(uint32_t id) {
  struct session *s = find(id);
  if (s != NULL)
    s->nproc--; /* at 0 the slot is free again */
}

uint32_t session_nproc(uint32_t id) {
  struct session *s = find(id);
  return s != NULL ? s->nproc : 0;
}

uint32_t session_max(uint32_t id) {
  struct session *s = find(id);
  return s != NULL ? s->max_procs : 0;
}
