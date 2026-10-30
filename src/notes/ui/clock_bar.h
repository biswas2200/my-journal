/* clock_bar: the study stopwatch and timer, one small row.
 *
 *   [Stopwatch|Timer] [25 min] 24:59 [Pause] [Reset]
 *
 * Stopwatch counts up; Timer counts down from its minutes (1 to 600,
 * 25 to start with) and stops itself at zero, with a bell. The mode and
 * the minutes can only be changed while stopped. The clock is
 * studyclock.h, so a sleeping laptop is not counted.
 *
 * Study time leaves the bar only through the "study" signal (whole
 * seconds, each second once): on pause, reset, a mode change, when a
 * timer ends, once a minute while running, and on jn_clock_bar_flush,
 * which the window calls before the topic changes or Notes closes.
 * "finished" follows when a timer reaches zero. */
#pragma once

#include <gtk/gtk.h>
#include "studyclock.h"

#define JN_TYPE_CLOCK_BAR (jn_clock_bar_get_type ())
G_DECLARE_FINAL_TYPE (JnClockBar, jn_clock_bar, JN, CLOCK_BAR, GtkBox)

typedef gint64 (*JnNowFunc) (gpointer user);

GtkWidget   *jn_clock_bar_new          (void);
/* Start, or pause; a finished timer starts again from the top. */
void         jn_clock_bar_toggle       (JnClockBar *b);
void         jn_clock_bar_pause        (JnClockBar *b);
void         jn_clock_bar_reset        (JnClockBar *b);
void         jn_clock_bar_set_mode     (JnClockBar *b, JnClockMode mode);
void         jn_clock_bar_set_minutes  (JnClockBar *b, int minutes);
/* Hands over the study time not yet reported ("study"), if any. */
void         jn_clock_bar_flush        (JnClockBar *b);
gboolean     jn_clock_bar_running      (JnClockBar *b);
JnClockMode  jn_clock_bar_mode         (JnClockBar *b);
/* The time on show ("24:59"). */
const char  *jn_clock_bar_text         (JnClockBar *b);

/* For tests: a stand-in monotonic clock (microseconds), and one tick of
 * what the bar does on its own while running. */
void         jn_clock_bar_set_now_func (JnClockBar *b, JnNowFunc now, gpointer user);
void         jn_clock_bar_tick         (JnClockBar *b);
