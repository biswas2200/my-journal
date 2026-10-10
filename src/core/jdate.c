/* jdate: local calendar days. See jdate.h. */
#include "jdate.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

/* English names on purpose: the UI is English and this keeps formatting
 * independent of the system locale. */
static const char *const MONTH_NAMES[12] = {
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December",
};
static const char *const WEEKDAY_NAMES[7] = {
  "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday",
};

int
jr_days_in_month (int year, int month)
{
  static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (month < 1 || month > 12)
    return 0;
  if (month == 2 && g_date_is_leap_year ((GDateYear) year))
    return 29;
  return days[month - 1];
}

gboolean
jr_day_valid (JrDay d)
{
  return d.year >= 1 && d.year <= 9999 && d.month >= 1 && d.month <= 12 &&
         d.day >= 1 && d.day <= jr_days_in_month (d.year, d.month);
}

JrDay
jr_day_from_time (gint64 t)
{
  time_t tt = (time_t) t;
  struct tm tm;
  localtime_r (&tt, &tm);
  return (JrDay){ tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday };
}

JrDay
jr_day_today (void)
{
  return jr_day_from_time ((gint64) time (NULL));
}

void
jr_day_to_iso (JrDay d, char out[JR_ISO_LEN])
{
  g_snprintf (out, JR_ISO_LEN, "%04d-%02d-%02d", d.year, d.month, d.day);
}

/* Reads exactly `n` digits from `s`; returns -1 if any is not a digit. */
static int
read_digits (const char *s, int n)
{
  int v = 0;
  for (int i = 0; i < n; i++)
    {
      if (!g_ascii_isdigit (s[i]))
        return -1;
      v = v * 10 + (s[i] - '0');
    }
  return v;
}

gboolean
jr_day_from_iso (const char *s, JrDay *out)
{
  if (s == NULL || strlen (s) != 10 || s[4] != '-' || s[7] != '-')
    return FALSE;
  JrDay d = { read_digits (s, 4), read_digits (s + 5, 2), read_digits (s + 8, 2) };
  if (!jr_day_valid (d))
    return FALSE;
  *out = d;
  return TRUE;
}

int
jr_day_compare (JrDay a, JrDay b)
{
  if (a.year != b.year)
    return a.year < b.year ? -1 : 1;
  if (a.month != b.month)
    return a.month < b.month ? -1 : 1;
  if (a.day != b.day)
    return a.day < b.day ? -1 : 1;
  return 0;
}

/* GDate on the stack: no allocation. Julian day numbers make arithmetic easy. */
static guint32
to_julian (JrDay d)
{
  GDate g;
  g_date_clear (&g, 1);
  g_date_set_dmy (&g, (GDateDay) d.day, (GDateMonth) d.month, (GDateYear) d.year);
  return g_date_get_julian (&g);
}

static JrDay
from_julian (guint32 j)
{
  GDate g;
  g_date_clear (&g, 1);
  g_date_set_julian (&g, j);
  return (JrDay){ g_date_get_year (&g), g_date_get_month (&g), g_date_get_day (&g) };
}

JrDay
jr_day_add (JrDay d, int days)
{
  return from_julian ((guint32) ((gint64) to_julian (d) + days));
}

int
jr_day_diff (JrDay from, JrDay to)
{
  return (int) ((gint64) to_julian (to) - (gint64) to_julian (from));
}

int
jr_day_weekday (JrDay d)
{
  /* Julian day 1 (1 Jan year 1) was a Monday. */
  return (int) ((to_julian (d) - 1) % 7) + 1;
}

/* Month from a word: at least the first three letters of its name. */
static int
parse_month_word (const char *w)
{
  gsize n = strlen (w);
  if (n < 3)
    return 0;
  for (int m = 0; m < 12; m++)
    if (n <= strlen (MONTH_NAMES[m]) && g_ascii_strncasecmp (w, MONTH_NAMES[m], n) == 0)
      return m + 1;
  return 0;
}

static int
parse_number_word (const char *w, gsize max_len)
{
  gsize n = strlen (w);
  if (n == 0 || n > max_len)
    return -1;
  return read_digits (w, (int) n);
}

gboolean
jr_day_parse_input (const char *text, JrDay today, JrDay *out)
{
  if (text == NULL)
    return FALSE;

  /* Trim, then copy. Text longer than any date is refused, never cut
   * short: a cut could turn "4 Oct <many spaces> 2025" into "4 Oct". */
  while (g_ascii_isspace (*text))
    text++;
  gsize len = strlen (text);
  while (len > 0 && g_ascii_isspace (text[len - 1]))
    len--;
  char buf[64];
  if (len >= sizeof buf)
    return FALSE;
  memcpy (buf, text, len);
  buf[len] = '\0';

  JrDay d = { 0, 0, 0 };
  gboolean have_year = FALSE;

  if (g_ascii_strcasecmp (buf, "today") == 0)
    d = today;
  else if (g_ascii_strcasecmp (buf, "yesterday") == 0)
    d = jr_day_add (today, -1);
  else if (jr_day_from_iso (buf, &d))
    have_year = TRUE;
  else
    {
      /* Split on spaces and commas into at most three words. */
      char *words[3];
      int n = 0;
      char *save = NULL;
      for (char *tok = strtok_r (buf, " ,", &save); tok != NULL;
           tok = strtok_r (NULL, " ,", &save))
        {
          if (n == 3)
            return FALSE;
          words[n++] = tok;
        }
      if (n < 2)
        return FALSE;

      /* "4 Oct" or "Oct 4", then an optional 4-digit year. */
      int month = parse_month_word (words[0]);
      int day = parse_number_word (words[1], 2);
      if (month == 0)
        {
          month = parse_month_word (words[1]);
          day = parse_number_word (words[0], 2);
        }
      if (month == 0 || day < 1)
        return FALSE;

      d = (JrDay){ today.year, month, day };
      if (n == 3)
        {
          if (strlen (words[2]) != 4)
            return FALSE;
          d.year = parse_number_word (words[2], 4);
          if (d.year < 1)
            return FALSE;
          have_year = TRUE;
        }
    }

  /* Without a year: this year's day, or last year's if this year's is
   * still to come or does not exist (29 Feb typed in January 2029 means
   * 29 Feb 2028). */
  if (!have_year && (!jr_day_valid (d) || jr_day_compare (d, today) > 0))
    d.year--;
  if (!jr_day_valid (d) || jr_day_compare (d, today) > 0)
    return FALSE;

  *out = d;
  return TRUE;
}

const char *
jr_month_name (int month)
{
  return (month >= 1 && month <= 12) ? MONTH_NAMES[month - 1] : "";
}

const char *
jr_weekday_name (JrDay d)
{
  return WEEKDAY_NAMES[jr_day_weekday (d) - 1];
}

const char *
jr_weekday_abbr (JrDay d)
{
  static const char *const abbr[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
  return abbr[jr_day_weekday (d) - 1];
}

void
jr_day_format_short (JrDay d, char *buf, gsize len)
{
  g_snprintf (buf, len, "%s %d %.3s", jr_weekday_abbr (d), d.day, jr_month_name (d.month));
}

void
jr_day_format_long (JrDay d, char *buf, gsize len)
{
  g_snprintf (buf, len, "%s %d %.3s %d", jr_weekday_abbr (d), d.day,
              jr_month_name (d.month), d.year);
}

void
jr_format_clock (gint64 t, char *buf, gsize len)
{
  time_t tt = (time_t) t;
  struct tm tm;
  localtime_r (&tt, &tm);
  int h12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
  g_snprintf (buf, len, "%02d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}
