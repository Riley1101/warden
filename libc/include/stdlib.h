#pragma once

#include <stddef.h>

/* The heap, over sbrk. malloc'd blocks are 16-byte aligned; a bad free
 * (double, or of a pointer malloc never gave out) ends the program. */
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);

__attribute__((noreturn)) void exit(int code);

/* Decimal, with an optional sign; stops at the first non-digit. */
int atoi(const char *s);
