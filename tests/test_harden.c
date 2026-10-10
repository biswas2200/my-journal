/* Tests for process hardening (src/core/harden.c). */
#include <sys/prctl.h>
#include <sys/resource.h>
#include <glib.h>
#include "harden.h"

static void
test_no_core_dumps_and_no_memory_peeking (void)
{
  g_assert_true (jr_harden_process ());
  /* Not dumpable: no core files, and other processes of the same user
   * cannot attach a debugger or read /proc/<pid>/mem. */
  g_assert_cmpint (prctl (PR_GET_DUMPABLE, 0, 0, 0, 0), ==, 0);
  struct rlimit rl;
  g_assert_cmpint (getrlimit (RLIMIT_CORE, &rl), ==, 0);
  g_assert_cmpuint (rl.rlim_cur, ==, 0);
  g_assert_cmpuint (rl.rlim_max, ==, 0);
  /* Calling it twice is fine. */
  g_assert_true (jr_harden_process ());
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/harden/process", test_no_core_dumps_and_no_memory_peeking);
  return g_test_run ();
}
