/* Tests for the active writing-time counter (src/core/active_time.c). */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include "active_time.h"

#define T0 1791316800 /* 2026-10-06T20:00:00Z */

/* Records every flush so tests can assert on them. */
typedef struct {
  int calls;
  char day[JR_ISO_LEN];
  gint64 seconds;
  gint64 total;
} Flushes;

static void
on_flush (const char *day, gint64 seconds, gpointer user)
{
  Flushes *f = user;
  f->calls++;
  g_strlcpy (f->day, day, sizeof f->day);
  f->seconds = seconds;
  f->total += seconds;
}

/* Calls tick once per second for [from, to). */
static void
run (JrActiveTimer *t, gint64 from, gint64 to)
{
  for (gint64 now = from; now < to; now++)
    jr_active_timer_tick (t, now);
}

static void
test_no_keystroke_counts_nothing (void)
{
  Flushes f = { 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, on_flush, &f);
  run (&t, T0, T0 + 100);
  jr_active_timer_flush (&t);
  g_assert_cmpint (f.total, ==, 0);
  g_assert_cmpint (f.calls, ==, 0);
  g_assert_cmpint (jr_active_timer_session_seconds (&t), ==, 0);
}

static void
test_pauses_after_60s_idle (void)
{
  Flushes f = { 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, on_flush, &f);
  jr_active_timer_keystroke (&t, T0);
  run (&t, T0, T0 + 300);
  jr_active_timer_flush (&t);
  /* Exactly 60 seconds count after one keystroke, then it pauses. */
  g_assert_cmpint (f.total, ==, 60);
  g_assert_cmpint (jr_active_timer_session_seconds (&t), ==, 60);
}

static void
test_keystrokes_keep_it_running (void)
{
  Flushes f = { 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, on_flush, &f);
  for (gint64 now = T0; now < T0 + 120; now++)
    {
      if ((now - T0) % 30 == 0)
        jr_active_timer_keystroke (&t, now);
      jr_active_timer_tick (&t, now);
    }
  jr_active_timer_flush (&t);
  g_assert_cmpint (f.total, ==, 120);
}

static void
test_flushes_every_15_seconds (void)
{
  Flushes f = { 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, on_flush, &f);
  jr_active_timer_keystroke (&t, T0);
  run (&t, T0, T0 + 14);
  g_assert_cmpint (f.calls, ==, 0);
  g_assert_cmpint (jr_active_timer_pending_seconds (&t), ==, 14);
  jr_active_timer_tick (&t, T0 + 14);
  g_assert_cmpint (f.calls, ==, 1);
  g_assert_cmpint (f.seconds, ==, 15);
  g_assert_cmpstr (f.day, ==, "2026-10-06");
  g_assert_cmpint (jr_active_timer_pending_seconds (&t), ==, 0);
  run (&t, T0 + 15, T0 + 30);
  g_assert_cmpint (f.calls, ==, 2);
  g_assert_cmpint (f.total, ==, 30);
}

static void
test_manual_flush_and_empty_flush (void)
{
  Flushes f = { 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, on_flush, &f);
  jr_active_timer_keystroke (&t, T0);
  run (&t, T0, T0 + 5);
  jr_active_timer_flush (&t); /* e.g. on lock or quit */
  g_assert_cmpint (f.calls, ==, 1);
  g_assert_cmpint (f.seconds, ==, 5);
  jr_active_timer_flush (&t); /* nothing pending: no call */
  g_assert_cmpint (f.calls, ==, 1);
}

static void
test_splits_time_at_midnight (void)
{
  /* Writing across midnight: seconds before go to the old day. */
  Flushes f = { 0 };
  JrActiveTimer t;
  gint64 midnight = T0 + 4 * 3600; /* 2026-10-07T00:00:00Z */
  jr_active_timer_init (&t, on_flush, &f);
  jr_active_timer_keystroke (&t, midnight - 5);
  run (&t, midnight - 5, midnight + 5);
  g_assert_cmpint (f.calls, ==, 1);
  g_assert_cmpstr (f.day, ==, "2026-10-06");
  g_assert_cmpint (f.seconds, ==, 5);
  jr_active_timer_flush (&t);
  g_assert_cmpstr (f.day, ==, "2026-10-07");
  g_assert_cmpint (f.seconds, ==, 5);
}

static void
test_stop_forgets_activity (void)
{
  /* Locking stops counting: no time on a locked screen. */
  Flushes f = { 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, on_flush, &f);
  jr_active_timer_keystroke (&t, T0);
  run (&t, T0, T0 + 10);
  jr_active_timer_stop (&t);
  g_assert_cmpint (f.total, ==, 10); /* stop flushes */
  run (&t, T0 + 10, T0 + 40);
  jr_active_timer_flush (&t);
  g_assert_cmpint (f.total, ==, 10);
  g_assert_cmpint (jr_active_timer_session_seconds (&t), ==, 0);
}

static void
test_unflushed_seconds_for_day (void)
{
  Flushes f = { 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, on_flush, &f);
  jr_active_timer_keystroke (&t, T0);
  run (&t, T0, T0 + 7);
  g_assert_cmpint (jr_active_timer_pending_for (&t, "2026-10-06"), ==, 7);
  g_assert_cmpint (jr_active_timer_pending_for (&t, "2026-10-05"), ==, 0);
}

int
main (int argc, char **argv)
{
  setenv ("TZ", "UTC0", 1);
  tzset ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/active-time/idle-start", test_no_keystroke_counts_nothing);
  g_test_add_func ("/active-time/idle-pause", test_pauses_after_60s_idle);
  g_test_add_func ("/active-time/keeps-running", test_keystrokes_keep_it_running);
  g_test_add_func ("/active-time/flush-15s", test_flushes_every_15_seconds);
  g_test_add_func ("/active-time/manual-flush", test_manual_flush_and_empty_flush);
  g_test_add_func ("/active-time/midnight", test_splits_time_at_midnight);
  g_test_add_func ("/active-time/stop", test_stop_forgets_activity);
  g_test_add_func ("/active-time/pending-for", test_unflushed_seconds_for_day);
  return g_test_run ();
}
