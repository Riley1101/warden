#include "t.h"

/* Run as: args a bc */
int main(int argc, char **argv) {
  if (argc != 3)
    return 1;
  if (!streq(argv[0], "args"))
    return 2;
  if (!streq(argv[1], "a") || !streq(argv[2], "bc"))
    return 3;
  if (argv[3] != NULL || argv[4] != NULL) /* argv end, empty envp */
    return 4;
  return 0;
}
