/* Tests for the wrong-PIN lockout timer (src/core/lockout.c). */
#include <glib.h>
#include "lockout.h"

#define T0 1791316800

static void
test_four_wrong_tries_do_not_lock (void)
{
  JrLockout l = { 0 };
  for (int i = 0; i < JR_LOCKOUT_MAX_TRIES - 1; i++)
    jr_lockout_fail (&l, T0 + i);
  g_assert_false (jr_lockout_active (&l, T0 + 4));
  g_assert_cmpint (jr_lockout_tries_left (&l), ==, 1);
}

static void
test_fifth_wrong_try_waits_30s (void)
{
  JrLockout l = { 0 };
  for (int i = 0; i < JR_LOCKOUT_MAX_TRIES; i++)
    jr_lockout_fail (&l, T0);
  g_assert_true (jr_lockout_active (&l, T0));
  g_assert_cmpint (jr_lockout_remaining (&l, T0), ==, 30);
  g_assert_cmpint (jr_lockout_remaining (&l, T0 + 29), ==, 1);
  g_assert_true (jr_lockout_active (&l, T0 + 29));
  g_assert_false (jr_lockout_active (&l, T0 + 30));
  g_assert_cmpint (jr_lockout_remaining (&l, T0 + 31), ==, 0);
  /* After waiting, a fresh set of five tries is allowed. */
  g_assert_cmpint (jr_lockout_tries_left (&l), ==, JR_LOCKOUT_MAX_TRIES);
}

static void
test_tries_during_lockout_are_ignored (void)
{
  JrLockout l = { 0 };
  for (int i = 0; i < JR_LOCKOUT_MAX_TRIES; i++)
    jr_lockout_fail (&l, T0);
  /* Typing more PINs while waiting must not extend or reset the wait. */
  jr_lockout_fail (&l, T0 + 10);
  g_assert_cmpint (jr_lockout_remaining (&l, T0 + 10), ==, 20);
}

static void
test_success_resets (void)
{
  JrLockout l = { 0 };
  jr_lockout_fail (&l, T0);
  jr_lockout_fail (&l, T0);
  jr_lockout_success (&l);
  g_assert_cmpint (jr_lockout_tries_left (&l), ==, JR_LOCKOUT_MAX_TRIES);
  g_assert_false (jr_lockout_active (&l, T0));
}

static void
test_clock_moved_back_does_not_extend (void)
{
  /* If the clock jumps back an hour, the wait is still at most 30 s. */
  JrLockout l = { 0 };
  for (int i = 0; i < JR_LOCKOUT_MAX_TRIES; i++)
    jr_lockout_fail (&l, T0);
  g_assert_cmpint (jr_lockout_remaining (&l, T0 - 3600), ==, 30);
}

static void
test_serialize_round_trip (void)
{
  /* Persisted so a restart during lockout does not reset it. */
  JrLockout l = { 0 }, back = { 0 };
  char buf[JR_LOCKOUT_STR_LEN];
  for (int i = 0; i < JR_LOCKOUT_MAX_TRIES; i++)
    jr_lockout_fail (&l, T0);
  jr_lockout_to_string (&l, buf, sizeof buf);
  g_assert_true (jr_lockout_from_string (buf, &back));
  g_assert_cmpint (jr_lockout_remaining (&back, T0 + 5), ==, 25);

  g_assert_false (jr_lockout_from_string ("garbage", &back));
  g_assert_false (jr_lockout_from_string (NULL, &back));
  g_assert_false (jr_lockout_from_string ("99:1", &back));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/lockout/four", test_four_wrong_tries_do_not_lock);
  g_test_add_func ("/lockout/fifth", test_fifth_wrong_try_waits_30s);
  g_test_add_func ("/lockout/ignored", test_tries_during_lockout_are_ignored);
  g_test_add_func ("/lockout/success", test_success_resets);
  g_test_add_func ("/lockout/clock-back", test_clock_moved_back_does_not_extend);
  g_test_add_func ("/lockout/serialize", test_serialize_round_trip);
  return g_test_run ();
}
