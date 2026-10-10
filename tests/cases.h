/* LeetCode-style table tests.
 *
 * A test is a table of cases: a name, an input and the expected output,
 * with the input's constraints written above the table. Every case runs,
 * even after one fails, and each failure is printed as
 *
 *   Case "not a leap year": Input "2026-02-29"  Expected invalid  Got 2026-03-01
 *
 * Hidden tests check many generated inputs against a slow reference that
 * is obviously right. They use g_test_rand_*, so a failing run prints a
 * seed and `--seed` replays it exactly. */
#pragma once

#include <stdarg.h>
#include <glib.h>

typedef struct {
  const char *table;
  int run;
  int failed;
} JrCases;

/* Records one case; on failure prints `Case "name": <details>`. */
G_GNUC_PRINTF (4, 5) static inline void
jr_case (JrCases *c, const char *name, gboolean ok, const char *details, ...)
{
  c->run++;
  if (ok)
    return;
  c->failed++;
  va_list ap;
  va_start (ap, details);
  g_autofree char *msg = g_strdup_vprintf (details, ap);
  va_end (ap);
  if (c->failed <= 20) /* a hidden test can fail thousands of times */
    g_printerr ("%s: Case \"%s\": %s\n", c->table, name, msg);
  else if (c->failed == 21)
    g_printerr ("%s: more failing cases not shown\n", c->table);
  g_test_fail ();
}

/* Ends a table: "parse: 41/41 cases passed". */
static inline void
jr_cases_done (const JrCases *c)
{
  g_test_message ("%s: %d/%d cases passed", c->table, c->run - c->failed, c->run);
  if (c->failed > 0)
    g_printerr ("%s: %d/%d cases passed\n", c->table, c->run - c->failed, c->run);
  g_assert_cmpint (c->run, >, 0);
}
