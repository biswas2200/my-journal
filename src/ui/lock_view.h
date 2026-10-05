/* lock_view: screen 01. Six dots, submits on the sixth digit. */
#pragma once

#include <gtk/gtk.h>
#include "window.h"

GtkWidget *jr_lock_view_new (JrWindow *win, JrLockReason reason, gint64 locked_at,
                             GtkWidget **header_out);
