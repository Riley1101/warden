#include "cap.h"

#include <abi/errno.h>
#include <agent.h>
#include <kstring.h>

int in_scope(const char *scope, const char *path) {
  if (scope[0] == '/' && scope[1] == '\0')
    return 1; /* the root holds everything */
  size_t n = 0;
  while (scope[n] != '\0' && scope[n] == path[n]) /* stops at path's NUL */
    n++;
  if (scope[n] != '\0')
    return 0;
  /* a whole component: the path ends here, or goes on below it */
  return path[n] == '\0' || path[n] == '/';
}

int cap_allows(const struct label *l, uint32_t need) {
  if (need == CAP_ANY || l->origin == ORIGIN_HUMAN)
    return 0;
  return (l->caps & need) == need ? 0 : -ECAPDENIED;
}

int cap_path(const struct label *l, const char *path, uint32_t need) {
  if (l->origin == ORIGIN_HUMAN)
    return 0; /* the fs reports what's wrong with the path itself */
  int err = cap_allows(l, need);
  if (err != 0)
    return err;
  char norm[PATH_MAX];
  err = path_normalize(path, norm, sizeof norm);
  if (err != 0)
    return err == -EINVAL ? -ENOENT : err; /* as the fs says: no cwd */
  if (in_scope(AGENT_PATH, norm))
    return -ECAPDENIED;
  if ((need & CAP_FS_WRITE) && !in_scope(l->scope, norm))
    return -ECAPDENIED;
  return 0;
}

int label_narrow(struct label *l, uint32_t session, uint32_t caps,
                 const char *scope) {
  char norm[PATH_MAX];
  int err = path_normalize(scope, norm, sizeof norm);
  if (err != 0)
    return err;

  if (l->origin == ORIGIN_AGENT) {
    if (session != l->session || !in_scope(l->scope, norm))
      return -EPERM;
    caps &= l->caps;
  }

  l->origin = ORIGIN_AGENT;
  l->session = session;
  l->caps = caps & CAP_ALL;
  memcpy(l->scope, norm, strlen(norm) + 1);
  return 0;
}
