/* Tests for the study stopwatch and timer (src/notes/core/studyclock.c),
 * LeetCode style (see cases.h).
 *
 * Input: a script of steps, each at a time in seconds on a monotonic
 * clock (any order the person can produce: start twice, pause while
 * stopped, the clock seen "going back"). A stopwatch counts up without
 * limit; a timer counts down from 1 s to 10 h and stops itself at zero.
 * Study time is handed over in whole seconds and never twice. */
#include <glib.h>
#include "cases.h"
#include "studyclock.h"

#define S(x) ((gint64) ((x) * G_USEC_PER_SEC))

typedef enum { START, PAUSE, RESET, TIMER, SHOWN, TAKE, DONE, RUNNING, END } Op;

typedef struct {
  Op     op;
  double at;   /* seconds */
  gint64 want; /* SHOWN: seconds shown; TAKE: seconds handed over; DONE/RUNNING/START: 0 or 1; TIMER: length */
} Step;

typedef struct {
  const char *name;
  JnClockMode mode;
  Step        steps[12];
} ClockCase;

static const ClockCase cases[] = {
  { "stopwatch counts up",    JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { SHOWN, 90, 90 }, { RUNNING, 90, 1 }, { END, 0, 0 } } },
  { "pause holds",            JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { PAUSE, 30, 0 }, { SHOWN, 100, 30 },
                                                    { RUNNING, 100, 0 }, { END, 0, 0 } } },
  { "resume adds on",         JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { PAUSE, 30, 0 }, { START, 100, 1 },
                                                    { SHOWN, 110, 40 }, { END, 0, 0 } } },
  { "reset goes to zero",     JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { RESET, 50, 0 }, { SHOWN, 60, 0 },
                                                    { RUNNING, 60, 0 }, { END, 0, 0 } } },
  { "start twice is one run", JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { START, 50, 1 }, { SHOWN, 60, 60 }, { END, 0, 0 } } },
  { "pause while stopped",    JN_CLOCK_STOPWATCH, { { PAUSE, 10, 0 }, { SHOWN, 20, 0 }, { RUNNING, 20, 0 }, { END, 0, 0 } } },
  { "clock seen going back",  JN_CLOCK_STOPWATCH, { { START, 100, 1 }, { SHOWN, 50, 0 }, { PAUSE, 40, 0 },
                                                    { SHOWN, 200, 0 }, { END, 0, 0 } } },
  { "hours",                  JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { SHOWN, 3723.9, 3723 }, { END, 0, 0 } } },
  { "a stopwatch never finishes", JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { DONE, 1e6, 0 }, { RUNNING, 1e6, 1 }, { END, 0, 0 } } },
  { "timer counts down",      JN_CLOCK_TIMER,     { { TIMER, 0, 60 }, { SHOWN, 0, 60 }, { START, 0, 1 },
                                                    { SHOWN, 0.5, 60 }, { SHOWN, 59.2, 1 }, { SHOWN, 60, 0 }, { END, 0, 0 } } },
  { "timer finishes once",    JN_CLOCK_TIMER,     { { TIMER, 0, 60 }, { START, 0, 1 }, { DONE, 59.9, 0 },
                                                    { DONE, 61, 1 }, { DONE, 62, 0 }, { RUNNING, 62, 0 },
                                                    { SHOWN, 500, 0 }, { START, 500, 0 }, { END, 0, 0 } } },
  { "timer pauses",           JN_CLOCK_TIMER,     { { TIMER, 0, 60 }, { START, 0, 1 }, { PAUSE, 20, 0 },
                                                    { SHOWN, 1000, 40 }, { START, 1000, 1 }, { SHOWN, 1030, 10 }, { END, 0, 0 } } },
  { "reset after the end",    JN_CLOCK_TIMER,     { { TIMER, 0, 60 }, { START, 0, 1 }, { DONE, 70, 1 }, { RESET, 80, 0 },
                                                    { SHOWN, 80, 60 }, { START, 80, 1 }, { SHOWN, 90, 50 }, { END, 0, 0 } } },
  { "too short a timer",      JN_CLOCK_TIMER,     { { TIMER, 0, 0 }, { SHOWN, 0, 1 }, { END, 0, 0 } } },
  { "too long a timer",       JN_CLOCK_TIMER,     { { TIMER, 0, 999999 }, { SHOWN, 0, 36000 }, { END, 0, 0 } } },
  { "new length while running", JN_CLOCK_TIMER,   { { TIMER, 0, 60 }, { START, 0, 1 }, { TIMER, 10, 120 },
                                                    { RUNNING, 10, 0 }, { SHOWN, 50, 120 }, { END, 0, 0 } } },
  { "seconds handed over once", JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { TAKE, 2.5, 2 }, { TAKE, 3.4, 1 }, { TAKE, 3.9, 0 },
                                                      { PAUSE, 10, 0 }, { TAKE, 10, 7 }, { TAKE, 99, 0 }, { END, 0, 0 } } },
  { "a timer hands over its length at most", JN_CLOCK_TIMER, { { TIMER, 0, 60 }, { START, 0, 1 }, { TAKE, 1000, 60 },
                                                               { TAKE, 2000, 0 }, { END, 0, 0 } } },
  { "reset forgets the rest", JN_CLOCK_STOPWATCH, { { START, 0, 1 }, { TAKE, 5, 5 }, { RESET, 8, 0 }, { TAKE, 9, 0 },
                                                    { START, 10, 1 }, { TAKE, 12, 2 }, { END, 0, 0 } } },
  { "nothing run, nothing handed over", JN_CLOCK_STOPWATCH, { { TAKE, 100, 0 }, { END, 0, 0 } } },
};

static void
test_table (void)
{
  JrCases c = { "studyclock", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      const ClockCase *k = &cases[i];
      JnClock clock;
      jn_clock_init (&clock, k->mode);
      for (const Step *s = k->steps; s->op != END; s++)
        {
          gint64 now = S (s->at), got = 0;
          gboolean check = TRUE;
          switch (s->op)
            {
            case START:   got = jn_clock_start (&clock, now); break;
            case PAUSE:   jn_clock_pause (&clock, now); check = FALSE; break;
            case RESET:   jn_clock_reset (&clock); check = FALSE; break;
            case TIMER:   jn_clock_set_timer (&clock, s->want); check = FALSE; break;
            case SHOWN:   got = jn_clock_shown (&clock, now); break;
            case TAKE:    got = jn_clock_take_unlogged (&clock, now); break;
            case DONE:    got = jn_clock_check_finished (&clock, now); break;
            case RUNNING: got = clock.running; break;
            case END:     break;
            }
          if (check)
            jr_case (&c, k->name, got == s->want, "Step %d at %.1f s  Expected %" G_GINT64_FORMAT "  Got %" G_GINT64_FORMAT,
                     (int) (s - k->steps), s->at, s->want, got);
        }
    }
  jr_cases_done (&c);
}

typedef struct {
  gint64      seconds;
  const char *clock, *studied;
} FormatCase;

static const FormatCase formats[] = {
  { 0,      "0:00",    "no time" },
  { 5,      "0:05",    "under a minute" },
  { 59,     "0:59",    "under a minute" },
  { 60,     "1:00",    "1 min" },
  { 245,    "4:05",    "4 min" },
  { 3599,   "59:59",   "59 min" },
  { 3600,   "1:00:00", "1 h" },
  { 3723,   "1:02:03", "1 h 2 min" },
  { 11400,  "3:10:00", "3 h 10 min" },
  { 360000, "100:00:00", "100 h" },
  { -5,     "0:00",    "no time" },
};

static void
test_format (void)
{
  JrCases c = { "format", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (formats); i++)
    {
      g_autofree char *clock = jn_clock_format (formats[i].seconds);
      g_autofree char *studied = jn_format_studied (formats[i].seconds);
      jr_case (&c, formats[i].clock, g_strcmp0 (clock, formats[i].clock) == 0 &&
               g_strcmp0 (studied, formats[i].studied) == 0, "Input %" G_GINT64_FORMAT "  Expected %s, %s  Got %s, %s",
               formats[i].seconds, formats[i].clock, formats[i].studied, clock, studied);
    }
  jr_cases_done (&c);
}

/* Hidden: random scripts against an obviously right model that adds up
 * the running intervals. Elapsed matches the model, a timer stays within
 * its length, and the seconds handed over add up to exactly the whole
 * seconds run since the last reset. */
static void
test_hidden (void)
{
  JrCases c = { "studyclock (hidden)", 0, 0 };
  for (int round = 0; round < 3000; round++)
    {
      gboolean timer = g_test_rand_bit ();
      gint64 length = S (g_test_rand_int_range (1, 120));
      JnClock clock;
      jn_clock_init (&clock, timer ? JN_CLOCK_TIMER : JN_CLOCK_STOPWATCH);
      if (timer)
        jn_clock_set_timer (&clock, length / G_USEC_PER_SEC);
      gint64 now = 0, run = 0, since = -1, taken = 0; /* the model */
      gboolean ok = TRUE;
      for (int step = 0; step < 40 && ok; step++)
        {
          now += g_test_rand_int_range (0, 20 * G_USEC_PER_SEC);
          gint64 model = run + (since >= 0 ? now - since : 0);
          if (timer && model > length)
            model = length;
          ok &= jn_clock_elapsed (&clock, now) == model;
          switch (g_test_rand_int_range (0, 4))
            {
            case 0:
              if (jn_clock_start (&clock, now) && since < 0)
                since = now;
              break;
            case 1:
              jn_clock_pause (&clock, now);
              run = model;
              since = -1;
              break;
            case 2:
              taken += jn_clock_take_unlogged (&clock, now);
              ok &= taken == model / G_USEC_PER_SEC;
              break;
            default:
              if (jn_clock_check_finished (&clock, now))
                {
                  run = length;
                  since = -1;
                }
              break;
            }
          if (timer && since >= 0 && run + now - since >= length)
            ok &= jn_clock_is_finished (&clock, now);
        }
      jr_case (&c, timer ? "timer" : "stopwatch", ok, "Seed round %d failed", round);
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/studyclock/table", test_table);
  g_test_add_func ("/studyclock/format", test_format);
  g_test_add_func ("/studyclock/hidden", test_hidden);
  return g_test_run ();
}
