#ifdef WARDEN_TEST
#include "ktest.h"
#include "fmt.h"
#include "path.h"

int test_sanity(void) {
    KASSERT(1 + 1 == 2);
    KASSERT(2 * 2 == 4);
    return 0;
}

int test_path_in_kernel(void) {
    char out[PATH_MAX];
    KASSERT(path_normalize("/home/../bin//ls", out, sizeof out) == 0);
    KASSERT(streq(out, "/bin/ls"));
    return 0;
}

int test_fmt_in_kernel(void) {
    char out[64];
    KASSERT(ksnprintf(out, sizeof out, "%d %s %016lx", -7, "ok",
                      0xffffffff80000000UL) == 22);
    KASSERT(streq(out, "-7 ok ffffffff80000000"));
    return 0;
}

#endif /* ifdef WARDEN_TEST */
