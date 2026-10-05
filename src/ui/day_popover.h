/* day_popover: screen 03, the day dropdown under the day button.
 *
 * Its content is built each time it opens and dropped when it closes, so
 * it costs no memory while hidden. */
#pragma once

#include <gtk/gtk.h>
#include "jdate.h"
#include "window.h"

/* `dim_target` gets the "dimmed" class while the popover is open. */
GtkWidget *jr_day_popover_new (JrWindow *win, JrDay current, GtkWidget *dim_target);
