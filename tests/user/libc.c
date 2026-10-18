#include "t.h"

static int strings(void) {
  char buf[32];
  if (strlen("") != 0 || strlen("abc") != 3)
    return 1;
  if (strcmp("a", "a") != 0 || strcmp("a", "b") >= 0 || strcmp("b", "a") <= 0 ||
      strcmp("ab", "a") <= 0 || strcmp("\xff", "a") <= 0) /* unsigned */
    return 2;
  if (strncmp("abcx", "abcy", 3) != 0 || strncmp("ab", "ac", 2) >= 0)
    return 3;
  const char *s = "a/b/c";
  if (strchr(s, '/') != s + 1 || strrchr(s, '/') != s + 3 ||
      strchr(s, 'z') != NULL || strchr(s, '\0') != s + 5)
    return 4;
  strcpy(buf, "ab");
  strcat(buf, "cd");
  if (strcmp(buf, "abcd") != 0)
    return 5;
  if (strlcpy(buf, "truncated", 4) != 9 || strcmp(buf, "tru") != 0)
    return 6;
  memset(buf, 'x', 8);
  memmove(buf + 2, buf, 4); /* overlapping */
  memcpy(buf, "01234567", 8);
  memmove(buf + 1, buf, 5);
  if (memcmp(buf, "00123467", 8) != 0)
    return 7;
  if (atoi("42") != 42 || atoi("-7x") != -7 || atoi("") != 0)
    return 8;
  return 0;
}

static int format(void) {
  char buf[16];
  int n = snprintf(buf, sizeof buf, "%d-%s-%x", -5, "ok", 255);
  if (n != 8 || strcmp(buf, "-5-ok-ff") != 0)
    return 1;
  n = snprintf(buf, 4, "%s", "truncated"); /* returns what it wanted */
  if (n != 9 || strcmp(buf, "tru") != 0)
    return 2;
  if (snprintf(buf, sizeof buf, "%5d|%-3s|", 42, "a") != 10 ||
      strcmp(buf, "   42|a  |") != 0)
    return 3;
  return 0;
}

static int heap(void) {
  /* Many blocks, each holding its own byte, then all freed. */
  static char *p[200];
  for (int i = 0; i < 200; i++) {
    p[i] = malloc((size_t)(i * 37 % 3000) + 1);
    if (p[i] == NULL || (unsigned long)p[i] % 16 != 0)
      return 1;
    memset(p[i], i, (size_t)(i * 37 % 3000) + 1);
  }
  for (int i = 0; i < 200; i += 2)
    free(p[i]);
  for (int i = 1; i < 200; i += 2)
    for (size_t k = 0; k <= (size_t)(i * 37 % 3000); k++)
      if (p[i][k] != (char)i)
        return 2;
  for (int i = 1; i < 200; i += 2)
    free(p[i]);

  /* Big enough to need the heap grown several times. */
  char *big = malloc(1 << 20);
  if (big == NULL)
    return 3;
  memset(big, 1, 1 << 20);
  big = realloc(big, 2 << 20);
  if (big == NULL || big[(1 << 20) - 1] != 1)
    return 4;
  free(big);

  int *z = calloc(64, sizeof *z);
  for (int i = 0; i < 64; i++)
    if (z == NULL || z[i] != 0)
      return 5;
  free(z);
  errno = 0;
  if (malloc((size_t)1 << 40) != NULL || errno != ENOMEM) /* > HEAP_MAX */
    return 6;
  return 0;
}

/* brk itself: the heap's end moves, shrinking gives pages back, and the
 * kernel refuses to move it below its start or past its limit. */
static int breaks(void) {
  char *start = sbrk(0);
  if (start == (char *)-1 || (unsigned long)start % 4096 != 0)
    return 1;
  if (sbrk(8192) != start || sbrk(0) != start + 8192)
    return 2;
  start[8191] = 1; /* mapped now */
  if (sbrk(-8192) != start + 8192 || sbrk(0) != start)
    return 3;
  if (!FAILS(brk((char *)0x1000), ENOMEM)) /* below the heap */
    return 4;
  if (sbrk(0) != start)
    return 5;

  /* After shrinking, the page is gone: touching it kills the toucher. */
  sbrk(4096);
  sbrk(-4096);
  pid_t pid = fork();
  if (pid == 0) {
    *(volatile char *)start = 1;
    _exit(0);
  }
  int st;
  if (wait(&st) != pid || !WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV)
    return 6;
  return 0;
}

int main(void) {
  int r;
  if ((r = strings()) != 0)
    return 10 + r;
  if ((r = format()) != 0)
    return 20 + r;
  if ((r = breaks()) != 0) /* first: malloc hasn't grown the heap yet */
    return 30 + r;
  if ((r = heap()) != 0)
    return 40 + r;
  printf("libc ok: pid %d\n", getpid());
  return 0;
}
