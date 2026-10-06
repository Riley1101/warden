#include "check.h"
#include "fmt.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

/* Format into a fresh buffer; check both the text and the returned length. */
#define FMT(want, ...)                                                         \
  do {                                                                         \
    char out_[128];                                                            \
    int r_ = ksnprintf(out_, sizeof out_, __VA_ARGS__);                        \
    CHECK_STR(out_, want);                                                     \
    CHECK(r_ == (int)strlen(want));                                            \
  } while (0)

static void test_plain(void) {
  FMT("", "%s", "");
  FMT("hello", "hello");
  FMT("100%", "100%%");
  FMT("a-b", "%c-%c", 'a', 'b');
}

static void test_signed(void) {
  FMT("0", "%d", 0);
  FMT("42", "%d", 42);
  FMT("-42", "%i", -42);
  FMT("-2147483648", "%d", INT_MIN);
  FMT("2147483647", "%d", INT_MAX);
  FMT("-9223372036854775808", "%ld", LONG_MIN);
  FMT("9223372036854775807", "%lld", LLONG_MAX);
}

static void test_unsigned(void) {
  FMT("4294967295", "%u", UINT_MAX);
  FMT("18446744073709551615", "%lu", ULONG_MAX);
  FMT("ff", "%x", 255);
  FMT("ffffffff", "%x", -1); /* %x reads an unsigned int, not a long */
  FMT("ffffffff80000000", "%lx", 0xffffffff80000000UL);
}

static void test_pointer(void) {
  FMT("0x0", "%p", (void *)0);
  FMT("0xdeadbeef", "%p", (void *)(uintptr_t)0xdeadbeef);
  FMT("    0x1000", "%10p", (void *)(uintptr_t)0x1000);
}

static void test_width(void) {
  FMT("0000000000001234", "%016lx", 0x1234UL);
  FMT("   42", "%5d", 42);
  FMT("42   |", "%-5d|", 42);
  FMT("-0042", "%05d", -42);
  FMT("  -42", "%5d", -42);
  FMT("123456", "%3d", 123456); /* width is a minimum, never truncates */
  FMT("   hi", "%5s", "hi");
  FMT("hi   |", "%-5s|", "hi");
}

/*
 * The next tests feed deliberately bad input. The format attribute on
 * ksnprintf rejects these at compile time (that's the point of it), so
 * silence it here only.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-overflow"
#pragma GCC diagnostic ignored "-Wformat-extra-args"

static void test_strings(void) {
  FMT("(null)", "%s", (const char *)NULL);
  FMT("a b c", "%s %s %s", "a", "b", "c");
}

static void test_unknown(void) {
  FMT("%q", "%q");
  FMT("x%", "x%");     /* lone trailing '%' */
  FMT("x%05", "x%05"); /* trailing incomplete spec */
}

#pragma GCC diagnostic pop

static void test_truncation(void) {
  char buf[4];
  CHECK(ksnprintf(buf, sizeof buf, "hello") == 5);
  CHECK_STR(buf, "hel");

  CHECK(ksnprintf(buf, sizeof buf, "%d", 123456) == 6);
  CHECK_STR(buf, "123");

  char one[1] = {'x'};
  CHECK(ksnprintf(one, sizeof one, "hello") == 5);
  CHECK(one[0] == '\0');

  /* size 0: nothing written (ASan would catch a write through NULL) */
  CHECK(ksnprintf(NULL, 0, "hello %d", 42) == 8);
}

/* Compare against the host libc on the subset where we promise to agree. */
static void test_random(void) {
  srand(1);
  for (int i = 0; i < 10000; i++) {
    long v = (long)(((unsigned long)rand() << 33) ^
                    ((unsigned long)rand() << 2) ^ (unsigned long)rand());
    if (rand() & 1)
      v = (long)(0UL - (unsigned long)v); /* -v overflows on LONG_MIN */
    char got[64], want[64];

    ksnprintf(got, sizeof got, "%ld|%lu|%lx|%020ld", v, (unsigned long)v,
              (unsigned long)v, v);
    snprintf(want, sizeof want, "%ld|%lu|%lx|%020ld", v, (unsigned long)v,
             (unsigned long)v, v);
    CHECK_STR(got, want);

    int iv = (int)v;
    ksnprintf(got, sizeof got, "%d|%u|%x|%-8d|", iv, (unsigned)iv,
              (unsigned)iv, iv);
    snprintf(want, sizeof want, "%d|%u|%x|%-8d|", iv, (unsigned)iv,
             (unsigned)iv, iv);
    CHECK_STR(got, want);
  }
}

int main(void) {
  test_plain();
  test_signed();
  test_unsigned();
  test_pointer();
  test_width();
  test_strings();
  test_unknown();
  test_truncation();
  test_random();
  return CHECK_DONE();
}
