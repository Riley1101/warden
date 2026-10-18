#pragma once

#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
/* Copies at most n - 1 bytes and always terminates (if n > 0); returns
 * strlen(src), so a result >= n means it was truncated. */
size_t strlcpy(char *dst, const char *src, size_t n);
char *strdup(const char *s); /* malloc'd; NULL if out of memory */
