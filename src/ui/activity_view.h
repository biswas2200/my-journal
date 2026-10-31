/* activity_view: screen 04. Month calendar shaded by minutes written,
 * four stat cards, and minutes for the last 7 days. Reads only the
 * entries and day_time tables, never entry text. */
#pragma once

#include <gtk/gtk.h>
#include "window.h"

/* The screen, and its header bar in *header_out (the window shows it). */
GtkWidget *jr_activity_view_new (JrWindow *win, GtkWidget **header_out);
