/* active_time: see active_time.h. */
#include "active_time.h"

void
jr_active_timer_init (JrActiveTimer *t, JrFlushFunc flush, gpointer user)
{
  *t = (JrActiveTimer){ 0 };
  t->flush = flush;
  t->user = user;
}

void
jr_active_timer_keystroke (JrActiveTimer *t, gint64 now)
{
  t->last_keystroke = now;
}

void
jr_active_timer_flush (JrActiveTimer *t)
{
  t->ticks_since_flush = 0;
  if (t->pending <= 0)
    return;
  gint64 seconds = t->pending;
  t->pending = 0; /* clear first so a re-entrant flush cannot double count */
  if (t->flush != NULL)
    t->flush (t->pending_day, seconds, t->user);
}

void
jr_active_timer_tick (JrActiveTimer *t, gint64 now)
{
  gboolean active = t->last_keystroke > 0 &&
                    now >= t->last_keystroke &&
                    now - t->last_keystroke < JR_IDLE_LIMIT_SECS;
  if (active)
    {
      char day[JR_ISO_LEN];
      jr_day_to_iso (jr_day_from_time (now), day);
      /* Crossing midnight: earlier seconds belong to the earlier day. */
      if (t->pending > 0 && g_strcmp0 (day, t->pending_day) != 0)
        jr_active_timer_flush (t);
      g_strlcpy (t->pending_day, day, sizeof t->pending_day);
      t->pending++;
      t->session++;
    }

  if (++t->ticks_since_flush >= JR_FLUSH_EVERY_SECS)
    jr_active_timer_flush (t);
}

void
jr_active_timer_stop (JrActiveTimer *t)
{
  jr_active_timer_flush (t);
  t->last_keystroke = 0;
  t->session = 0;
}

gint64
jr_active_timer_session_seconds (const JrActiveTimer *t)
{
  return t->session;
}

gint64
jr_active_timer_pending_seconds (const JrActiveTimer *t)
{
  return t->pending;
}

gint64
jr_active_timer_pending_for (const JrActiveTimer *t, const char *day_iso)
{
  return (t->pending > 0 && g_strcmp0 (t->pending_day, day_iso) == 0) ? t->pending : 0;
}
