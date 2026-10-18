#include "t.h"

/* Run as: args a bc */
int main(int argc, char **argv) {
  if (argc != 3)
    return 1;
  if (strcmp(argv[0], "args") != 0)
    return 2;
  if (strcmp(argv[1], "a") != 0 || strcmp(argv[2], "bc") != 0)
    return 3;
  if (argv[3] != NULL || argv[4] != NULL) /* argv end, empty envp */
    return 4;
  return 0;
}
