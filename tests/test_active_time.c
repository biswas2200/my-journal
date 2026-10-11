/* Tests for the writing-time counter (src/core/active_time.c), LeetCode
 * style (see cases.h). Each case is a script of events, run from a start
 * time, and what the counter must have saved afterwards:
 *
 *   k    a keystroke now
 *   tN   N ticks, one per second (the clock moves N seconds); t = t1
 *   +N   the clock jumps N seconds with no ticks (laptop asleep)
 *   -N   the clock moves back N seconds
 *   f    save now (lock, quit, day switch)
 *   s    stop (lock): save, forget the keystroke and the session
 *
 * Rules: a tick counts if a keystroke happened in the 60 seconds up to it
 * (that second included); counted seconds are saved every 15 ticks and
 * when the day changes, and always to the day they were counted on. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include "active_time.h"
#include "cases.h"

#define T0 G_GINT64_CONSTANT (1791316800)       /* 2026-10-06 20:00:00 UTC */
#define MIDNIGHT (T0 + 4 * 3600)                 /* 2026-10-07 00:00:00 UTC */
#define IST_MIDNIGHT (MIDNIGHT - 19800)          /* 2026-10-07 00:00 in India */
#define DST_ENDS G_GINT64_CONSTANT (1793512800)  /* 2026-11-01 02:00 EDT -> 01:00 EST */
#define NEW_YORK "EST5EDT,M3.2.0,M11.1.0"

static void
set_tz (const char *tz)
{
  setenv ("TZ", tz, 1);
  tzset ();
}

/* Each save, as "DD:seconds" (day of the month). */
static void
log_save (const char *day, gint64 seconds, gpointer user)
{
  GString *log = user;
  g_string_append_printf (log, "%s%s:%" G_GINT64_FORMAT, log->len > 0 ? " " : "", day + 8, seconds);
}

/* Runs `script` from `start`; returns the clock afterwards. */
static gint64
replay (JrActiveTimer *t, const char *script, gint64 start)
{
  gint64 now = start;
  g_auto (GStrv) events = g_strsplit (script, " ", 0);
  for (char **e = events; *e != NULL; e++)
    {
      const char *ev = *e;
      if (*ev == '\0')
        continue;
      if (strcmp (ev, "k") == 0)
        jr_active_timer_keystroke (t, now);
      else if (ev[0] == 't')
        for (gint64 n = ev[1] ? g_ascii_strtoll (ev + 1, NULL, 10) : 1; n > 0; n--)
          jr_active_timer_tick (t, now++);
      else if (ev[0] == '+' || ev[0] == '-')
        now += g_ascii_strtoll (ev, NULL, 10);
      else if (strcmp (ev, "f") == 0)
        jr_active_timer_flush (t);
      else if (strcmp (ev, "s") == 0)
        jr_active_timer_stop (t);
      else
        {
          g_printerr ("bad event %s\n", ev);
          g_assert_not_reached ();
        }
    }
  return now;
}

/* Unsaved seconds as "DD:seconds", or "" if none; "?" if the counter
 * does not report them for their own day. */
static void
show_pending (const JrActiveTimer *t, char *buf, gsize len)
{
  gint64 p = jr_active_timer_pending_seconds (t);
  if (p == 0)
    g_strlcpy (buf, "", len);
  else if (jr_active_timer_pending_for (t, t->pending_day) != p)
    g_strlcpy (buf, "?", len);
  else
    g_snprintf (buf, len, "%s:%" G_GINT64_FORMAT, t->pending_day + 8, p);
}

/* Constraints: ticks come once a second; the clock is a real date in
 * 2026; tz is the user's zone (UTC if not given). */
static void
test_rules (void)
{
  static const struct {
    const char *name;
    const char *tz;
    gint64 start;
    const char *script;
    const char *saved;   /* every save, in order */
    const char *pending; /* counted, not saved yet */
    gint64 session;
  } cases[] = {
    /* Counting. */
    { "nothing typed", NULL, T0, "t20 f", "", "", 0 },
    { "a keystroke but no ticks", NULL, T0, "k f", "", "", 0 },
    { "one tick after a keystroke", NULL, T0, "k t", "", "06:1", 1 },
    { "two keystrokes in a second count once", NULL, T0, "k k t", "", "06:1", 1 },
    { "the 60th second after a keystroke counts", NULL, T0, "k +59 t", "", "06:1", 1 },
    { "the 61st does not", NULL, T0, "k +60 t", "", "", 0 },
    { "one keystroke: 60 s, then it pauses", NULL, T0, "k t75 f", "06:15 06:15 06:15 06:15", "", 60 },
    { "typing again within 60 s keeps it going", NULL, T0, "k +50 k +50 t20", "06:10", "", 10 },
    { "a long pause stops it; typing restarts it", NULL, T0, "k t5 +100 t5 k t5 f", "06:10", "", 10 },
    /* Saving every 15 seconds. */
    { "nothing saved before 15 ticks", NULL, T0, "k t14", "", "06:14", 14 },
    { "the 15th tick saves", NULL, T0, "k t15", "06:15", "", 15 },
    { "idle ticks count towards the 15 too", NULL, T0, "t10 k t5", "06:5", "", 5 },
    { "a save restarts the 15", NULL, T0, "k t10 f t14", "06:10", "06:14", 24 },
    { "and the 15th after it saves", NULL, T0, "k t10 f t15", "06:10 06:15", "", 25 },
    { "saving with nothing counted calls nothing", NULL, T0, "f f t3 f", "", "", 0 },
    /* Midnight. */
    { "midnight splits the time", NULL, MIDNIGHT - 5, "k t10 f", "06:5 07:5", "", 10 },
    { "the tick at 00:00 belongs to the new day", NULL, MIDNIGHT - 1, "k t2 f", "06:1 07:1", "", 2 },
    { "unsaved seconds after midnight are the new day's", NULL, MIDNIGHT - 3, "k t7", "06:3", "07:4", 7 },
    { "India: midnight at 18:30 UTC", "IST-5:30", IST_MIDNIGHT - 2, "k t4 f", "06:2 07:2", "", 4 },
    { "summer time ending does not split the day", NEW_YORK, DST_ENDS - 5, "k t10 f", "01:10", "", 10 },
    /* Sleep and clock changes. */
    { "asleep an hour: none of it counts", NULL, T0, "k t5 +3600 t10 f", "06:5", "", 5 },
    { "asleep 30 s: the keystroke covers the rest of its minute", NULL, T0, "k t5 +30 t30 f",
      "06:15 06:15", "", 30 },
    { "clock moved back an hour: waits for the clock", NULL, T0, "k t5 -3600 t10 f", "06:5", "", 5 },
    { "clock moved back: a new keystroke counts at once", NULL, T0, "k t5 -3600 k t5 f", "06:10", "", 10 },
    { "clock moved back over midnight", NULL, MIDNIGHT, "k t3 -10 k t3 f", "07:3 06:3", "", 6 },
    /* Locking. */
    { "locking saves and clears the session", NULL, T0, "k t10 s", "06:10", "", 0 },
    { "after locking the old keystroke is forgotten", NULL, T0, "k t10 s t30 f", "06:10", "", 0 },
    { "typing after unlocking counts again", NULL, T0, "k t10 s k t5 f", "06:10 06:5", "", 5 },
    { "locking with nothing counted saves nothing", NULL, T0, "s", "", "", 0 },
  };
  JrCases c = { .table = "rules" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      set_tz (cases[i].tz ? cases[i].tz : "UTC0");
      GString *saved = g_string_new (NULL);
      JrActiveTimer t;
      jr_active_timer_init (&t, log_save, saved);
      replay (&t, cases[i].script, cases[i].start);
      char pending[32];
      show_pending (&t, pending, sizeof pending);
      gint64 session = jr_active_timer_session_seconds (&t);
      jr_case (&c, cases[i].name,
               strcmp (saved->str, cases[i].saved) == 0 && strcmp (pending, cases[i].pending) == 0 &&
                 session == cases[i].session,
               "Input \"%s\"  Expected saved [%s] unsaved [%s] session %" G_GINT64_FORMAT
               "  Got saved [%s] unsaved [%s] session %" G_GINT64_FORMAT,
               cases[i].script, cases[i].saved, cases[i].pending, cases[i].session, saved->str, pending,
               session);
      g_string_free (saved, TRUE);
    }
  set_tz ("UTC0");
  jr_cases_done (&c);
}

/* Saving from inside a save (the database write re-enters): each second
 * is still saved once. */
typedef struct {
  JrActiveTimer *timer;
  gint64 total;
  int calls;
} Reentry;

static void
save_and_save_again (const char *day, gint64 seconds, gpointer user)
{
  (void) day;
  Reentry *r = user;
  r->total += seconds;
  if (++r->calls < 50)
    jr_active_timer_flush (r->timer);
}

/* Constraints: the save callback may be NULL (seconds are dropped, not
 * kept forever), or may save again from inside itself. */
static void
test_callbacks (void)
{
  JrCases c = { .table = "callbacks" };

  JrActiveTimer t;
  jr_active_timer_init (&t, NULL, NULL);
  replay (&t, "k t20 f", T0);
  jr_case (&c, "no callback", jr_active_timer_pending_seconds (&t) == 0 &&
                                jr_active_timer_session_seconds (&t) == 20,
           "Input \"k t20 f\"  Expected unsaved 0 session 20  Got unsaved %" G_GINT64_FORMAT
           " session %" G_GINT64_FORMAT,
           jr_active_timer_pending_seconds (&t), jr_active_timer_session_seconds (&t));

  Reentry r = { &t, 0, 0 };
  jr_active_timer_init (&t, save_and_save_again, &r);
  replay (&t, "k t40 f", T0);
  jr_case (&c, "saving again from inside a save", r.total == 40 && r.calls == 3,
           "Input \"k t40 f\"  Expected 40 s in 3 saves  Got %" G_GINT64_FORMAT " s in %d saves",
           r.total, r.calls);
  jr_cases_done (&c);
}

/* ---- Hidden tests --------------------------------------------------- */

/* The reference: per-day totals keyed by ISO day, with days computed
 * from the zone's fixed offset and GLib's own calendar. */
static void
ref_day (gint64 now, gint64 offset, char out[JR_ISO_LEN])
{
  gint64 local = now + offset;
  gint64 days = local / 86400 - (local % 86400 < 0); /* floor */
  GDate d;
  g_date_clear (&d, 1);
  g_date_set_julian (&d, (guint32) (719163 + days)); /* 1970-01-01 */
  g_snprintf (out, JR_ISO_LEN, "%04d-%02d-%02d", g_date_get_year (&d), g_date_get_month (&d),
              g_date_get_day (&d));
}

static void
add (GHashTable *days, const char *day, gint64 seconds)
{
  gint64 *v = g_hash_table_lookup (days, day);
  if (v == NULL)
    {
      v = g_new0 (gint64, 1);
      g_hash_table_insert (days, g_strdup (day), v);
    }
  *v += seconds;
}

typedef struct {
  GHashTable *saved;
  int bad_saves; /* saves of 0, a negative count, or more than 15 s */
} Saves;

static void
record_save (const char *day, gint64 seconds, gpointer user)
{
  Saves *s = user;
  if (seconds < 1 || seconds > JR_FLUSH_EVERY_SECS)
    s->bad_saves++;
  add (s->saved, day, seconds);
}

/* Saved plus unsaved equals the reference for every day there is. */
static gboolean
totals_match (GHashTable *want, GHashTable *saved, const JrActiveTimer *t)
{
  GHashTableIter it;
  gpointer key, value;
  guint seen = 0;
  g_hash_table_iter_init (&it, want);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      gint64 *s = g_hash_table_lookup (saved, key);
      gint64 got = (s ? *s : 0) + jr_active_timer_pending_for (t, key);
      if (got != *(gint64 *) value)
        return FALSE;
      seen += s != NULL;
    }
  /* Nothing saved to a day the reference never counted. */
  return seen == g_hash_table_size (saved) &&
         (jr_active_timer_pending_seconds (t) == 0 || g_hash_table_contains (want, t->pending_day));
}

/* Hidden test: random scripts of keystrokes, runs of ticks, sleeps,
 * saves and locks in four fixed-offset zones, many starting just before
 * a local midnight. The reference counts a tick if any keystroke since
 * the last lock is in the 60 seconds up to it (the clock only moves
 * forward here). Checked after every event: the session, every day's
 * total, at most 14 s unsaved, and every save between 1 and 15 s. */
static void
test_hidden_scripts (void)
{
  static const struct {
    const char *tz;
    gint64 offset;
  } zones[] = { { "UTC0", 0 }, { "IST-5:30", 19800 }, { "HST10", -36000 }, { "<+14>-14", 50400 } };
  JrCases c = { .table = "hidden: random scripts" };
  GArray *keys = g_array_new (FALSE, FALSE, sizeof (gint64));
  for (gsize z = 0; z < G_N_ELEMENTS (zones); z++)
    {
      set_tz (zones[z].tz);
      for (int n = 0; n < 100; n++)
        {
          gint64 now = n % 2 ? g_test_rand_int_range (0, 365 * 86400) + G_GINT64_CONSTANT (1767225600)
                             : MIDNIGHT - zones[z].offset + g_test_rand_int_range (-30, 30) * 86400 -
                                 g_test_rand_int_range (0, 400);
          gint64 start = now;
          GHashTable *want = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
          Saves saves = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free), 0 };
          GString *script = g_string_new (NULL);
          gint64 session = 0;
          gboolean ok = TRUE;
          JrActiveTimer t;
          jr_active_timer_init (&t, record_save, &saves);
          g_array_set_size (keys, 0);
          for (int e = 0; e < 100 && ok; e++)
            {
              int r = g_test_rand_int_range (0, 100);
              if (r < 30)
                {
                  jr_active_timer_keystroke (&t, now);
                  g_array_append_val (keys, now);
                  g_string_append (script, " k");
                }
              else if (r < 80)
                {
                  int ticks = g_test_rand_int_range (1, 90);
                  for (int k = 0; k < ticks; k++, now++)
                    {
                      jr_active_timer_tick (&t, now);
                      gboolean counts = FALSE;
                      for (guint i = 0; i < keys->len; i++)
                        {
                          gint64 key = g_array_index (keys, gint64, i);
                          counts |= key <= now && now - key < 60;
                        }
                      if (counts)
                        {
                          char day[JR_ISO_LEN];
                          ref_day (now, zones[z].offset, day);
                          add (want, day, 1);
                          session++;
                        }
                    }
                  g_string_append_printf (script, " t%d", ticks);
                }
              else if (r < 88)
                {
                  int jump = g_test_rand_bit () ? g_test_rand_int_range (1, 120)
                                                : g_test_rand_int_range (3600, 90000);
                  now += jump;
                  g_string_append_printf (script, " +%d", jump);
                }
              else if (r < 96)
                {
                  jr_active_timer_flush (&t);
                  g_string_append (script, " f");
                }
              else
                {
                  jr_active_timer_stop (&t);
                  g_array_set_size (keys, 0);
                  session = 0;
                  g_string_append (script, " s");
                }
              ok = jr_active_timer_session_seconds (&t) == session &&
                   jr_active_timer_pending_seconds (&t) < JR_FLUSH_EVERY_SECS && saves.bad_saves == 0 &&
                   totals_match (want, saves.saved, &t);
            }
          jr_case (&c, zones[z].tz, ok, "Input \"%s\" from %" G_GINT64_FORMAT " in %s  Expected the "
                   "reference's totals  Got a mismatch after the last event", script->str + 1,
                   start, zones[z].tz);
          g_string_free (script, TRUE);
          g_hash_table_unref (want);
          g_hash_table_unref (saves.saved);
        }
    }
  g_array_unref (keys);
  set_tz ("UTC0");
  jr_cases_done (&c);
}

/* Huge input: three days of writing without a break (a keystroke every
 * 30 s), in India, from an hour before midnight. 259,200 ticks; each day
 * gets exactly its own seconds and no save is over 15 s. */
static void
test_three_day_marathon (void)
{
  JrCases c = { .table = "huge: three-day marathon" };
  set_tz ("IST-5:30");
  Saves saves = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free), 0 };
  JrActiveTimer t;
  jr_active_timer_init (&t, record_save, &saves);
  gint64 start = IST_MIDNIGHT - 3600;
  for (gint64 now = start; now < start + 3 * 86400; now++)
    {
      if ((now - start) % 30 == 0)
        jr_active_timer_keystroke (&t, now);
      jr_active_timer_tick (&t, now);
    }
  jr_active_timer_stop (&t);
  static const struct {
    const char *day;
    gint64 seconds;
  } want[] = { { "2026-10-06", 3600 }, { "2026-10-07", 86400 }, { "2026-10-08", 86400 },
               { "2026-10-09", 82800 } };
  for (gsize i = 0; i < G_N_ELEMENTS (want); i++)
    {
      gint64 *got = g_hash_table_lookup (saves.saved, want[i].day);
      jr_case (&c, want[i].day, got != NULL && *got == want[i].seconds,
               "Expected %" G_GINT64_FORMAT " s  Got %" G_GINT64_FORMAT " s", want[i].seconds,
               got ? *got : 0);
    }
  jr_case (&c, "only those days", g_hash_table_size (saves.saved) == G_N_ELEMENTS (want),
           "Expected %d days  Got %u", (int) G_N_ELEMENTS (want), g_hash_table_size (saves.saved));
  jr_case (&c, "every save 1-15 s", saves.bad_saves == 0, "Got %d bad saves", saves.bad_saves);
  g_hash_table_unref (saves.saved);
  set_tz ("UTC0");
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  set_tz ("UTC0");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/active-time/rules", test_rules);
  g_test_add_func ("/active-time/callbacks", test_callbacks);
  g_test_add_func ("/active-time/hidden/scripts", test_hidden_scripts);
  g_test_add_func ("/active-time/huge/three-day-marathon", test_three_day_marathon);
  return g_test_run ();
}
