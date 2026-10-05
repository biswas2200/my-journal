/* widgets: tiny helpers shared by the screens. Plain GTK widgets, no
 * subclasses, so there is nothing extra to free. */
#pragma once

#include <gtk/gtk.h>

/* A label with CSS classes (NULL-terminated list). */
GtkWidget *jr_label          (const char *text, const char *first_class, ...) G_GNUC_NULL_TERMINATED;
/* A button with an optional icon and a label. */
GtkWidget *jr_button         (const char *icon_name, const char *text);
/* Row of six PIN dots; jr_pin_dots_set fills the first `n`. */
GtkWidget *jr_pin_dots_new   (void);
void       jr_pin_dots_set   (GtkWidget *dots, int n);
/* Muted small-caps title over a value, used for stats. */
GtkWidget *jr_stat_new       (const char *title, GtkWidget **value_out);
/* Header bar with window controls; `title` may be NULL. */
GtkWidget *jr_header_new     (const char *title);
/* 1 px vertical separator for the header. */
GtkWidget *jr_header_sep     (void);
/* Zeroes the text of an editable before it is destroyed. */
void       jr_wipe_editable  (GtkEditable *editable);
