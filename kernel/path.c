#include "path.h"

#include <abi/errno.h>

int path_normalize(const char *in, char *out, size_t out_size) {
  if (in == NULL || out == NULL || out_size < 2)
    return -EINVAL;
  if (in[0] != '/')
    return -EINVAL; /* empty or relative */

  size_t len = 0; /* bytes written to out, without the NUL */
  const char *p = in;

  while (*p != '\0') {
    /* skip any run of slashes */
    while (*p == '/')
      p++;
    if (*p == '\0')
      break;

    /* find the end of this component */
    const char *start = p;
    while (*p != '\0' && *p != '/')
      p++;
    size_t clen = (size_t)(p - start);

    if (clen == 1 && start[0] == '.')
      continue; /* "." : stay here */

    if (clen == 2 && start[0] == '.' && start[1] == '.') {
      /* "..": remove the last component (at the root, stay at root) */
      while (len > 0 && out[len - 1] != '/')
        len--;
      if (len > 0)
        len--; /* drop the slash before it too */
      continue;
    }

    /* normal component: append "/" + name, keeping room for the NUL */
    if (len + 1 + clen + 1 > out_size)
      return -ENAMETOOLONG;
    out[len++] = '/';
    for (size_t i = 0; i < clen; i++)
      out[len++] = start[i];
  }

  if (len == 0)
    out[len++] = '/'; /* everything resolved to the root */
  out[len] = '\0';
  return 0;
}
