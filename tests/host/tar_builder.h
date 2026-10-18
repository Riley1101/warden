#pragma once

/* Build ustar archives by hand for host tests: header + contents, as GNU
 * tar --format=ustar does. ar_reset(), add() members, finish(). */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t ar[16 * 512];
static size_t ar_len;

static uint8_t *add(const char *prefix, const char *name, char type,
                    const char *body) {
  uint8_t *h = ar + ar_len;
  memset(h, 0, 512);
  size_t n = strlen(body);
  memcpy(h, name, strlen(name));
  if (prefix != NULL)
    memcpy(h + 345, prefix, strlen(prefix));
  snprintf((char *)h + 100, 8, "%07o", 0644);
  snprintf((char *)h + 124, 12, "%011o", (unsigned)n & 07777777);
  h[156] = type;
  memcpy(h + 257, "ustar", 6);
  memcpy(h + 263, "00", 2);
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++)
    sum += h[i];
  snprintf((char *)h + 148, 8, "%06o", sum);
  memcpy(h + 512, body, n);
  ar_len += 512 + (n + 511) / 512 * 512;
  return h;
}

static void finish(void) {
  memset(ar + ar_len, 0, 1024);
  ar_len += 1024;
}

static void ar_reset(void) {
  memset(ar, 0, sizeof ar);
  ar_len = 0;
}
