/* studyclock: see studyclock.h. */
#include "studyclock.h"

void
jn_clock_init (JnClock *c, JnClockMode mode)
{
  *c = (JnClock) { 0 };
  c->mode = mode;
  if (mode == JN_CLOCK_TIMER)
    c->length = 25 * 60 * G_USEC_PER_SEC;
}

void
jn_clock_set_timer (JnClock *c, gint64 seconds)
{
  seconds = CLAMP (seconds, 1, (gint64) JN_TIMER_MAX_MINUTES * 60);
  jn_clock_init (c, JN_CLOCK_TIMER);
  c->length = seconds * G_USEC_PER_SEC;
}

gint64
jn_clock_elapsed (const JnClock *c, gint64 now)
{
  gint64 run = c->banked;
  if (c->running && now > c->started)
    run += now - c->started;
  if (c->mode == JN_CLOCK_TIMER && run > c->length)
    run = c->length;
  return run;
}

gboolean
jn_clock_is_finished (const JnClock *c, gint64 now)
{
  return c->mode == JN_CLOCK_TIMER && jn_clock_elapsed (c, now) >= c->length;
}

gboolean
jn_clock_start (JnClock *c, gint64 now)
{
  if (jn_clock_is_finished (c, now))
    return FALSE;
  if (!c->running)
    {
      c->running = TRUE;
      c->started = now;
    }
  return TRUE;
}

void
jn_clock_pause (JnClock *c, gint64 now)
{
  if (!c->running)
    return;
  c->banked = jn_clock_elapsed (c, now);
  c->running = FALSE;
}

void
jn_clock_reset (JnClock *c)
{
  gint64 length = c->length;
  jn_clock_init (c, c->mode);
  c->length = length;
}

gboolean
jn_clock_check_finished (JnClock *c, gint64 now)
{
  if (!c->running || !jn_clock_is_finished (c, now))
    return FALSE;
  jn_clock_pause (c, now);
  return TRUE;
}

gint64
jn_clock_shown (const JnClock *c, gint64 now)
{
  gint64 run = jn_clock_elapsed (c, now);
  if (c->mode == JN_CLOCK_STOPWATCH)
    return run / G_USEC_PER_SEC;
  return (c->length - run + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC; /* 0:00 only at the end */
}

gint64
jn_clock_take_unlogged (JnClock *c, gint64 now)
{
  gint64 seconds = (jn_clock_elapsed (c, now) - c->logged) / G_USEC_PER_SEC;
  if (seconds <= 0)
    return 0;
  c->logged += seconds * G_USEC_PER_SEC;
  return seconds;
}

char *
jn_clock_format (gint64 seconds)
{
  seconds = MAX (seconds, 0);
  gint64 h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
  if (h > 0)
    return g_strdup_printf ("%" G_GINT64_FORMAT ":%02d:%02d", h, (int) m, (int) s);
  return g_strdup_printf ("%d:%02d", (int) m, (int) s);
}

char *
jn_format_studied (gint64 seconds)
{
  if (seconds <= 0)
    return g_strdup ("no time");
  if (seconds < 60)
    return g_strdup ("under a minute");
  gint64 h = seconds / 3600, m = seconds / 60 % 60;
  if (h == 0)
    return g_strdup_printf ("%d min", (int) m);
  if (m == 0)
    return g_strdup_printf ("%" G_GINT64_FORMAT " h", h);
  return g_strdup_printf ("%" G_GINT64_FORMAT " h %d min", h, (int) m);
}
