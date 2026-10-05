/* Tests for calendar shading levels and minute rounding (src/core/shade.c). */
#include <glib.h>
#include "shade.h"

static void
test_levels_by_minutes (void)
{
  /* Spec: none, 1 to 10, 11 to 20, 21 and above. */
  g_assert_cmpint (jr_shade_level (0, FALSE), ==, 0);
  g_assert_cmpint (jr_shade_level (1, TRUE), ==, 1);
  g_assert_cmpint (jr_shade_level (10, TRUE), ==, 1);
  g_assert_cmpint (jr_shade_level (11, TRUE), ==, 2);
  g_assert_cmpint (jr_shade_level (20, TRUE), ==, 2);
  g_assert_cmpint (jr_shade_level (21, TRUE), ==, 3);
  g_assert_cmpint (jr_shade_level (600, TRUE), ==, 3);
}

static void
test_written_day_is_never_blank (void)
{
  /* A day with entries but under a minute of typing still shows as written. */
  g_assert_cmpint (jr_shade_level (0, TRUE), ==, 1);
  /* Minutes without an entry (text deleted later) still shade by minutes. */
  g_assert_cmpint (jr_shade_level (12, FALSE), ==, 2);
  g_assert_cmpint (jr_shade_level (-5, FALSE), ==, 0);
}

static void
test_minutes_from_seconds (void)
{
  g_assert_cmpint (jr_minutes_from_seconds (0), ==, 0);
  g_assert_cmpint (jr_minutes_from_seconds (1), ==, 1); /* any writing shows */
  g_assert_cmpint (jr_minutes_from_seconds (89), ==, 1);
  g_assert_cmpint (jr_minutes_from_seconds (90), ==, 2);
  g_assert_cmpint (jr_minutes_from_seconds (18 * 60), ==, 18);
  g_assert_cmpint (jr_minutes_from_seconds (-3), ==, 0);
}

static void
test_duration_text (void)
{
  char buf[32];
  jr_format_minutes (0, buf, sizeof buf);
  g_assert_cmpstr (buf, ==, "0 min");
  jr_format_minutes (17, buf, sizeof buf);
  g_assert_cmpstr (buf, ==, "17 min");
  jr_format_minutes (87, buf, sizeof buf);
  g_assert_cmpstr (buf, ==, "1 h 27 min");
  jr_format_minutes (120, buf, sizeof buf);
  g_assert_cmpstr (buf, ==, "2 h");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/shade/levels", test_levels_by_minutes);
  g_test_add_func ("/shade/written", test_written_day_is_never_blank);
  g_test_add_func ("/shade/minutes", test_minutes_from_seconds);
  g_test_add_func ("/shade/duration", test_duration_text);
  return g_test_run ();
}
