#include "cap.h"
#include "check.h"

#include <abi/errno.h>

/* An agent label for session 1 holding `caps`, writes under `scope`. */
static struct label agent(uint32_t caps, const char *scope) {
  struct label l = {0};
  CHECK(label_narrow(&l, 1, caps, scope) == 0);
  return l;
}

static void test_in_scope(void) {
  CHECK(in_scope("/home/work", "/home/work"));
  CHECK(in_scope("/home/work", "/home/work/a/b"));
  CHECK(!in_scope("/home/work", "/home/workevil"));
  CHECK(!in_scope("/home/work", "/home/wor"));
  CHECK(!in_scope("/home/work", "/home"));
  CHECK(!in_scope("/home/work", "/"));
  CHECK(in_scope("/", "/"));
  CHECK(in_scope("/", "/bin/ls"));
}

static void test_human(void) {
  struct label human = {0};
  CHECK(cap_allows(&human, CAP_SPAWN) == 0);
  CHECK(cap_path(&human, "/bin/ls", CAP_FS_WRITE) == 0);
  CHECK(cap_path(&human, "relative", CAP_FS_READ) == 0); /* the fs's to judge */
}

static void test_agent_caps(void) {
  struct label l = agent(CAP_FS_READ, "/home/work");
  CHECK(cap_allows(&l, CAP_ANY) == 0);
  CHECK(cap_allows(&l, CAP_FS_READ) == 0);
  CHECK(cap_allows(&l, CAP_SPAWN) == -ECAPDENIED);
  CHECK(cap_allows(&l, CAP_FS_READ | CAP_SPAWN) == -ECAPDENIED);
  /* reading is not scoped, but writing needs the cap even in scope */
  CHECK(cap_path(&l, "/bin/ls", CAP_FS_READ) == 0);
  CHECK(cap_path(&l, "/home/work/a", CAP_FS_WRITE) == -ECAPDENIED);
}

static void test_agent_scope(void) {
  struct label l = agent(CAP_FS_READ | CAP_FS_WRITE, "/home/work");
  CHECK(cap_path(&l, "/home/work", CAP_FS_WRITE) == 0);
  CHECK(cap_path(&l, "/home/work/a/b", CAP_FS_WRITE) == 0);
  CHECK(cap_path(&l, "/home/work//a/./b/", CAP_FS_WRITE) == 0);
  CHECK(cap_path(&l, "/home/workevil", CAP_FS_WRITE) == -ECAPDENIED);
  CHECK(cap_path(&l, "/home/work/../../bin/ls", CAP_FS_WRITE) == -ECAPDENIED);
  CHECK(cap_path(&l, "/home/work/../work2", CAP_FS_WRITE) == -ECAPDENIED);
  CHECK(cap_path(&l, "/bin/ls", CAP_FS_WRITE) == -ECAPDENIED);
  CHECK(cap_path(&l, "home/work/a", CAP_FS_WRITE) == -ENOENT);
}

static void test_agent_channel(void) {
  struct label l = agent(CAP_ALL, "/");
  CHECK(cap_path(&l, "/dev/agent", CAP_FS_READ) == -ECAPDENIED);
  CHECK(cap_path(&l, "/dev//./agent", CAP_FS_READ) == -ECAPDENIED);
  CHECK(cap_path(&l, "/dev/agentx", CAP_FS_READ) == 0);
  struct label human = {0};
  CHECK(cap_path(&human, "/dev/agent", CAP_FS_READ | CAP_FS_WRITE) == 0);
}

static void test_narrow(void) {
  struct label l = agent(CAP_FS_READ | CAP_FS_WRITE, "/home/work/");
  CHECK_STR(l.scope, "/home/work"); /* stored normalized */
  CHECK(l.origin == ORIGIN_AGENT);

  /* asking for more caps gets only what it already had */
  CHECK(label_narrow(&l, 1, CAP_ALL, "/home/work/sub") == 0);
  CHECK(l.caps == (CAP_FS_READ | CAP_FS_WRITE));
  CHECK_STR(l.scope, "/home/work/sub");

  /* the scope can't widen, nor escape by prefix or by .. */
  CHECK(label_narrow(&l, 1, CAP_FS_READ, "/home/work") == -EPERM);
  CHECK(label_narrow(&l, 1, CAP_FS_READ, "/home/work/subevil") == -EPERM);
  CHECK(label_narrow(&l, 1, CAP_FS_READ, "/home/work/sub/../..") == -EPERM);
  /* nor the session change */
  CHECK(label_narrow(&l, 2, CAP_FS_READ, "/home/work/sub") == -EPERM);
  CHECK(label_narrow(&l, 1, CAP_FS_READ, "nope") == -EINVAL);
  /* and a failure leaves it as it was */
  CHECK(l.caps == (CAP_FS_READ | CAP_FS_WRITE));
  CHECK_STR(l.scope, "/home/work/sub");

  /* unknown bits are dropped */
  struct label m = agent(0xffu, "/");
  CHECK(m.caps == CAP_ALL);
}

int main(void) {
  test_in_scope();
  test_human();
  test_agent_caps();
  test_agent_scope();
  test_agent_channel();
  test_narrow();
  return CHECK_DONE();
}
