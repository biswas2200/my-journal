/* bar_chart: minutes per day for the last 7 days (screen 04).
 * Drawn directly with GtkSnapshot: one widget, no child per bar. */
#pragma once

#include <gtk/gtk.h>

#define JR_BAR_CHART_BARS 7

#define JR_TYPE_BAR_CHART (jr_bar_chart_get_type ())
G_DECLARE_FINAL_TYPE (JrBarChart, jr_bar_chart, JR, BAR_CHART, GtkWidget)

GtkWidget *jr_bar_chart_new (void);
/* `labels` are copied (short weekday names). `highlight` is drawn white. */
void       jr_bar_chart_set (JrBarChart *chart, const int minutes[JR_BAR_CHART_BARS],
                             const char *const labels[JR_BAR_CHART_BARS], int highlight);
