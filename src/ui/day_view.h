/* day_view: screen 02 (today, writing) and past days (read-only + Edit).
 *
 * A day is a column of stamped blocks: time on the left, text on the
 * right. A new block is a draft until its first keystroke, which stamps
 * it from the system clock. Changes save about one second after typing
 * stops; the window also saves on lock, blur and quit.
 */
#pragma once

#include <gtk/gtk.h>
#include "jdate.h"
#include "window.h"

#define JR_TYPE_DAY_VIEW (jr_day_view_get_type ())
G_DECLARE_FINAL_TYPE (JrDayView, jr_day_view, JR, DAY_VIEW, GtkBox)

JrDayView *jr_day_view_new              (JrWindow *win, JrDay day, GtkWidget **header_out);
/* Writes every changed block now. */
void       jr_day_view_save             (JrDayView *view);
/* Called once per second. Returns TRUE if the calendar day rolled over
 * while idle and today's page should be rebuilt. */
gboolean   jr_day_view_tick             (JrDayView *view, gint64 now);
void       jr_day_view_focus            (JrDayView *view);
/* Ctrl+Enter: start a new stamped block (today only). */
void       jr_day_view_new_entry        (JrDayView *view);
/* Ctrl+K: open the day dropdown. */
void       jr_day_view_open_days        (JrDayView *view);
/* TRUE if this view shows today's date (and the date has not rolled over). */
gboolean   jr_day_view_is_current_today (JrDayView *view);
