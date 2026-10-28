/* notes_window: subjects and topics on the left, the note on the right.
 *
 * Opening a topic shows an empty page; the study session starts with the
 * first keystroke (so just looking creates nothing) and saves about a
 * second after typing stops, and when you switch, export or leave. A
 * session left empty is removed. Export writes the whole topic as
 * Markdown to <export folder>/<subject>/<topic>.md. */
#pragma once

#include <gtk/gtk.h>
#include "editor.h"
#include "notesdb.h"

#define JN_TYPE_WINDOW (jn_window_get_type ())
G_DECLARE_FINAL_TYPE (JnWindow, jn_window, JN, WINDOW, GtkApplicationWindow)

/* Called once when the person leaves through Home or Journal, with the
 * exit code for the launcher (programs.h), before the window closes. */
typedef void (*JnLeaveFunc) (int code, gpointer user);

/* Takes ownership of `db`. */
JnWindow *jn_window_new            (GtkApplication *app, JnDb *db, const char *export_dir);
void      jn_window_set_navigation (JnWindow *win, JnLeaveFunc leave, gpointer user);
void      jn_window_leave          (JnWindow *win, int code);

gint64    jn_window_add_subject    (JnWindow *win, const char *name);
/* In the selected subject (or the open topic's). */
gint64    jn_window_add_topic      (JnWindow *win, const char *name);
void      jn_window_select_subject (JnWindow *win, gint64 subject);
void      jn_window_open_topic     (JnWindow *win, gint64 topic);
void      jn_window_open_session   (JnWindow *win, gint64 session);
/* Saves, then shows an empty page for a new session in the same topic. */
void      jn_window_new_session    (JnWindow *win);
void      jn_window_save           (JnWindow *win);
/* Writes the open topic as Markdown; returns the file's path. */
char     *jn_window_export         (JnWindow *win, GError **error);

gint64    jn_window_topic          (JnWindow *win);
gint64    jn_window_session        (JnWindow *win); /* 0 until the first keystroke */
JnEditor *jn_window_editor         (JnWindow *win);
JnDb     *jn_window_db             (JnWindow *win);
