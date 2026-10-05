/* active_time: counts seconds actually spent writing.
 *
 * Call jr_active_timer_tick() once per second and
 * jr_active_timer_keystroke() on every edit. A second counts only if a
 * keystroke happened in the last 60 seconds. Counted seconds are handed to
 * the flush callback every 15 seconds, when the day changes, and on
 * jr_active_timer_flush()/jr_active_timer_stop(). Time is injected so the
 * logic is testable without waiting.
 */
#pragma once

#include <glib.h>
#include "jdate.h"

#define JR_IDLE_LIMIT_SECS 60
#define JR_FLUSH_EVERY_SECS 15

typedef void (*JrFlushFunc) (const char *day_iso, gint64 seconds, gpointer user);

typedef struct {
  gint64      last_keystroke;            /* unix seconds, 0 = none yet */
  gint64      pending;                   /* counted, not yet flushed */
  char        pending_day[JR_ISO_LEN];   /* day `pending` belongs to */
  gint64      session;                   /* counted since init/stop */
  int         ticks_since_flush;
  JrFlushFunc flush;
  gpointer    user;
} JrActiveTimer;

void   jr_active_timer_init            (JrActiveTimer *t, JrFlushFunc flush, gpointer user);
void   jr_active_timer_keystroke       (JrActiveTimer *t, gint64 now);
void   jr_active_timer_tick            (JrActiveTimer *t, gint64 now);
void   jr_active_timer_flush           (JrActiveTimer *t);
/* Flushes and forgets activity and session time (used on lock). */
void   jr_active_timer_stop            (JrActiveTimer *t);
gint64 jr_active_timer_session_seconds (const JrActiveTimer *t);
gint64 jr_active_timer_pending_seconds (const JrActiveTimer *t);
/* Counted but unflushed seconds that belong to `day_iso`. */
gint64 jr_active_timer_pending_for     (const JrActiveTimer *t, const char *day_iso);
