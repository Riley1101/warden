#include "check.h"
#include "policy.h"

static const char PROFILES[] = "# comment\n"
                               "\n"
                               "readonly  r-s-  /home/work  16\n"
                               "work-rw   rwsk  /home/work  32  # trailing\n";

static void test_profiles(void) {
  char text[sizeof PROFILES];
  struct profile p;

  memcpy(text, PROFILES, sizeof text);
  CHECK(profile_find(text, "work-rw", &p) == 1);
  CHECK_STR(p.name, "work-rw");
  CHECK_STR(p.scope, "/home/work");
  CHECK(p.caps == CAP_ALL && p.max_procs == 32);

  memcpy(text, PROFILES, sizeof text);
  CHECK(profile_find(text, "readonly", &p) == 1);
  CHECK(p.caps == (CAP_FS_READ | CAP_SPAWN) && p.max_procs == 16);

  memcpy(text, PROFILES, sizeof text);
  CHECK(profile_find(text, "root", &p) == 0);

  /* every bad line is found, even before the profile asked for */
  static const char *const bad[] = {
      "x rwsk /home/work\nok rwsk /w 1\n",    /* too few fields */
      "x rwsk /home/work 1 2\nok rwsk /w 1\n", /* too many */
      "x rwxk /home/work 1\nok rwsk /w 1\n",  /* not rwsk */
      "x wr-- /home/work 1\nok rwsk /w 1\n",  /* letters out of place */
      "x rwsk home/work 1\nok rwsk /w 1\n",   /* relative scope */
      "x rwsk /home/work 0\nok rwsk /w 1\n",  /* no processes */
      "x rwsk /home/work 9x\nok rwsk /w 1\n", /* not a number */
      "x rwsk /home/work 99999999999\nok rwsk /w 1\n",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char b[128];
    strcpy(b, bad[i]);
    CHECK(profile_find(b, "ok", &p) == -1);
  }
  char second[] = "ok rwsk /w 1\nbad\n";
  CHECK(profile_find(second, "nope", &p) == -2);
}

static const char DENY[] = "# the shipped rules\n"
                           "word ..\n"
                           "word /dev/agent\n"
                           "writes rm mv mkdir\n";

static struct denylist deny;
static const struct profile work = {"work-rw", CAP_ALL, "/home/work", 32};
static const struct profile ro = {"readonly", CAP_FS_READ | CAP_SPAWN,
                                  "/home/work", 16};

static int allowed(const struct profile *p, const char *line) {
  char word[POLICY_WORD];
  return deny_check(&deny, p, line, word) == NULL;
}

static void test_deny_parse(void) {
  char text[sizeof DENY];
  memcpy(text, DENY, sizeof text);
  CHECK(deny_parse(text, &deny) == 0);
  CHECK(deny.nword == 2 && deny.nwrites == 3);
  CHECK_STR(deny.writes[2], "mkdir");

  struct denylist d;
  char a[] = "word\n", b[] = "word a b\n", c[] = "\nforbid rm\n";
  CHECK(deny_parse(a, &d) == 1);
  CHECK(deny_parse(b, &d) == 1);
  CHECK(deny_parse(c, &d) == 2);
}

static void test_deny_check(void) {
  /* the work goes through */
  CHECK(allowed(&work, "ls -l /home/work"));
  CHECK(allowed(&work, "echo hi > /home/work/notes ; cat /home/work/notes"));
  CHECK(allowed(&work, "mkdir /home/work/a && mv /home/work/a /home/work/b"));
  CHECK(allowed(&work, "rm /home/work/b"));
  CHECK(allowed(&work, "cat /bin/ls | wc -c"));
  CHECK(allowed(&work, "echo '# not a comment' >> /home/work/f # this is"));
  CHECK(allowed(&work, "ls /home/work ; ps"));

  /* the rules catch what they're written for */
  CHECK(!allowed(&work, "rm /bin/ls"));
  CHECK(!allowed(&work, "rm /home/work/../../bin/ls"));
  CHECK(!allowed(&work, "cat /dev/agent"));
  CHECK(!allowed(&work, "echo x > /bin/ls"));
  CHECK(!allowed(&work, "echo x >> /etc/profiles"));
  CHECK(!allowed(&work, "echo x>/bin/ls")); /* no blank needed */
  CHECK(!allowed(&work, "mv /home/work/a /bin/a"));
  CHECK(!allowed(&work, "mv /bin/ls /home/work/ls"));
  CHECK(!allowed(&work, "rm /home/workevil/x")); /* by component */
  CHECK(!allowed(&work, "cd /bin ; rm ls"));     /* relative writes */
  CHECK(!allowed(&work, "ls ; /bin/rm /bin/ls")); /* by its full name */
  CHECK(!allowed(&work, "rm \"/bin\"'/ls'"));     /* quotes come off */
  CHECK(!allowed(&work, "ls|rm /bin/ls"));
  CHECK(!allowed(&work, "echo 'unclosed"));

  /* readonly: nothing may be written at all */
  CHECK(allowed(&ro, "cat /home/work/notes"));
  CHECK(!allowed(&ro, "echo x > /home/work/notes"));
  CHECK(!allowed(&ro, "rm /home/work/notes"));

  /* and what the text doesn't show, it can't see: nested shells,
   * scripts, programs it doesn't know write */
  CHECK(allowed(&work, "sh -c 'rm /bin/ls'"));
  CHECK(allowed(&work, "sh /home/work/script"));
  CHECK(allowed(&ro, "sh -c 'echo x > /home/work/notes'"));

  /* the word it was about comes back with the reason */
  char word[POLICY_WORD];
  CHECK_STR(deny_check(&deny, &work, "ls ; rm /bin/ls", word),
            "writes outside the scope");
  CHECK_STR(word, "/bin/ls");
}

int main(void) {
  test_profiles();
  test_deny_parse();
  test_deny_check();
  return CHECK_DONE();
}
