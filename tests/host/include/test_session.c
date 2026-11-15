#include "check.h"
#include "session.h"

#include <abi/errno.h>

static void test_limit(void) {
  CHECK(session_enter(1, 0) == -EINVAL);
  CHECK(session_nproc(1) == 0);

  CHECK(session_enter(1, 3) == 0); /* the marking process itself */
  CHECK(session_spawn(1) == 0);
  CHECK(session_spawn(1) == 0);
  CHECK(session_nproc(1) == 3);
  CHECK(session_spawn(1) == -EAGAIN);
  CHECK(session_nproc(1) == 3); /* a refusal counts nothing */

  session_leave(1);
  CHECK(session_spawn(1) == 0); /* room again */
  CHECK(session_spawn(1) == -EAGAIN);
}

static void test_never_raised(void) {
  /* session 1 holds 3, at limit 3 */
  session_lower(1, 10);
  CHECK(session_spawn(1) == -EAGAIN);
  CHECK(session_enter(1, 10) == -EAGAIN); /* a new marker can't raise it */
  session_lower(1, 0);                    /* 0 is not a limit */
  session_leave(1);
  CHECK(session_spawn(1) == 0);

  /* lowering below the count keeps who is there, and lets no one in */
  session_lower(1, 1);
  CHECK(session_nproc(1) == 3);
  CHECK(session_spawn(1) == -EAGAIN);
  session_leave(1);
  session_leave(1);
  CHECK(session_spawn(1) == -EAGAIN); /* 1 of 1 */
}

static void test_forgotten(void) {
  session_leave(1);
  CHECK(session_nproc(1) == 0);
  CHECK(session_spawn(1) == -EAGAIN); /* gone: nothing to fork into */
  session_leave(1);                   /* harmless */

  /* marking into it again starts afresh, with the new limit */
  CHECK(session_enter(1, 5) == 0);
  for (int i = 0; i < 4; i++)
    CHECK(session_spawn(1) == 0);
  CHECK(session_spawn(1) == -EAGAIN);
  for (int i = 0; i < 5; i++)
    session_leave(1);
  CHECK(session_nproc(1) == 0);
}

static void test_separate_and_full(void) {
  CHECK(session_enter(1, 1) == 0);
  CHECK(session_enter(2, 2) == 0);
  CHECK(session_spawn(1) == -EAGAIN);
  CHECK(session_spawn(2) == 0); /* each its own limit */

  for (uint32_t id = 3; id < 3 + SESSION_MAX - 2; id++)
    CHECK(session_enter(id, 1) == 0);
  CHECK(session_enter(99, 1) == -ENOMEM); /* every slot taken */
  session_leave(3);
  CHECK(session_enter(99, 1) == 0); /* a freed slot is reused */
}

int main(void) {
  test_limit();
  test_never_raised();
  test_forgotten();
  test_separate_and_full();
  return CHECK_DONE();
}
