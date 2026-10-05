#pragma once

#include <serial.h>

#ifdef WARDEN_TEST

struct ktest {
    const char *name;
    int(*fn)(void);
};


#define KTEST_STR_(x) #x
#define KTEST_STR(x) KTEST_STR_(x)

#define KASSERT(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            serial_puts(COM1, "    assert failed: " #cond                   \
                              " (" __FILE__ ":" KTEST_STR(__LINE__) ")\n"); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

__attribute__((noreturn)) void ktest_run_all(void);
__attribute__((noreturn)) void ktest_fail(void);

#endif // WARDEN_TEST
