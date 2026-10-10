/* Tests for local calendar days (src/core/jdate.c), LeetCode style:
 * tables of input -> expected output with their edge cases (see
 * cases.h), then hidden tests that check every day from 1900 to 2200 and
 * thousands of random inputs against slow references that are obviously
 * right. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include "cases.h"
#include "jdate.h"

#define VALID TRUE
#define INVALID FALSE
#define NO_DAY { 0, 0, 0 }

/* 2026-10-06 00:00 UTC, a Tuesday. */
#define MIDNIGHT G_GINT64_CONSTANT (1791244800)
#define AT(h, m) (MIDNIGHT + (h) * 3600 + (m) * 60)

#define SP5 "     "
#define SP50 SP5 SP5 SP5 SP5 SP5 SP5 SP5 SP5 SP5 SP5

typedef char DayText[16];

/* "2026-10-04", or "invalid". */
static const char *
show (gboolean ok, JrDay d, DayText buf)
{
  if (!ok)
    return "invalid";
  g_snprintf (buf, sizeof (DayText), "%04d-%02d-%02d", d.year, d.month, d.day);
  return buf;
}

static gboolean
same_day (JrDay a, JrDay b)
{
  return a.year == b.year && a.month == b.month && a.day == b.day;
}

static void
set_tz (const char *tz)
{
  setenv ("TZ", tz, 1);
  tzset ();
}

/* Constraints: month 1..12 of any year, Gregorian leap years; 0 otherwise. */
static void
test_days_in_month (void)
{
  static const struct {
    const char *name;
    int year, month, expected;
  } cases[] = {
    { "January", 2026, 1, 31 },
    { "February", 2026, 2, 28 },
    { "leap February", 2028, 2, 29 },
    { "century: not leap", 1900, 2, 28 },
    { "century: not leap (2100)", 2100, 2, 28 },
    { "every 400 years: leap", 2000, 2, 29 },
    { "April", 2026, 4, 30 },
    { "June", 2026, 6, 30 },
    { "September", 2026, 9, 30 },
    { "November", 2026, 11, 30 },
    { "December", 2026, 12, 31 },
    { "month 0", 2026, 0, 0 },
    { "month 13", 2026, 13, 0 },
    { "negative month", 2026, -1, 0 },
  };
  JrCases c = { .table = "days-in-month" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      int got = jr_days_in_month (cases[i].year, cases[i].month);
      jr_case (&c, cases[i].name, got == cases[i].expected, "Input %04d-%02d  Expected %d  Got %d",
               cases[i].year, cases[i].month, cases[i].expected, got);
    }
  jr_cases_done (&c);
}

/* Constraints: exactly "YYYY-MM-DD" in ASCII digits, year 0001..9999,
 * and a day that exists on the calendar. Valid days turn back into the
 * same text. */
static void
test_iso (void)
{
  static const struct {
    const char *name;
    const char *input;
    gboolean ok;
    JrDay expected;
  } cases[] = {
    { "basic", "2026-10-04", VALID, { 2026, 10, 4 } },
    { "first day there is", "0001-01-01", VALID, { 1, 1, 1 } },
    { "last day there is", "9999-12-31", VALID, { 9999, 12, 31 } },
    { "year zero", "0000-01-01", INVALID, NO_DAY },
    { "leap day", "2028-02-29", VALID, { 2028, 2, 29 } },
    { "not a leap year", "2026-02-29", INVALID, NO_DAY },
    { "century: not leap", "1900-02-29", INVALID, NO_DAY },
    { "every 400 years: leap", "2000-02-29", VALID, { 2000, 2, 29 } },
    { "30 February", "2026-02-30", INVALID, NO_DAY },
    { "31 April", "2026-04-31", INVALID, NO_DAY },
    { "month 00", "2026-00-10", INVALID, NO_DAY },
    { "month 13", "2026-13-10", INVALID, NO_DAY },
    { "day 00", "2026-10-00", INVALID, NO_DAY },
    { "day 32", "2026-10-32", INVALID, NO_DAY },
    { "empty", "", INVALID, NO_DAY },
    { "NULL", NULL, INVALID, NO_DAY },
    { "one-digit month", "2026-1-04", INVALID, NO_DAY },
    { "trailing junk", "2026-10-04x", INVALID, NO_DAY },
    { "trailing newline", "2026-10-04\n", INVALID, NO_DAY },
    { "leading space", " 2026-10-04", INVALID, NO_DAY },
    { "slashes", "2026/10/04", INVALID, NO_DAY },
    { "letter in the year", "20a6-10-04", INVALID, NO_DAY },
    { "plus sign", "2026-10-+4", INVALID, NO_DAY },
    { "minus sign", "2026-10--4", INVALID, NO_DAY },
    { "full-width digits", "\xef\xbc\x92\xef\xbc\x90\xef\xbc\x92\xef\xbc\x96-10-04", INVALID, NO_DAY },
  };
  JrCases c = { .table = "iso" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      const char *input = cases[i].input ? cases[i].input : "(null)";
      JrDay got = NO_DAY;
      gboolean ok = jr_day_from_iso (cases[i].input, &got);
      DayText e, g;
      jr_case (&c, cases[i].name, ok == cases[i].ok && (!ok || same_day (got, cases[i].expected)),
               "Input \"%s\"  Expected %s  Got %s", input, show (cases[i].ok, cases[i].expected, e),
               show (ok, got, g));
      if (ok)
        {
          char iso[JR_ISO_LEN];
          jr_day_to_iso (got, iso);
          jr_case (&c, cases[i].name, strcmp (iso, input) == 0,
                   "Input %s, back to text  Expected \"%s\"  Got \"%s\"", g, input, iso);
        }
    }
  jr_cases_done (&c);
}

/* Constraints: the start and the result lie in 0001-01-01..9999-12-31.
 * (The app itself only counts back up to a week from today.) Adding n
 * days and the difference between the two days must agree. */
static void
test_add_and_diff (void)
{
  static const struct {
    const char *name;
    JrDay from;
    int days;
    JrDay expected;
  } cases[] = {
    { "next day", { 2026, 10, 6 }, 1, { 2026, 10, 7 } },
    { "zero days", { 2026, 10, 6 }, 0, { 2026, 10, 6 } },
    { "back over a month end", { 2026, 10, 1 }, -1, { 2026, 9, 30 } },
    { "the 7-day chart's first day", { 2026, 10, 6 }, -6, { 2026, 9, 30 } },
    { "into a leap day", { 2028, 2, 28 }, 1, { 2028, 2, 29 } },
    { "out of a leap day", { 2028, 2, 29 }, 1, { 2028, 3, 1 } },
    { "no leap day", { 2026, 2, 28 }, 1, { 2026, 3, 1 } },
    { "over the year end", { 2026, 12, 31 }, 1, { 2027, 1, 1 } },
    { "back over the year end", { 2027, 1, 1 }, -1, { 2026, 12, 31 } },
    { "a whole leap year", { 2028, 1, 1 }, 366, { 2029, 1, 1 } },
    { "a whole common year", { 2026, 1, 1 }, 365, { 2027, 1, 1 } },
    { "400 years", { 2000, 1, 1 }, 146097, { 2400, 1, 1 } },
    { "back to the first day there is", { 1, 1, 2 }, -1, { 1, 1, 1 } },
    { "on to the last day there is", { 9999, 12, 30 }, 1, { 9999, 12, 31 } },
    { "the whole range", { 1, 1, 1 }, 3652058, { 9999, 12, 31 } },
    { "the whole range, backwards", { 9999, 12, 31 }, -3652058, { 1, 1, 1 } },
  };
  JrCases c = { .table = "add-and-diff" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      DayText f, e, g;
      JrDay got = jr_day_add (cases[i].from, cases[i].days);
      jr_case (&c, cases[i].name, same_day (got, cases[i].expected),
               "Input %s %+d days  Expected %s  Got %s", show (TRUE, cases[i].from, f), cases[i].days,
               show (TRUE, cases[i].expected, e), show (TRUE, got, g));
      int diff = jr_day_diff (cases[i].from, cases[i].expected);
      jr_case (&c, cases[i].name, diff == cases[i].days, "Input days from %s to %s  Expected %d  Got %d",
               f, e, cases[i].days, diff);
      int order = jr_day_compare (cases[i].from, cases[i].expected);
      int want = (cases[i].days > 0) - (cases[i].days < 0);
      jr_case (&c, cases[i].name, (order > 0) - (order < 0) == -want,
               "Input compare %s with %s  Expected %d  Got %d", f, e, -want, order);
    }
  jr_cases_done (&c);
}

/* Constraints: any valid day. Monday = 1 .. Sunday = 7. */
static void
test_weekday (void)
{
  static const struct {
    const char *name;
    JrDay day;
    int weekday;
    const char *full, *abbr;
  } cases[] = {
    { "a Tuesday", { 2026, 10, 6 }, 2, "Tuesday", "Tue" },
    { "a Monday: the week starts", { 2026, 10, 5 }, 1, "Monday", "Mon" },
    { "a Sunday: the week ends", { 2026, 10, 4 }, 7, "Sunday", "Sun" },
    { "first day there is", { 1, 1, 1 }, 1, "Monday", "Mon" },
    { "1900", { 1900, 1, 1 }, 1, "Monday", "Mon" },
    { "Unix epoch", { 1970, 1, 1 }, 4, "Thursday", "Thu" },
    { "2000", { 2000, 1, 1 }, 6, "Saturday", "Sat" },
    { "a leap day", { 2000, 2, 29 }, 2, "Tuesday", "Tue" },
    { "last day there is", { 9999, 12, 31 }, 5, "Friday", "Fri" },
  };
  JrCases c = { .table = "weekday" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      DayText d;
      show (TRUE, cases[i].day, d);
      int got = jr_day_weekday (cases[i].day);
      jr_case (&c, cases[i].name, got == cases[i].weekday, "Input %s  Expected %d  Got %d", d,
               cases[i].weekday, got);
      const char *full = jr_weekday_name (cases[i].day), *abbr = jr_weekday_abbr (cases[i].day);
      jr_case (&c, cases[i].name, strcmp (full, cases[i].full) == 0 && strcmp (abbr, cases[i].abbr) == 0,
               "Input %s  Expected %s/%s  Got %s/%s", d, cases[i].full, cases[i].abbr, full, abbr);
    }
  jr_cases_done (&c);
}

/* Constraints: what someone types in "Jump to a date". At most 63
 * characters once trimmed (longer is refused, never cut short); "today",
 * "yesterday", "YYYY-MM-DD", or a day and a month name of 3+ letters in
 * either order with an optional 4-digit year. Without a year it means the
 * latest such day that is not in the future, this year or last. The
 * future never opens. */
static void
test_parse_input (void)
{
#define TODAY { 2026, 10, 6 }
  static const struct {
    const char *name;
    JrDay today;
    const char *input;
    gboolean ok;
    JrDay expected;
  } cases[] = {
    { "day month", TODAY, "4 Oct", VALID, { 2026, 10, 4 } },
    { "month day", TODAY, "Oct 4", VALID, { 2026, 10, 4 } },
    { "full month name and a year", TODAY, "4 october 2025", VALID, { 2025, 10, 4 } },
    { "comma before the year", TODAY, "Sep 30, 2026", VALID, { 2026, 9, 30 } },
    { "comma, no space", TODAY, "4,Oct", VALID, { 2026, 10, 4 } },
    { "ISO with spaces around", TODAY, "  2026-10-04 ", VALID, { 2026, 10, 4 } },
    { "today", TODAY, "today", VALID, TODAY },
    { "yesterday", TODAY, "Yesterday", VALID, { 2026, 10, 5 } },
    { "shouting", TODAY, "TODAY", VALID, TODAY },
    { "mixed-case month", TODAY, "4 oCT", VALID, { 2026, 10, 4 } },
    { "4-letter month", TODAY, "4 Sept", VALID, { 2026, 9, 4 } },
    { "leading zero", TODAY, "04 Oct", VALID, { 2026, 10, 4 } },
    { "today's date without a year", TODAY, "6 Oct", VALID, TODAY },
    { "tomorrow's date: last year", TODAY, "7 Oct", VALID, { 2025, 10, 7 } },
    { "Christmas: last year", TODAY, "25 Dec", VALID, { 2025, 12, 25 } },
    { "first day there is", TODAY, "1 Jan 0001", VALID, { 1, 1, 1 } },
    /* New Year. */
    { "31 Dec on New Year's Day", { 2026, 1, 1 }, "31 Dec", VALID, { 2025, 12, 31 } },
    { "yesterday on New Year's Day", { 2026, 1, 1 }, "yesterday", VALID, { 2025, 12, 31 } },
    { "1 Jan on New Year's Day", { 2026, 1, 1 }, "1 Jan", VALID, { 2026, 1, 1 } },
    /* 29 February. */
    { "29 Feb, after it", { 2028, 3, 1 }, "29 Feb", VALID, { 2028, 2, 29 } },
    { "29 Feb, early the next year", { 2029, 1, 15 }, "29 Feb", VALID, { 2028, 2, 29 } },
    { "29 Feb, a year on", { 2029, 2, 28 }, "Feb 29", VALID, { 2028, 2, 29 } },
    { "29 Feb, none since last year", { 2028, 2, 28 }, "29 Feb", INVALID, NO_DAY },
    { "29 Feb, years ago", TODAY, "29 Feb", INVALID, NO_DAY },
    { "29 Feb of a common year", TODAY, "29 Feb 2025", INVALID, NO_DAY },
    /* Not a date. */
    { "empty", TODAY, "", INVALID, NO_DAY },
    { "only spaces", TODAY, "   ", INVALID, NO_DAY },
    { "NULL", TODAY, NULL, INVALID, NO_DAY },
    { "a word", TODAY, "hello", INVALID, NO_DAY },
    { "only a number", TODAY, "4", INVALID, NO_DAY },
    { "2-letter month", TODAY, "4 Oc", INVALID, NO_DAY },
    { "month name too long", TODAY, "4 Octobers", INVALID, NO_DAY },
    { "two numbers", TODAY, "4 10", INVALID, NO_DAY },
    { "two months", TODAY, "Oct Nov", INVALID, NO_DAY },
    { "day 0", TODAY, "0 Oct", INVALID, NO_DAY },
    { "31 February", TODAY, "31 Feb", INVALID, NO_DAY },
    { "31 November", TODAY, "31 Nov", INVALID, NO_DAY },
    { "3-digit day", TODAY, "004 Oct", INVALID, NO_DAY },
    { "negative day", TODAY, "-4 Oct", INVALID, NO_DAY },
    { "2-digit year", TODAY, "4 Oct 25", INVALID, NO_DAY },
    { "5-digit year", TODAY, "4 Oct 02025", INVALID, NO_DAY },
    { "year 0", TODAY, "4 Oct 0000", INVALID, NO_DAY },
    { "four words", TODAY, "4 Oct 2025 now", INVALID, NO_DAY },
    { "dash between", TODAY, "4-Oct", INVALID, NO_DAY },
    { "full-width digit", TODAY, "\xef\xbc\x94 Oct", INVALID, NO_DAY },
    /* The future never opens. */
    { "tomorrow, ISO", TODAY, "2026-10-07", INVALID, NO_DAY },
    { "next year", TODAY, "4 Oct 2027", INVALID, NO_DAY },
    /* Length: 63 characters once trimmed. */
    { "63 characters", TODAY, "4 Oct" SP50 "    2025", VALID, { 2025, 10, 4 } },
    { "64 characters", TODAY, "4 Oct" SP50 SP5 "2025", INVALID, NO_DAY },
    { "too long: the year would be cut off", TODAY, "4 Oct" SP50 SP5 "   2025", INVALID, NO_DAY },
    { "long padding is trimmed first", TODAY, SP50 SP5 SP5 "4 Oct 2025" SP50, VALID, { 2025, 10, 4 } },
  };
#undef TODAY
  JrCases c = { .table = "jump-to-date" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      JrDay got = NO_DAY;
      gboolean ok = jr_day_parse_input (cases[i].input, cases[i].today, &got);
      DayText t, e, g;
      jr_case (&c, cases[i].name, ok == cases[i].ok && (!ok || same_day (got, cases[i].expected)),
               "Input \"%s\" (today %s)  Expected %s  Got %s",
               cases[i].input ? cases[i].input : "(null)", show (TRUE, cases[i].today, t),
               show (cases[i].ok, cases[i].expected, e), show (ok, got, g));
    }
  jr_cases_done (&c);
}

/* Constraints: any valid day; English names whatever the system locale. */
static void
test_format_day (void)
{
  static const struct {
    const char *name;
    JrDay day;
    const char *lng, *shrt;
  } cases[] = {
    { "basic", { 2026, 10, 6 }, "Tue 6 Oct 2026", "Tue 6 Oct" },
    { "two-digit day", { 2026, 12, 25 }, "Fri 25 Dec 2026", "Fri 25 Dec" },
    { "first of the month", { 2026, 3, 1 }, "Sun 1 Mar 2026", "Sun 1 Mar" },
    { "long month name, cut to 3", { 2026, 9, 30 }, "Wed 30 Sep 2026", "Wed 30 Sep" },
    { "year 1", { 1, 1, 1 }, "Mon 1 Jan 1", "Mon 1 Jan" },
  };
  JrCases c = { .table = "format-day" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      DayText d;
      char lng[32], shrt[32];
      show (TRUE, cases[i].day, d);
      jr_day_format_long (cases[i].day, lng, sizeof lng);
      jr_day_format_short (cases[i].day, shrt, sizeof shrt);
      jr_case (&c, cases[i].name, strcmp (lng, cases[i].lng) == 0, "Input %s  Expected \"%s\"  Got \"%s\"",
               d, cases[i].lng, lng);
      jr_case (&c, cases[i].name, strcmp (shrt, cases[i].shrt) == 0,
               "Input %s (short)  Expected \"%s\"  Got \"%s\"", d, cases[i].shrt, shrt);
    }
  static const struct {
    int month;
    const char *expected;
  } months[] = {
    { 1, "January" }, { 2, "February" }, { 9, "September" }, { 12, "December" },
    { 0, "" },        { 13, "" },        { -1, "" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (months); i++)
    {
      const char *got = jr_month_name (months[i].month);
      jr_case (&c, "month name", strcmp (got, months[i].expected) == 0,
               "Input %d  Expected \"%s\"  Got \"%s\"", months[i].month, months[i].expected, got);
    }
  jr_cases_done (&c);
}

/* Constraints: a 12-hour clock with a leading zero; seconds are dropped. */
static void
test_format_clock (void)
{
  static const struct {
    const char *name;
    gint64 t;
    const char *expected;
  } cases[] = {
    { "midnight", AT (0, 0), "12:00 AM" },
    { "just after midnight", AT (0, 14), "12:14 AM" },
    { "last minute of the 12 AM hour", AT (0, 59), "12:59 AM" },
    { "1 AM", AT (1, 0), "01:00 AM" },
    { "morning", AT (9, 14), "09:14 AM" },
    { "last minute before noon", AT (11, 59), "11:59 AM" },
    { "noon", AT (12, 0), "12:00 PM" },
    { "just after noon", AT (12, 5), "12:05 PM" },
    { "1 PM", AT (13, 0), "01:00 PM" },
    { "evening", AT (22, 42), "10:42 PM" },
    { "last minute of the day", AT (23, 59), "11:59 PM" },
    { "seconds are dropped", AT (23, 59) + 59, "11:59 PM" },
  };
  set_tz ("UTC0");
  JrCases c = { .table = "format-clock" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      char buf[16];
      jr_format_clock (cases[i].t, buf, sizeof buf);
      jr_case (&c, cases[i].name, strcmp (buf, cases[i].expected) == 0,
               "Input %" G_GINT64_FORMAT " (UTC)  Expected \"%s\"  Got \"%s\"", cases[i].t,
               cases[i].expected, buf);
    }
  jr_cases_done (&c);
}

/* Constraints: any buffer size from 1 up. Text that does not fit is cut,
 * still ends in NUL, and nothing past the buffer is touched. */
static void
test_format_small_buffers (void)
{
  set_tz ("UTC0");
  JrCases c = { .table = "format-small-buffers" };
  const char *full_day = "Tue 6 Oct 2026", *full_clock = "10:42 PM";
  for (gsize len = 1; len <= 16; len++)
    {
      char buf[24];
      for (int which = 0; which < 2; which++)
        {
          const char *full = which == 0 ? full_day : full_clock;
          memset (buf, '#', sizeof buf);
          if (which == 0)
            jr_day_format_long ((JrDay){ 2026, 10, 6 }, buf, len);
          else
            jr_format_clock (AT (22, 42), buf, len);
          gsize want = MIN (len - 1, strlen (full));
          gboolean untouched = TRUE;
          for (gsize k = len; k < sizeof buf; k++)
            untouched = untouched && buf[k] == '#';
          jr_case (&c, which == 0 ? "day" : "clock",
                   strlen (buf) == want && strncmp (buf, full, want) == 0 && untouched,
                   "Input buffer of %" G_GSIZE_FORMAT "  Expected \"%.*s\"  Got \"%.*s\"%s", len,
                   (int) want, full, (int) MIN (strnlen (buf, sizeof buf), 23), buf,
                   untouched ? "" : " and wrote past the end");
        }
    }
  jr_cases_done (&c);
}

/* Constraints: the day a moment falls on where the user is. Spec: an
 * entry written at 00:30 belongs to the new day. */
static void
test_day_from_time (void)
{
  static const struct {
    const char *name;
    const char *tz;
    gint64 t;
    JrDay expected;
  } cases[] = {
    { "23:59 is still that day", "UTC0", AT (23, 59), { 2026, 10, 6 } },
    { "00:00 starts the new day", "UTC0", AT (24, 0), { 2026, 10, 7 } },
    { "00:30 belongs to the new day", "UTC0", AT (24, 30), { 2026, 10, 7 } },
    { "a second before midnight", "UTC0", MIDNIGHT - 1, { 2026, 10, 5 } },
    { "India (+5:30): already tomorrow", "IST-5:30", AT (20, 0), { 2026, 10, 7 } },
    { "India (+5:30): just before its midnight", "IST-5:30", AT (18, 29), { 2026, 10, 6 } },
    { "Nepal (+5:45)", "NPT-5:45", AT (18, 15), { 2026, 10, 7 } },
    { "Hawaii (-10): still yesterday", "HST10", AT (9, 0), { 2026, 10, 5 } },
    { "furthest ahead (+14)", "<+14>-14", AT (10, 0), { 2026, 10, 7 } },
    { "furthest ahead (+14), a second earlier", "<+14>-14", AT (10, 0) - 1, { 2026, 10, 6 } },
    { "furthest behind (-12)", "<-12>12", AT (11, 59), { 2026, 10, 5 } },
    { "summer time (UTC-4)", "EST5EDT,M3.2.0,M11.1.0", 1782880200, { 2026, 7, 1 } },
    { "just before summer time ends", "EST5EDT,M3.2.0,M11.1.0", 1793511000, { 2026, 11, 1 } },
    { "after summer time ends (UTC-5)", "EST5EDT,M3.2.0,M11.1.0", 1793593800, { 2026, 11, 1 } },
    { "Unix epoch", "UTC0", 0, { 1970, 1, 1 } },
    { "before 1970", "UTC0", -1, { 1969, 12, 31 } },
    { "last second of 32-bit time", "UTC0", G_GINT64_CONSTANT (2147483647), { 2038, 1, 19 } },
    { "past 32-bit time", "UTC0", G_GINT64_CONSTANT (2147483648) + 86400, { 2038, 1, 20 } },
    { "last second there is", "UTC0", G_GINT64_CONSTANT (253402300799), { 9999, 12, 31 } },
  };
  JrCases c = { .table = "day-from-time" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      set_tz (cases[i].tz);
      JrDay got = jr_day_from_time (cases[i].t);
      DayText e, g;
      jr_case (&c, cases[i].name, same_day (got, cases[i].expected),
               "Input %" G_GINT64_FORMAT " in %s  Expected %s  Got %s", cases[i].t, cases[i].tz,
               show (TRUE, cases[i].expected, e), show (TRUE, got, g));
    }
  set_tz ("UTC0");
  jr_cases_done (&c);
}

/* ---- Hidden tests --------------------------------------------------- */

#define WALK_FROM 1900 /* 1 Jan 1900 was a Monday */
#define WALK_TO 2200

/* The plainest possible "next day", built on days_in_month (checked above). */
static JrDay
next_day (JrDay d)
{
  if (++d.day > jr_days_in_month (d.year, d.month))
    {
      d.day = 1;
      if (++d.month > 12)
        {
          d.month = 1;
          d.year++;
        }
    }
  return d;
}

static JrDay
prev_day (JrDay d)
{
  if (--d.day < 1)
    {
      if (--d.month < 1)
        {
          d.month = 12;
          d.year--;
        }
      d.day = jr_days_in_month (d.year, d.month);
    }
  return d;
}

/* Days since 1970-01-01 by the C library's calendar, not GLib's. */
static gint64
libc_days (JrDay d)
{
  struct tm tm = { 0 };
  tm.tm_year = d.year - 1900;
  tm.tm_mon = d.month - 1;
  tm.tm_mday = d.day;
  tm.tm_hour = 12;
  return ((gint64) timegm (&tm) - 12 * 3600) / 86400;
}

/* Hidden test: every day from 1900 to 2200, one at a time. */
static void
test_hidden_every_day (void)
{
  const JrDay start = { WALK_FROM, 1, 1 }, epoch = { 1970, 1, 1 };
  JrCases c = { .table = "hidden: every day 1900-2200" };
  JrDay prev = start;
  int i = 0;
  for (JrDay d = start; d.year <= WALK_TO; prev = d, d = next_day (d), i++)
    {
      DayText t, g;
      show (TRUE, d, t);
      jr_case (&c, "valid", jr_day_valid (d), "Input %s  Expected valid  Got invalid", t);
      if (d.day == jr_days_in_month (d.year, d.month))
        jr_case (&c, "the day after a month's last", !jr_day_valid ((JrDay){ d.year, d.month, d.day + 1 }),
                 "Input %04d-%02d-%02d  Expected invalid  Got valid", d.year, d.month, d.day + 1);
      JrDay added = jr_day_add (start, i);
      jr_case (&c, "add", same_day (added, d), "Input 1900-01-01 %+d days  Expected %s  Got %s", i, t,
               show (TRUE, added, g));
      int diff = jr_day_diff (start, d);
      jr_case (&c, "diff", diff == i, "Input days from 1900-01-01 to %s  Expected %d  Got %d", t, i, diff);
      gint64 libc = libc_days (d), ours = jr_day_diff (epoch, d);
      jr_case (&c, "matches the C library", ours == libc,
               "Input days from 1970-01-01 to %s  Expected %" G_GINT64_FORMAT "  Got %" G_GINT64_FORMAT, t,
               libc, ours);
      int weekday = jr_day_weekday (d);
      jr_case (&c, "weekday", weekday == i % 7 + 1, "Input %s  Expected %d  Got %d", t, i % 7 + 1, weekday);
      if (i > 0)
        jr_case (&c, "order", jr_day_compare (prev, d) < 0 && jr_day_compare (d, prev) > 0 &&
                                  jr_day_compare (d, d) == 0,
                 "Input %s after the day before  Expected later  Got not later", t);
      char iso[JR_ISO_LEN];
      JrDay back = NO_DAY;
      jr_day_to_iso (d, iso);
      jr_case (&c, "iso round trip", strcmp (iso, t) == 0 && jr_day_from_iso (iso, &back) && same_day (back, d),
               "Input %s  Expected \"%s\"  Got \"%s\"", t, t, iso);
    }
  jr_cases_done (&c);
}

/* Hidden test: random pairs of days; adding their distance gets from one
 * to the other. */
static void
test_hidden_random_pairs (void)
{
  GArray *days = g_array_new (FALSE, FALSE, sizeof (JrDay));
  for (JrDay d = { WALK_FROM, 1, 1 }; d.year <= WALK_TO; d = next_day (d))
    g_array_append_val (days, d);
  JrCases c = { .table = "hidden: random pairs" };
  for (int n = 0; n < 20000; n++)
    {
      int i = g_test_rand_int_range (0, (gint32) days->len);
      int j = g_test_rand_int_range (0, (gint32) days->len);
      JrDay a = g_array_index (days, JrDay, i), b = g_array_index (days, JrDay, j);
      JrDay got = jr_day_add (a, j - i);
      DayText ta, tb, g;
      show (TRUE, a, ta);
      show (TRUE, b, tb);
      jr_case (&c, "add", same_day (got, b), "Input %s %+d days  Expected %s  Got %s", ta, j - i, tb,
               show (TRUE, got, g));
      int diff = jr_day_diff (a, b), order = jr_day_compare (a, b);
      jr_case (&c, "diff and order", diff == j - i && (order > 0) - (order < 0) == (i > j) - (i < j),
               "Input %s to %s  Expected %d days  Got %d (compare %d)", ta, tb, j - i, diff, order);
    }
  g_array_unref (days);
  jr_cases_done (&c);
}

static gint64
floor_div (gint64 a, gint64 b)
{
  gint64 q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* Hidden test: random moments, and the seconds either side of random
 * local midnights, in fixed-offset zones. The day must be
 * floor((t + offset) / 86400) days after 1970-01-01. */
static void
test_hidden_day_from_time (void)
{
  static const struct {
    const char *tz;
    gint64 offset;
  } zones[] = {
    { "UTC0", 0 },          { "IST-5:30", 19800 },  { "NPT-5:45", 20700 },
    { "HST10", -36000 },    { "<+14>-14", 50400 },  { "<-12>12", -43200 },
  };
  const JrDay epoch = { 1970, 1, 1 };
  JrCases c = { .table = "hidden: day from time" };
  for (gsize z = 0; z < G_N_ELEMENTS (zones); z++)
    {
      set_tz (zones[z].tz);
      for (int n = 0; n < 4000; n++)
        {
          gint64 t;
          if (n % 2 == 0) /* anywhere from 1901 to 2514 */
            t = (gint64) g_test_rand_double_range (-2147483648.0, 17179869184.0);
          else /* a local midnight, or the second before it */
            t = (gint64) g_test_rand_int_range (-24000, 198000) * 86400 - zones[z].offset -
                g_test_rand_bit ();
          JrDay want = jr_day_add (epoch, (int) floor_div (t + zones[z].offset, 86400));
          JrDay got = jr_day_from_time (t);
          DayText e, g;
          jr_case (&c, zones[z].tz, same_day (got, want),
                   "Input %" G_GINT64_FORMAT " in %s  Expected %s  Got %s", t, zones[z].tz,
                   show (TRUE, want, e), show (TRUE, got, g));
        }
    }
  set_tz ("UTC0");
  jr_cases_done (&c);
}

/* The reference for a day and month without a year: walk back from
 * today, one day at a time, through this year and last. */
static gboolean
latest_past (JrDay today, int month, int day, JrDay *out)
{
  for (JrDay d = today; d.year >= today.year - 1; d = prev_day (d))
    if (d.month == month && d.day == day)
      {
        *out = d;
        return TRUE;
      }
  return FALSE;
}

static const char *const MONTHS[12] = {
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December",
};

/* A month written any way that is allowed: 3 letters up to the full
 * name, in any mix of case. */
static void
random_month (int month, char out[16])
{
  const char *name = MONTHS[month - 1];
  int n = g_test_rand_int_range (3, (gint32) strlen (name) + 1);
  for (int i = 0; i < n; i++)
    out[i] = g_test_rand_bit () ? g_ascii_toupper (name[i]) : g_ascii_tolower (name[i]);
  out[n] = '\0';
}

static const char *
random_gap (void)
{
  static const char *const gaps[] = { " ", "  ", ", ", " ,", "," };
  return gaps[g_test_rand_int_range (0, G_N_ELEMENTS (gaps))];
}

static const char *
random_pad (void)
{
  static const char *const pads[] = { "", " ", "   " };
  return pads[g_test_rand_int_range (0, G_N_ELEMENTS (pads))];
}

/* A day number, sometimes with a leading zero. */
static void
random_day_number (int day, char out[4])
{
  g_snprintf (out, 4, g_test_rand_bit () ? "%d" : "%02d", day);
}

static void
check_parse (JrCases *c, const char *name, const char *text, JrDay today, gboolean want_ok, JrDay want)
{
  JrDay got = NO_DAY;
  gboolean ok = jr_day_parse_input (text, today, &got);
  DayText t, e, g;
  jr_case (c, name, ok == want_ok && (!ok || same_day (got, want)),
           "Input \"%s\" (today %s)  Expected %s  Got %s", text, show (TRUE, today, t),
           show (want_ok, want, e), show (ok, got, g));
}

/* Hidden test: for many "todays", every month and day number without a
 * year, and every day from about a year back to a year ahead with one,
 * each written a random allowed way. */
static void
test_hidden_parse (void)
{
  static const JrDay tricky[] = {
    { 2026, 10, 6 }, { 2026, 1, 1 },  { 2026, 12, 31 }, { 2028, 2, 28 }, { 2028, 2, 29 },
    { 2028, 3, 1 },  { 2029, 1, 15 }, { 2029, 2, 28 },  { 2029, 3, 1 },  { 2000, 2, 29 },
  };
  JrCases c = { .table = "hidden: jump to a date" };
  for (int k = 0; k < 30; k++)
    {
      JrDay today = k < (int) G_N_ELEMENTS (tricky)
                        ? tricky[k]
                        : jr_day_add ((JrDay){ 1950, 1, 1 }, g_test_rand_int_range (0, 73000));
      char text[64], mon[16], num[4];
      for (int m = 1; m <= 12; m++)
        for (int d = 1; d <= 31; d++)
          {
            JrDay want = NO_DAY;
            gboolean want_ok = latest_past (today, m, d, &want);
            random_month (m, mon);
            random_day_number (d, num);
            if (g_test_rand_bit ())
              g_snprintf (text, sizeof text, "%s%s%s%s%s", random_pad (), num, random_gap (), mon, random_pad ());
            else
              g_snprintf (text, sizeof text, "%s%s%s%s%s", random_pad (), mon, random_gap (), num, random_pad ());
            check_parse (&c, "no year", text, today, want_ok, want);
          }
      for (int off = -400; off <= 400; off++)
        {
          JrDay d = jr_day_add (today, off);
          random_month (d.month, mon);
          random_day_number (d.day, num);
          switch (g_test_rand_int_range (0, 3))
            {
            case 0:
              g_snprintf (text, sizeof text, "%s%04d-%02d-%02d%s", random_pad (), d.year, d.month, d.day,
                          random_pad ());
              break;
            case 1:
              g_snprintf (text, sizeof text, "%s%s%s%s%s%04d%s", random_pad (), num, random_gap (), mon,
                          random_gap (), d.year, random_pad ());
              break;
            default:
              g_snprintf (text, sizeof text, "%s%s%s%s%s%04d%s", random_pad (), mon, random_gap (), num,
                          random_gap (), d.year, random_pad ());
              break;
            }
          check_parse (&c, "with a year", text, today, off <= 0, d);
        }
    }
  jr_cases_done (&c);
}

/* Max-size inputs: far longer than any date, or padded far beyond the
 * limit; refused or trimmed, never cut short, never a crash. */
static void
test_parse_huge_input (void)
{
  const JrDay today = { 2026, 10, 6 };
  JrCases c = { .table = "jump-to-date, huge input" };
  g_autofree char *junk = g_strnfill (100000, 'x');
  g_autofree char *spaces = g_strnfill (100000, ' ');
  g_autofree char *wide = g_strconcat ("4 Oct", spaces, "2025", NULL);
  g_autofree char *padded = g_strconcat (spaces, "4 Oct 2025", spaces, NULL);
  check_parse (&c, "100,000 letters", junk, today, INVALID, (JrDay) NO_DAY);
  check_parse (&c, "100,000 spaces", spaces, today, INVALID, (JrDay) NO_DAY);
  check_parse (&c, "100,000 spaces inside", wide, today, INVALID, (JrDay) NO_DAY);
  check_parse (&c, "100,000 spaces around", padded, today, VALID, (JrDay){ 2025, 10, 4 });
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  set_tz ("UTC0");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/jdate/days-in-month", test_days_in_month);
  g_test_add_func ("/jdate/iso", test_iso);
  g_test_add_func ("/jdate/add-and-diff", test_add_and_diff);
  g_test_add_func ("/jdate/weekday", test_weekday);
  g_test_add_func ("/jdate/jump-to-date", test_parse_input);
  g_test_add_func ("/jdate/format-day", test_format_day);
  g_test_add_func ("/jdate/format-clock", test_format_clock);
  g_test_add_func ("/jdate/format-small-buffers", test_format_small_buffers);
  g_test_add_func ("/jdate/day-from-time", test_day_from_time);
  g_test_add_func ("/jdate/hidden/every-day", test_hidden_every_day);
  g_test_add_func ("/jdate/hidden/random-pairs", test_hidden_random_pairs);
  g_test_add_func ("/jdate/hidden/day-from-time", test_hidden_day_from_time);
  g_test_add_func ("/jdate/hidden/jump-to-date", test_hidden_parse);
  g_test_add_func ("/jdate/huge-input", test_parse_huge_input);
  return g_test_run ();
}
