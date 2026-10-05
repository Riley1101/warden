#pragma once

#include <stdio.h>
#include <string.h>

static int check_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      check_failures++;                                                        \
    }                                                                          \
  } while (0)

#define CHECK_STR(got, want)                                                   \
  do {                                                                         \
    if (strcmp((got), (want)) != 0) {                                          \
      fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__,  \
              (got), (want));                                                  \
      check_failures++;                                                        \
    }                                                                          \
  } while (0)

/* Call at the end of main(). */
#define CHECK_DONE()                                                           \
  (printf("%s: %s\n", __FILE__, check_failures ? "FAILED" : "ok"),             \
   check_failures ? 1 : 0)
