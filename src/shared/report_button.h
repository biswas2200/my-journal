/* report_button: "⚑ Report" for a header bar, on the start screen and
 * in Notes. Its small menu:
 *
 *   Something is broken      the bug form
 *   Something looks wrong    the UI form
 *   An idea or improvement   the idea form
 *
 * Each opens the project's GitHub form (report.h) in your browser,
 * through the desktop's portal (openuri.h), with the version, the system
 * and the part of the app filled in. What happened, the steps and the
 * screenshots go into the form there: nothing is sent from here.
 *
 * The Journal has no button of its own: it may not open links (its
 * AppArmor lock), so its reports start from the start screen. */
#pragma once

#include <gtk/gtk.h>
#include "report.h"

/* Opens a form's address; FALSE (and why) if nothing took it. */
typedef gboolean (*JrReportOpen) (const char *uri, gpointer user, GError **error);

/* The button. `part` ("Notes"; NULL: none) starts the report's title. */
GtkWidget *jr_report_button_new        (const char *part, const char *version);
/* Tests: `open` in place of the browser. */
void       jr_report_button_set_opener (GtkWidget *button, JrReportOpen open, gpointer user);
/* Opens the form for `kind`, as its menu item does. When no browser
 * takes it, the address goes on the clipboard and a dialog says so;
 * returns FALSE then. */
gboolean   jr_report_button_choose     (GtkWidget *button, JrReportKind kind);
