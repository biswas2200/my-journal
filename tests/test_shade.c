/* Tests for calendar shading and minute text (src/core/shade.c), LeetCode
 * style (see cases.h): tables of edge cases, hidden tests against slow
 * references, and the biggest values the types allow. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "shade.h"

/* Constraints: minutes is any int (the database file could be damaged, so
 * negative and huge values must not misbehave). Spec: none, 1 to 10,
 * 11 to 20, 21 and above; a day with entries is never blank. */
static void
test_shade_level (void)
{
  static const struct {
    const char *name;
    int minutes;
    gboolean has_entries;
    int expected;
  } cases[] = {
    { "nothing written", 0, FALSE, 0 },
    { "one minute", 1, TRUE, 1 },
    { "ten minutes: top of step 1", 10, TRUE, 1 },
    { "eleven minutes: bottom of step 2", 11, TRUE, 2 },
    { "twenty minutes: top of step 2", 20, TRUE, 2 },
    { "twenty-one minutes: bottom of step 3", 21, TRUE, 3 },
    { "ten hours", 600, TRUE, 3 },
    { "entries but under a minute", 0, TRUE, 1 },
    { "minutes but text deleted later", 12, FALSE, 2 },
    { "one minute, no entry", 1, FALSE, 1 },
    { "negative minutes, no entry", -5, FALSE, 0 },
    { "negative minutes with entries", -5, TRUE, 1 },
    { "smallest int, no entry", G_MININT, FALSE, 0 },
    { "smallest int with entries", G_MININT, TRUE, 1 },
    { "largest int", G_MAXINT, FALSE, 3 },
  };
  JrCases c = { .table = "shade-level" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      int got = jr_shade_level (cases[i].minutes, cases[i].has_entries);
      jr_case (&c, cases[i].name, got == cases[i].expected,
               "Input minutes=%d entries=%s  Expected %d  Got %d", cases[i].minutes,
               cases[i].has_entries ? "yes" : "no", cases[i].expected, got);
    }
  jr_cases_done (&c);
}

/* Constraints: seconds is any gint64. Rounds to the nearest minute, half
 * a minute rounds up, any writing at all is at least 1, nothing (or a
 * negative count from a damaged file) is 0, and the answer never goes
 * past G_MAXINT. */
static void
test_minutes_from_seconds (void)
{
  static const struct {
    const char *name;
    gint64 seconds;
    int expected;
  } cases[] = {
    { "nothing", 0, 0 },
    { "one second shows as a minute", 1, 1 },
    { "29 seconds", 29, 1 },
    { "30 seconds", 30, 1 },
    { "one minute", 60, 1 },
    { "89 seconds rounds down", 89, 1 },
    { "90 seconds rounds up", 90, 2 },
    { "18 minutes", 18 * 60, 18 },
    { "18.5 minutes", 18 * 60 + 30, 19 },
    { "a whole day", 86400, 1440 },
    { "negative", -3, 0 },
    { "smallest gint64", G_MININT64, 0 },
    { "largest that fits", (gint64) G_MAXINT * 60, G_MAXINT },
    { "one minute past what fits", (gint64) G_MAXINT * 60 + 60, G_MAXINT },
    { "largest gint64", G_MAXINT64, G_MAXINT },
    { "largest gint64 less 29", G_MAXINT64 - 29, G_MAXINT },
  };
  JrCases c = { .table = "minutes-from-seconds" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      int got = jr_minutes_from_seconds (cases[i].seconds);
      jr_case (&c, cases[i].name, got == cases[i].expected,
               "Input %" G_GINT64_FORMAT "  Expected %d  Got %d", cases[i].seconds,
               cases[i].expected, got);
    }
  jr_cases_done (&c);
}

/* Constraints: minutes is any int; buf holds at least 1 byte. Text is
 * "N min" under an hour, "N h" on the hour, "N h M min" otherwise. A
 * negative count (never from the app) shows as "0 min". A short buffer
 * gets a cut-off but terminated text. */
static void
test_format_minutes (void)
{
  static const struct {
    const char *name;
    int minutes;
    gsize len;
    const char *expected;
  } cases[] = {
    { "zero", 0, 32, "0 min" },
    { "one minute", 1, 32, "1 min" },
    { "under an hour", 17, 32, "17 min" },
    { "59 minutes", 59, 32, "59 min" },
    { "exactly an hour", 60, 32, "1 h" },
    { "an hour and a minute", 61, 32, "1 h 1 min" },
    { "hours and minutes", 87, 32, "1 h 27 min" },
    { "two hours", 120, 32, "2 h" },
    { "a whole day", 1440, 32, "24 h" },
    { "largest int", G_MAXINT, 32, "35791394 h 7 min" },
    { "negative", -1, 32, "0 min" },
    { "negative hours", -61, 32, "0 min" },
    { "smallest int", G_MININT, 32, "0 min" },
    { "buffer just fits", 87, 11, "1 h 27 min" },
    { "buffer one short", 87, 10, "1 h 27 mi" },
    { "one-byte buffer", 87, 1, "" },
  };
  JrCases c = { .table = "format-minutes" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      char buf[40];
      memset (buf, 'X', sizeof buf);
      jr_format_minutes (cases[i].minutes, buf, cases[i].len);
      /* Nothing written past len: the byte after it is still 'X'. */
      gboolean ok = memchr (buf, '\0', cases[i].len) != NULL && buf[cases[i].len] == 'X' &&
                    strcmp (buf, cases[i].expected) == 0;
      jr_case (&c, cases[i].name, ok, "Input minutes=%d len=%" G_GSIZE_FORMAT "  Expected \"%s\"  Got \"%.*s\"",
               cases[i].minutes, cases[i].len, cases[i].expected, (int) cases[i].len, buf);
    }
  jr_cases_done (&c);
}

/* Reference for the shade: the spec's steps, one range per row. */
static int
slow_shade (int minutes, gboolean has_entries)
{
  static const struct {
    gint64 from, to;
    int level;
  } steps[] = { { 1, 10, 1 }, { 11, 20, 2 }, { 21, G_MAXINT, 3 } };
  for (gsize i = 0; i < G_N_ELEMENTS (steps); i++)
    if (minutes >= steps[i].from && minutes <= steps[i].to)
      return steps[i].level;
  return has_entries ? 1 : 0;
}

/* Hidden test: every minute count of two days, both ways, plus random
 * ints across the whole range. */
static void
test_hidden_shade (void)
{
  JrCases c = { .table = "hidden: shade-level" };
  for (int n = 0; n < 200000; n++)
    {
      int minutes = n < 6000 ? n / 2 - 100 : (int) g_test_rand_int ();
      gboolean has_entries = n % 2;
      int got = jr_shade_level (minutes, has_entries);
      int want = slow_shade (minutes, has_entries);
      jr_case (&c, "minutes", got == want, "Input minutes=%d entries=%d  Expected %d  Got %d", minutes,
               has_entries, want, got);
    }
  jr_cases_done (&c);
}

/* Reference for rounding: compares the remainder with half a minute,
 * without any addition that could overflow. */
static int
slow_minutes (gint64 seconds)
{
  if (seconds <= 0)
    return 0;
  guint64 whole = (guint64) seconds / 60, rest = (guint64) seconds % 60;
  guint64 m = whole + (rest >= 30 ? 1 : 0);
  if (m == 0)
    return 1;
  return m > G_MAXINT ? G_MAXINT : (int) m;
}

/* Hidden test: every second of the first two hours, random counts up to
 * a few years, and random gint64 values of every size. */
static void
test_hidden_minutes (void)
{
  JrCases c = { .table = "hidden: minutes-from-seconds" };
  for (int n = 0; n < 300000; n++)
    {
      gint64 seconds;
      if (n < 7200)
        seconds = n;
      else if (n % 3 == 0)
        seconds = g_test_rand_int_range (0, G_MAXINT32);
      else /* a random gint64 of a random bit length, either sign */
        {
          guint64 bits = ((guint64) g_test_rand_int () << 32) | g_test_rand_int ();
          bits >>= g_test_rand_int_range (0, 64);
          seconds = (gint64) bits;
        }
      int got = jr_minutes_from_seconds (seconds);
      int want = slow_minutes (seconds);
      jr_case (&c, "seconds", got == want, "Input %" G_GINT64_FORMAT "  Expected %d  Got %d", seconds,
               want, got);
    }
  jr_cases_done (&c);
}

/* Reads "N min", "N h" or "N h M min" back to minutes; -1 if the text
 * breaks a rule (leading zeros, "0 h", "0 min" after hours, M >= 60). */
static gint64
read_back (const char *text)
{
  guint64 h = 0, m = 0;
  char **words = g_strsplit (text, " ", -1);
  guint n = g_strv_length (words);
  gboolean ok = FALSE;
  if (n == 2 && strcmp (words[1], "min") == 0)
    ok = g_ascii_string_to_unsigned (words[0], 10, 0, 59, &m, NULL);
  else if (n == 2 && strcmp (words[1], "h") == 0)
    ok = g_ascii_string_to_unsigned (words[0], 10, 1, G_MAXINT, &h, NULL);
  else if (n == 4 && strcmp (words[1], "h") == 0 && strcmp (words[3], "min") == 0)
    ok = g_ascii_string_to_unsigned (words[0], 10, 1, G_MAXINT, &h, NULL) &&
         g_ascii_string_to_unsigned (words[2], 10, 1, 59, &m, NULL);
  /* No leading zeros or signs: the number reads back to the same text. */
  for (guint i = 0; ok && i < n; i += 2)
    ok = words[i][0] != '0' || words[i][1] == '\0';
  g_strfreev (words);
  return ok ? (gint64) (h * 60 + m) : -1;
}

/* Hidden test: random minute counts; the text reads back to the count. */
static void
test_hidden_format (void)
{
  JrCases c = { .table = "hidden: format-minutes" };
  for (int n = 0; n < 200000; n++)
    {
      int minutes = n < 3000 ? n : g_test_rand_int_range (0, G_MAXINT);
      char buf[32];
      jr_format_minutes (minutes, buf, sizeof buf);
      gint64 back = read_back (buf);
      jr_case (&c, "minutes", back == minutes, "Input %d  Got \"%s\" (reads back as %" G_GINT64_FORMAT ")",
               minutes, buf, back);
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/shade/level", test_shade_level);
  g_test_add_func ("/shade/minutes-from-seconds", test_minutes_from_seconds);
  g_test_add_func ("/shade/format-minutes", test_format_minutes);
  g_test_add_func ("/shade/hidden/level", test_hidden_shade);
  g_test_add_func ("/shade/hidden/minutes-from-seconds", test_hidden_minutes);
  g_test_add_func ("/shade/hidden/format-minutes", test_hidden_format);
  return g_test_run ();
}
