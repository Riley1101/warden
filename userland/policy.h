#pragma once

#include <abi/cap.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * agentd's policy: the profiles in /etc/profiles, and the deny-list in
 * /etc/denylist that configurations B and D check each command against.
 *
 * The deny-list is the baseline the paper measures the kernel against: a
 * userspace wrapper that reads the command line, the way such wrappers
 * do, and decides before anything runs. It splits the line as a shell
 * would (quotes removed, operators apart) but runs nothing and expands
 * nothing, so what it sees is the text, not what the text does.
 *
 * Pure, so it is host-tested (tests/host/include/test_policy.c).
 */

#define POLICY_NAME  32
#define POLICY_SCOPE 128
#define POLICY_WORD  256 /* longest word the check takes apart */
#define DENY_RULES   32

struct profile {
  char name[POLICY_NAME];
  uint32_t caps;
  char scope[POLICY_SCOPE];
  uint32_t max_procs;
};

struct denylist {
  char word[DENY_RULES][64]; /* no word of a line may contain one */
  int nword;
  char writes[DENY_RULES][16]; /* commands that change what they're given */
  int nwrites;
};

/* The next blank-separated word of *s, terminated in place; NULL at the
 * end of the line or at a '#' comment. */
static char *policy_word(char **s) {
  char *p = *s;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '\0' || *p == '#')
    return NULL;
  char *w = p;
  while (*p != '\0' && *p != ' ' && *p != '\t')
    p++;
  if (*p != '\0')
    *p++ = '\0';
  *s = p;
  return w;
}

/* Caps as ls -l shows them, "rwsk" with '-' for each missing: -1 if not. */
static int64_t policy_caps(const char *s) {
  static const char letters[] = "rwsk";
  static const uint32_t bits[] = {CAP_FS_READ, CAP_FS_WRITE, CAP_SPAWN,
                                  CAP_KILL};
  if (strlen(s) != 4)
    return -1;
  uint32_t caps = 0;
  for (int i = 0; i < 4; i++) {
    if (s[i] == letters[i])
      caps |= bits[i];
    else if (s[i] != '-')
      return -1;
  }
  return caps;
}

/* A positive decimal number below 100000, or 0 if `s` isn't one. */
static uint32_t policy_number(const char *s) {
  uint32_t n = 0;
  for (; *s != '\0'; s++) {
    if (*s < '0' || *s > '9' || n >= 10000)
      return 0;
    n = n * 10 + (uint32_t)(*s - '0');
  }
  return n;
}

/* `needle` occurs in `s` (libc has no strstr). */
static int policy_contains(const char *s, const char *needle) {
  size_t n = strlen(needle);
  for (; *s != '\0'; s++)
    if (strncmp(s, needle, n) == 0)
      return 1;
  return 0;
}

/* Copy `src` into `dst[n]`: 0, or -1 if it doesn't fit. */
static int policy_copy(char *dst, const char *src, size_t n) {
  if (strlen(src) >= n)
    return -1;
  memcpy(dst, src, strlen(src) + 1);
  return 0;
}

/*
 * The profile called `name` from the text of /etc/profiles, one per line:
 *   name caps scope max-procs
 * 1 found, 0 not there, or -(line number) of the first bad line.
 */
__attribute__((unused)) static int profile_find(char *text, const char *name,
                                                struct profile *out) {
  int line = 0;
  for (char *next = text; *next != '\0';) {
    char *s = next;
    while (*next != '\0' && *next != '\n')
      next++;
    if (*next == '\n')
      *next++ = '\0';
    line++;

    char *w[5];
    int n = 0;
    while (n < 5 && (w[n] = policy_word(&s)) != NULL)
      n++;
    if (n == 0)
      continue; /* blank, or a comment */
    if (n != 4)
      return -line;
    int64_t caps = policy_caps(w[1]);
    uint32_t max = policy_number(w[3]);
    if (caps < 0 || w[2][0] != '/' || max == 0 ||
        strlen(w[0]) >= POLICY_NAME || strlen(w[2]) >= POLICY_SCOPE)
      return -line;
    if (strcmp(w[0], name) == 0) {
      policy_copy(out->name, w[0], sizeof out->name);
      policy_copy(out->scope, w[2], sizeof out->scope);
      out->caps = (uint32_t)caps;
      out->max_procs = max;
      return 1;
    }
  }
  return 0;
}

/*
 * The text of /etc/denylist into *d, one rule per line:
 *   word TEXT        no word of a command line may contain TEXT
 *   writes CMD ...   CMD changes the files it is given
 * 0, or the number of the first bad line.
 */
__attribute__((unused)) static int deny_parse(char *text,
                                              struct denylist *d) {
  memset(d, 0, sizeof *d);
  int line = 0;
  for (char *next = text; *next != '\0';) {
    char *s = next;
    while (*next != '\0' && *next != '\n')
      next++;
    if (*next == '\n')
      *next++ = '\0';
    line++;

    char *kind = policy_word(&s);
    if (kind == NULL)
      continue;
    char *arg = policy_word(&s);
    if (arg == NULL)
      return line;
    if (strcmp(kind, "word") == 0) {
      if (policy_word(&s) != NULL || d->nword == DENY_RULES ||
          policy_copy(d->word[d->nword++], arg, sizeof d->word[0]) != 0)
        return line;
    } else if (strcmp(kind, "writes") == 0) {
      for (; arg != NULL; arg = policy_word(&s))
        if (d->nwrites == DENY_RULES ||
            policy_copy(d->writes[d->nwrites++], arg, sizeof d->writes[0]) != 0)
          return line;
    } else {
      return line;
    }
  }
  return 0;
}

/* `path` is `scope` or below it, by whole components. */
static int policy_in_scope(const char *scope, const char *path) {
  size_t n = strlen(scope);
  if (n == 1 && scope[0] == '/')
    return path[0] == '/';
  return strncmp(path, scope, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* The check, carried from one word of the line to the next. */
struct deny_state {
  const struct denylist *d;
  const struct profile *p;
  int at_command; /* the next word names a command */
  int writing;    /* the current command changes its arguments */
  int redirect;   /* '>' the next word is written, '<' read, or 0 */
};

/* A reason the word is refused, or NULL. */
static const char *deny_word(struct deny_state *st, const char *w) {
  for (int i = 0; i < st->d->nword; i++)
    if (policy_contains(w, st->d->word[i]))
      return "contains a denied word";

  int target = 0;
  if (st->redirect != 0) {
    target = st->redirect == '>';
    st->redirect = 0;
  } else if (st->at_command) {
    st->at_command = 0;
    const char *base = strrchr(w, '/') ? strrchr(w, '/') + 1 : w;
    st->writing = 0;
    for (int i = 0; i < st->d->nwrites; i++)
      st->writing |= strcmp(base, st->d->writes[i]) == 0;
  } else {
    target = st->writing && w[0] != '-';
  }
  if (!target)
    return NULL;
  if (!(st->p->caps & CAP_FS_WRITE))
    return "the profile cannot write";
  if (w[0] != '/')
    return "a relative path is written";
  if (!policy_in_scope(st->p->scope, w))
    return "writes outside the scope";
  return NULL;
}

/*
 * Whether `line` may run under the profile: NULL, or why not, with the
 * word it was about copied into `word[POLICY_WORD]`.
 */
__attribute__((unused)) static const char *
deny_check(const struct denylist *d, const struct profile *p, const char *line,
           char *word) {
  struct deny_state st = {d, p, 1, 0, 0};
  size_t len = 0;
  int in_word = 0;
  char quote = 0;
  for (const char *c = line;; c++) {
    if (quote != 0 && *c != '\0') {
      if (*c == quote)
        quote = 0;
      else if (len + 1 < POLICY_WORD)
        word[len++] = *c;
      else
        return "a word is too long to check";
      continue;
    }
    if (*c == '\'' || *c == '"') {
      quote = *c;
      in_word = 1;
      continue;
    }
    int ends = *c == '\0' || *c == ' ' || *c == '\t' || *c == ';' ||
               *c == '|' || *c == '&' || *c == '<' || *c == '>' ||
               (*c == '#' && !in_word);
    if (!ends) {
      if (len + 1 >= POLICY_WORD)
        return "a word is too long to check";
      word[len++] = *c;
      in_word = 1;
      continue;
    }
    if (in_word) {
      word[len] = '\0';
      const char *why = deny_word(&st, word);
      if (why != NULL)
        return why;
      len = 0;
      in_word = 0;
    }
    if (quote != 0)
      return "a quote is not closed";
    if (*c == '\0' || *c == '#')
      return NULL;
    if (*c == ';' || *c == '|' || *c == '&') {
      st.at_command = 1;
      st.writing = 0;
    } else if (*c == '<' || *c == '>') {
      st.redirect = *c;
    }
  }
}
