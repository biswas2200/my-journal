/* Tests for day-boundary and date parsing logic (src/core/jdate.c). */
#include <stdlib.h>
#include <time.h>
#include <glib.h>
#include "jdate.h"

/* Unix time for a local wall-clock moment in the current TZ. */
static gint64
local_time (int y, int mo, int d, int h, int mi)
{
  struct tm tm = { 0 };
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_isdst = -1;
  return (gint64) mktime (&tm);
}

static void
assert_day (JrDay got, int y, int m, int d)
{
  g_assert_cmpint (got.year, ==, y);
  g_assert_cmpint (got.month, ==, m);
  g_assert_cmpint (got.day, ==, d);
}

static void
test_entry_after_midnight_belongs_to_new_day (void)
{
  /* Spec: an entry written at 00:30 belongs to the new day. */
  assert_day (jr_day_from_time (local_time (2026, 10, 6, 23, 59)), 2026, 10, 6);
  assert_day (jr_day_from_time (local_time (2026, 10, 7, 0, 0)), 2026, 10, 7);
  assert_day (jr_day_from_time (local_time (2026, 10, 7, 0, 30)), 2026, 10, 7);
}

static void
test_day_uses_local_timezone (void)
{
  /* 2026-10-06 20:00 UTC is already 01:30 on the 7th in IST (+05:30). */
  gint64 t = 1791316800; /* 2026-10-06T20:00:00Z */
  setenv ("TZ", "UTC0", 1);
  tzset ();
  assert_day (jr_day_from_time (t), 2026, 10, 6);
  setenv ("TZ", "IST-5:30", 1);
  tzset ();
  assert_day (jr_day_from_time (t), 2026, 10, 7);
}

static void
test_year_end_boundary (void)
{
  assert_day (jr_day_from_time (local_time (2026, 12, 31, 23, 59)), 2026, 12, 31);
  assert_day (jr_day_from_time (local_time (2027, 1, 1, 0, 1)), 2027, 1, 1);
}

static void
test_iso_round_trip (void)
{
  char buf[JR_ISO_LEN];
  JrDay d;
  jr_day_to_iso ((JrDay){ 2026, 3, 4 }, buf);
  g_assert_cmpstr (buf, ==, "2026-03-04");
  g_assert_true (jr_day_from_iso ("2026-10-04", &d));
  assert_day (d, 2026, 10, 4);
  g_assert_false (jr_day_from_iso ("2026-02-30", &d));
  g_assert_false (jr_day_from_iso ("2026-1-04", &d));
  g_assert_false (jr_day_from_iso ("2026-10-04x", &d));
  g_assert_false (jr_day_from_iso ("", &d));
  g_assert_false (jr_day_from_iso (NULL, &d));
}

static void
test_arithmetic (void)
{
  JrDay d = { 2026, 10, 1 };
  assert_day (jr_day_add (d, -1), 2026, 9, 30);
  assert_day (jr_day_add ((JrDay){ 2028, 2, 28 }, 1), 2028, 2, 29);
  assert_day (jr_day_add ((JrDay){ 2026, 12, 31 }, 1), 2027, 1, 1);
  g_assert_cmpint (jr_day_diff ((JrDay){ 2026, 9, 30 }, (JrDay){ 2026, 10, 6 }), ==, 6);
  g_assert_cmpint (jr_day_compare ((JrDay){ 2026, 10, 6 }, (JrDay){ 2026, 10, 7 }), <, 0);
  g_assert_cmpint (jr_day_compare ((JrDay){ 2026, 10, 6 }, (JrDay){ 2026, 10, 6 }), ==, 0);
  /* Tue 6 Oct 2026: Monday = 1 ... Sunday = 7. */
  g_assert_cmpint (jr_day_weekday ((JrDay){ 2026, 10, 6 }), ==, 2);
  g_assert_cmpint (jr_day_weekday ((JrDay){ 2026, 10, 4 }), ==, 7);
  g_assert_cmpint (jr_days_in_month (2026, 2), ==, 28);
  g_assert_cmpint (jr_days_in_month (2028, 2), ==, 29);
}

static void
test_parse_user_input (void)
{
  JrDay today = { 2026, 10, 6 };
  JrDay d;
  g_assert_true (jr_day_parse_input ("4 Oct", today, &d));
  assert_day (d, 2026, 10, 4);
  g_assert_true (jr_day_parse_input ("  2026-10-04 ", today, &d));
  assert_day (d, 2026, 10, 4);
  g_assert_true (jr_day_parse_input ("Oct 4", today, &d));
  assert_day (d, 2026, 10, 4);
  g_assert_true (jr_day_parse_input ("4 october 2025", today, &d));
  assert_day (d, 2025, 10, 4);
  g_assert_true (jr_day_parse_input ("Sep 30, 2026", today, &d));
  assert_day (d, 2026, 9, 30);
  /* No year and the date is later this year: it means last year. */
  g_assert_true (jr_day_parse_input ("25 Dec", today, &d));
  assert_day (d, 2025, 12, 25);
  g_assert_true (jr_day_parse_input ("today", today, &d));
  assert_day (d, 2026, 10, 6);
  g_assert_true (jr_day_parse_input ("Yesterday", today, &d));
  assert_day (d, 2026, 10, 5);
}

static void
test_parse_rejects_bad_input (void)
{
  JrDay today = { 2026, 10, 6 };
  JrDay d;
  g_assert_false (jr_day_parse_input ("", today, &d));
  g_assert_false (jr_day_parse_input ("hello", today, &d));
  g_assert_false (jr_day_parse_input ("31 Feb", today, &d));
  g_assert_false (jr_day_parse_input ("4", today, &d));
  g_assert_false (jr_day_parse_input ("4 Oc", today, &d));
  /* The future cannot be opened. */
  g_assert_false (jr_day_parse_input ("2026-10-07", today, &d));
  g_assert_false (jr_day_parse_input ("4 Oct 2027", today, &d));
}

static void
test_formatting (void)
{
  char buf[32];
  jr_day_format_long ((JrDay){ 2026, 10, 6 }, buf, sizeof buf);
  g_assert_cmpstr (buf, ==, "Tue 6 Oct 2026");
  jr_day_format_short ((JrDay){ 2026, 10, 6 }, buf, sizeof buf);
  g_assert_cmpstr (buf, ==, "Tue 6 Oct");
  g_assert_cmpstr (jr_weekday_name ((JrDay){ 2026, 10, 6 }), ==, "Tuesday");
  g_assert_cmpstr (jr_month_name (10), ==, "October");

  setenv ("TZ", "UTC0", 1);
  tzset ();
  jr_format_clock (1791316800 + 2 * 3600 + 42 * 60, buf, sizeof buf); /* 22:42 */
  g_assert_cmpstr (buf, ==, "10:42 PM");
  jr_format_clock (1791316800 - 20 * 3600 + 14 * 60, buf, sizeof buf); /* 00:14 */
  g_assert_cmpstr (buf, ==, "12:14 AM");
  jr_format_clock (1791316800 - 8 * 3600 + 5 * 60, buf, sizeof buf); /* 12:05 */
  g_assert_cmpstr (buf, ==, "12:05 PM");
}

int
main (int argc, char **argv)
{
  setenv ("TZ", "UTC0", 1);
  tzset ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/jdate/midnight", test_entry_after_midnight_belongs_to_new_day);
  g_test_add_func ("/jdate/timezone", test_day_uses_local_timezone);
  g_test_add_func ("/jdate/year-end", test_year_end_boundary);
  g_test_add_func ("/jdate/iso", test_iso_round_trip);
  g_test_add_func ("/jdate/arithmetic", test_arithmetic);
  g_test_add_func ("/jdate/parse", test_parse_user_input);
  g_test_add_func ("/jdate/parse-bad", test_parse_rejects_bad_input);
  g_test_add_func ("/jdate/format", test_formatting);
  return g_test_run ();
}
