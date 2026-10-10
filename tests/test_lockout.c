/* Tests for the wrong-try rules (src/core/lockout.c), LeetCode style
 * (see cases.h). Each case is a script of events and the state expected
 * after it:
 *
 *   w+N  a wrong passphrase, master passphrase or recovery key at T0+N s
 *   r    a right one
 *   p    a wrong PIN
 *   P    the right PIN, or a new PIN was set
 *   save the state is saved and read back (an app restart)
 *
 * Rules: 5 wrong passphrases or keys start a 30 s wait (tries during it
 * do not count); 5 wrong PINs block the PIN until a new one is set, and
 * no amount of waiting brings it back. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "lockout.h"

#define T0 G_GINT64_CONSTANT (1791316800)

typedef struct {
  gint64 remaining;
  int    tries_left;
  int    pin_left;
  gboolean blocked;
} State;

static gboolean
replay (const char *script, JrLockout *l)
{
  *l = (JrLockout){ 0 };
  g_auto (GStrv) events = g_strsplit (script, " ", 0);
  for (char **e = events; *e != NULL; e++)
    {
      if (**e == '\0')
        continue;
      if (g_str_has_prefix (*e, "w+"))
        jr_lockout_fail (l, T0 + g_ascii_strtoll (*e + 2, NULL, 10));
      else if (strcmp (*e, "r") == 0)
        jr_lockout_success (l);
      else if (strcmp (*e, "p") == 0)
        jr_lockout_pin_fail (l);
      else if (strcmp (*e, "P") == 0)
        jr_lockout_pin_success (l);
      else if (strcmp (*e, "save") == 0)
        {
          char buf[JR_LOCKOUT_STR_LEN];
          jr_lockout_to_string (l, buf, sizeof buf);
          if (!jr_lockout_from_string (buf, l))
            return FALSE;
        }
      else
        g_error ("bad event %s", *e);
    }
  return TRUE;
}

static State
state_at (const JrLockout *l, gint64 at)
{
  return (State){ jr_lockout_remaining (l, T0 + at), jr_lockout_tries_left (l),
                  jr_lockout_pin_tries_left (l), jr_lockout_pin_blocked (l) };
}

static gboolean
same_state (State a, State b)
{
  return a.remaining == b.remaining && a.tries_left == b.tries_left && a.pin_left == b.pin_left &&
         a.blocked == b.blocked;
}

#define SHOW_STATE "wait %" G_GINT64_FORMAT " s, %d tries, %d PIN tries%s"
#define STATE_ARGS(s) (s).remaining, (s).tries_left, (s).pin_left, (s).blocked ? ", PIN blocked" : ""

#define W5 "w+0 w+0 w+0 w+0 w+0"
#define P5 "p p p p p"

/* Constraints: events in time order (except where the clock moves back);
 * the state is read at T0 + `at` seconds. */
static void
test_rules (void)
{
  static const struct {
    const char *name;
    const char *script;
    gint64 at;
    State expected;
  } cases[] = {
    { "nothing yet", "", 0, { 0, 5, 5, FALSE } },
    /* Passphrases, master passphrases and recovery keys: a 30 s wait. */
    { "four wrong: no wait", "w+0 w+1 w+2 w+3", 4, { 0, 1, 5, FALSE } },
    { "fifth wrong: 30 s wait", W5, 0, { 30, 5, 5, FALSE } },
    { "a second before the wait ends", W5, 29, { 1, 5, 5, FALSE } },
    { "the wait is over", W5, 30, { 0, 5, 5, FALSE } },
    { "tries while waiting are ignored", W5 " w+10 w+10", 10, { 20, 5, 5, FALSE } },
    { "a fresh five after the wait", W5 " w+30 w+31 w+32 w+33", 34, { 0, 1, 5, FALSE } },
    { "the second wait", W5 " w+30 w+31 w+32 w+33 w+34", 35, { 29, 5, 5, FALSE } },
    { "a right one resets the count", "w+0 w+1 r", 2, { 0, 5, 5, FALSE } },
    { "a right one ends the wait", W5 " r", 1, { 0, 5, 5, FALSE } },
    { "clock moved back an hour: still 30 s", W5, -3600, { 30, 5, 5, FALSE } },
    { "a restart keeps the wait", W5 " save", 5, { 25, 5, 5, FALSE } },
    { "a restart keeps the count", "w+0 w+0 w+0 save", 1, { 0, 2, 5, FALSE } },
    /* PINs: blocked after 5, no timer. */
    { "four wrong PINs", "p p p p", 0, { 0, 5, 1, FALSE } },
    { "fifth wrong PIN blocks it", P5, 0, { 0, 5, 0, TRUE } },
    { "blocked a year later", P5, 365 * 86400, { 0, 5, 0, TRUE } },
    { "more PINs after the block change nothing", P5 " p p", 0, { 0, 5, 0, TRUE } },
    { "wrong PINs never start the wait", P5, 0, { 0, 5, 0, TRUE } },
    { "the right PIN resets the count", "p p p P", 0, { 0, 5, 5, FALSE } },
    { "a new PIN ends the block", P5 " P", 0, { 0, 5, 5, FALSE } },
    { "a right passphrase does not end the block", P5 " r", 0, { 0, 5, 0, TRUE } },
    { "a restart keeps the block", P5 " save", 0, { 0, 5, 0, TRUE } },
    { "a restart keeps the PIN count", "p p save", 0, { 0, 5, 3, FALSE } },
    /* The two counts are separate. */
    { "wrong passphrases leave PIN tries alone", "w+0 w+0 w+0", 0, { 0, 2, 5, FALSE } },
    { "wrong PINs leave the other tries alone", "p p p w+0", 0, { 0, 4, 2, FALSE } },
    { "the right PIN also ends the wait", W5 " P", 1, { 0, 5, 5, FALSE } },
    { "blocked PIN and a wait at once", P5 " " W5, 10, { 20, 5, 0, TRUE } },
  };
  JrCases c = { .table = "rules" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      JrLockout l;
      gboolean ok = replay (cases[i].script, &l);
      State got = state_at (&l, cases[i].at);
      jr_case (&c, cases[i].name, ok && same_state (got, cases[i].expected),
               "Input \"%s\" at +%" G_GINT64_FORMAT " s  Expected " SHOW_STATE "  Got " SHOW_STATE "%s",
               cases[i].script, cases[i].at, STATE_ARGS (cases[i].expected), STATE_ARGS (got),
               ok ? "" : " (did not read back)");
    }
  jr_cases_done (&c);
}

/* Constraints: what was saved by jr_lockout_to_string, by an older
 * version ("failed:until"), or anything else (refused). */
static void
test_saved_form (void)
{
  static const struct {
    const char *name;
    const char *input;
    gboolean ok;
    int failed;
    gint64 until;
    int pin_failed;
  } cases[] = {
    { "nothing", "0:0:0", TRUE, 0, 0, 0 },
    { "waiting", "0:1791316830:0", TRUE, 0, 1791316830, 0 },
    { "four wrong", "4:0:0", TRUE, 4, 0, 0 },
    { "PIN blocked", "0:0:5", TRUE, 0, 0, 5 },
    { "older version, no PIN count", "3:0", TRUE, 3, 0, 0 },
    { "older version, waiting", "0:1791316830", TRUE, 0, 1791316830, 0 },
    { "five wrong is never saved", "5:0:0", FALSE, 0, 0, 0 },
    { "six wrong PINs", "0:0:6", FALSE, 0, 0, 0 },
    { "negative count", "-1:0:0", FALSE, 0, 0, 0 },
    { "negative time", "0:-1:0", FALSE, 0, 0, 0 },
    { "negative PIN count", "0:0:-1", FALSE, 0, 0, 0 },
    { "count overflows", "99999999999999999999:0:0", FALSE, 0, 0, 0 },
    { "time overflows", "0:99999999999999999999:0", FALSE, 0, 0, 0 },
    { "four parts", "0:0:0:0", FALSE, 0, 0, 0 },
    { "one part", "0", FALSE, 0, 0, 0 },
    { "empty part", "0::0", FALSE, 0, 0, 0 },
    { "space before", " 0:0:0", FALSE, 0, 0, 0 },
    { "space after", "0:0:0 ", FALSE, 0, 0, 0 },
    { "plus sign", "+1:0:0", FALSE, 0, 0, 0 },
    { "hex", "0x1:0:0", FALSE, 0, 0, 0 },
    { "letters", "garbage", FALSE, 0, 0, 0 },
    { "empty", "", FALSE, 0, 0, 0 },
    { "NULL", NULL, FALSE, 0, 0, 0 },
  };
  JrCases c = { .table = "saved-form" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      JrLockout got = { 7, 7, 7 };
      gboolean ok = jr_lockout_from_string (cases[i].input, &got);
      gboolean match = ok == cases[i].ok &&
                       (!ok || (got.failed == cases[i].failed && got.until == cases[i].until &&
                                got.pin_failed == cases[i].pin_failed));
      jr_case (&c, cases[i].name, match,
               "Input \"%s\"  Expected %s %d:%" G_GINT64_FORMAT ":%d  Got %s %d:%" G_GINT64_FORMAT ":%d",
               cases[i].input ? cases[i].input : "(null)", cases[i].ok ? "read" : "refused",
               cases[i].failed, cases[i].until, cases[i].pin_failed, ok ? "read" : "refused",
               got.failed, got.until, got.pin_failed);
      if (ok)
        {
          char buf[JR_LOCKOUT_STR_LEN];
          JrLockout back = { 0 };
          jr_lockout_to_string (&got, buf, sizeof buf);
          jr_case (&c, cases[i].name,
                   jr_lockout_from_string (buf, &back) && back.failed == got.failed &&
                       back.until == got.until && back.pin_failed == got.pin_failed,
                   "Input \"%s\", saved again as \"%s\"  Expected the same state  Got a different one",
                   cases[i].input, buf);
        }
    }
  jr_cases_done (&c);
}

/* The reference: the plain rules, replayed from the start for every
 * question, with no saved state to get wrong. */
static State
reference (const char *const *events, int n, gint64 at)
{
  int failed = 0, pins = 0;
  gint64 until = 0;
  for (int i = 0; i < n; i++)
    {
      const char *e = events[i];
      if (g_str_has_prefix (e, "w+"))
        {
          gint64 t = T0 + g_ascii_strtoll (e + 2, NULL, 10);
          if (t >= until && ++failed == 5)
            {
              failed = 0;
              until = t + 30;
            }
        }
      else if (strcmp (e, "r") == 0)
        failed = 0, until = 0;
      else if (strcmp (e, "P") == 0)
        failed = 0, until = 0, pins = 0;
      else if (strcmp (e, "p") == 0 && pins < 5)
        pins++;
    }
  gint64 left = MAX (0, MIN (until - (T0 + at), 30));
  return (State){ left, 5 - failed, 5 - pins, pins >= 5 };
}

/* Hidden test: random scripts (time only moves forward), checked against
 * the reference after every event, once straight through and once with
 * a restart after every event. */
static void
test_hidden_random_scripts (void)
{
  JrCases c = { .table = "hidden: random scripts" };
  static const char *const kinds[] = { "w", "w", "w", "r", "p", "p", "P" };
  for (int n = 0; n < 5000; n++)
    {
      int len = g_test_rand_int_range (1, 40);
      g_autoptr (GPtrArray) events = g_ptr_array_new_with_free_func (g_free);
      GString *script = g_string_new (NULL);
      JrLockout l = { 0 }, saved = { 0 };
      gint64 t = 0;
      for (int k = 0; k < len; k++)
        {
          const char *kind = kinds[g_test_rand_int_range (0, G_N_ELEMENTS (kinds))];
          t += g_test_rand_int_range (0, 12);
          char *e = *kind == 'w' ? g_strdup_printf ("w+%" G_GINT64_FORMAT, t) : g_strdup (kind);
          g_ptr_array_add (events, e);
          g_string_append_printf (script, "%s%s", k ? " " : "", e);

          JrLockout *both[2] = { &l, &saved };
          for (int s = 0; s < 2; s++)
            {
              if (*kind == 'w')
                jr_lockout_fail (both[s], T0 + t);
              else if (*kind == 'r')
                jr_lockout_success (both[s]);
              else if (*kind == 'p')
                jr_lockout_pin_fail (both[s]);
              else
                jr_lockout_pin_success (both[s]);
            }
          char buf[JR_LOCKOUT_STR_LEN];
          jr_lockout_to_string (&saved, buf, sizeof buf);
          gboolean read = jr_lockout_from_string (buf, &saved);

          gint64 at = t + g_test_rand_int_range (0, 40);
          State want = reference ((const char *const *) events->pdata, k + 1, at);
          State got = state_at (&l, at), got_saved = state_at (&saved, at);
          jr_case (&c, "straight through", same_state (got, want),
                   "Input \"%s\" at +%" G_GINT64_FORMAT " s  Expected " SHOW_STATE "  Got " SHOW_STATE,
                   script->str, at, STATE_ARGS (want), STATE_ARGS (got));
          jr_case (&c, "restart after every event", read && same_state (got_saved, want),
                   "Input \"%s\" at +%" G_GINT64_FORMAT " s  Expected " SHOW_STATE "  Got " SHOW_STATE,
                   script->str, at, STATE_ARGS (want), STATE_ARGS (got_saved));
        }
      g_string_free (script, TRUE);
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/lockout/rules", test_rules);
  g_test_add_func ("/lockout/saved-form", test_saved_form);
  g_test_add_func ("/lockout/hidden/random-scripts", test_hidden_random_scripts);
  return g_test_run ();
}
