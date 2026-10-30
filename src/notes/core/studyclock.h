/* studyclock: the stopwatch and the countdown timer used to track study
 * time, without any real clock inside.
 *
 * Every call takes `now`, a monotonic time in microseconds (the window
 * passes g_get_monotonic_time (), which stops while the laptop sleeps, so
 * a sleeping laptop is not counted as studying). A stopwatch counts up
 * with no limit. A timer counts down from its length and stops by itself
 * at zero; it then has to be reset (or given a new length) to run again.
 * A clock never counts backwards: a `now` earlier than the start counts
 * as no time.
 *
 * Study tracking: jn_clock_take_unlogged hands over the whole seconds run
 * since the last call, so writing them to the study log any number of
 * times never counts a second twice. Reset forgets what was not taken. */
#pragma once

#include <glib.h>

typedef enum { JN_CLOCK_STOPWATCH, JN_CLOCK_TIMER } JnClockMode;

typedef struct {
  JnClockMode mode;
  gboolean    running;
  gint64      started; /* when the current run began */
  gint64      banked;  /* run before the current run */
  gint64      length;  /* timer: its whole length */
  gint64      logged;  /* already handed over as study time */
} JnClock;

#define JN_TIMER_MAX_MINUTES 600 /* ten hours */

void      jn_clock_init           (JnClock *c, JnClockMode mode);
/* Makes it a stopped timer of `seconds` (1 s to JN_TIMER_MAX_MINUTES),
 * clamped into that range; what was not taken is forgotten. */
void      jn_clock_set_timer      (JnClock *c, gint64 seconds);
/* FALSE (and nothing changes) for a timer that has already finished. */
gboolean  jn_clock_start          (JnClock *c, gint64 now);
void      jn_clock_pause          (JnClock *c, gint64 now);
void      jn_clock_reset          (JnClock *c);
/* Time run so far, in microseconds; a timer's never exceeds its length. */
gint64    jn_clock_elapsed        (const JnClock *c, gint64 now);
/* Whole seconds to show: elapsed for a stopwatch, the time left (rounded
 * up, so it reads 0:00 only at the end) for a timer. */
gint64    jn_clock_shown          (const JnClock *c, gint64 now);
/* A timer that has reached zero: it stops itself (at exactly its length)
 * and returns TRUE once, the first time it is seen to have finished. */
gboolean  jn_clock_check_finished (JnClock *c, gint64 now);
gboolean  jn_clock_is_finished    (const JnClock *c, gint64 now);
/* Whole seconds run and not yet handed over; the rest waits. */
gint64    jn_clock_take_unlogged  (JnClock *c, gint64 now);

/* "0:05", "4:05", "59:59", "1:02:03". Negative counts as 0. */
char     *jn_clock_format         (gint64 seconds);
/* Study time in words: "no time", "under a minute", "25 min", "1 h",
 * "3 h 10 min". */
char     *jn_format_studied       (gint64 seconds);
