/* jdate: local calendar days, parsing and formatting.
 *
 * A JrDay is a plain value (no allocation). "Local" always means the
 * system timezone at the moment of the call, so an entry stamped at
 * 00:30 belongs to the new day.
 */
#pragma once

#include <glib.h>

#define JR_ISO_LEN 11 /* "YYYY-MM-DD" plus NUL */

typedef struct {
  int year;
  int month; /* 1..12 */
  int day;   /* 1..31 */
} JrDay;

gboolean    jr_day_valid        (JrDay d);
int         jr_days_in_month    (int year, int month);

/* Local calendar day that contains the unix time `t`. */
JrDay       jr_day_from_time    (gint64 t);
JrDay       jr_day_today        (void);

void        jr_day_to_iso       (JrDay d, char out[JR_ISO_LEN]);
gboolean    jr_day_from_iso     (const char *s, JrDay *out);

int         jr_day_compare      (JrDay a, JrDay b);
JrDay       jr_day_add          (JrDay d, int days);
int         jr_day_diff         (JrDay from, JrDay to); /* to - from, in days */
int         jr_day_weekday      (JrDay d);              /* 1 = Monday .. 7 = Sunday */

/* Parses what the user types in "Jump to a date": "4 Oct", "Oct 4",
 * "4 October 2025", "2026-10-04", "today", "yesterday". A date without a
 * year that is still to come this year, or does not exist this year
 * (29 Feb), means last year. Future dates fail, and so does text over 63
 * characters once trimmed. */
gboolean    jr_day_parse_input  (const char *text, JrDay today, JrDay *out);

void        jr_day_format_long  (JrDay d, char *buf, gsize len); /* "Tue 6 Oct 2026" */
void        jr_day_format_short (JrDay d, char *buf, gsize len); /* "Tue 6 Oct" */
const char *jr_weekday_name     (JrDay d);                       /* "Tuesday" */
const char *jr_weekday_abbr     (JrDay d);                       /* "Tue" */
const char *jr_month_name       (int month);                     /* "October" */

/* 12-hour wall-clock time of `t`, e.g. "09:14 AM". */
void        jr_format_clock     (gint64 t, char *buf, gsize len);
