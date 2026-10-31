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
#include "client.h"
#include "clock_bar.h"
#include "driveclient.h"
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

/* New subject or topic (the topic in the selected subject, opened);
 * 0 when the name is empty or taken. */
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

/* The AI that writes enhanced copies (taken over; NULL: no AI). */
void      jn_window_set_enhancer   (JnWindow *win, JnEnhancer *enhancer);
/* Asks for an enhanced copy of the open session: the first time, after
 * a question about what is sent where. FALSE if it did not start. */
gboolean  jn_window_enhance        (JnWindow *win);
/* The enhanced copy (or reference) on screen, as Markdown, or NULL. */
char     *jn_window_enhanced_text  (JnWindow *win);
/* The topic reference: every enhanced copy of the topic, oldest first,
 * read-only; Export then writes "<topic> (reference).md". */
void      jn_window_open_reference (JnWindow *win, gint64 topic);
gboolean  jn_window_in_reference   (JnWindow *win);
gboolean  jn_window_is_enhancing   (JnWindow *win);
/* The study stopwatch and timer, and "Today 25 min · 3 h in all" for
 * the open topic. Study time goes to the topic open while it ran. */
JnClockBar *jn_window_clock_bar    (JnWindow *win);
const char *jn_window_studied      (JnWindow *win);
/* ---- Google Drive ------------------------------------------------------
 * The backup to Google Drive (driveclient.h, taken over; NULL: none).
 * Signed in, each topic shows whether it is backed up ("Backed up 3 Nov
 * 14:05", "Changed since the last backup (1 Nov)", "Not backed up yet"),
 * a topic that needs it is marked in the list, and closing Notes first
 * backs up whatever changed (the window hides meanwhile, a minute at
 * most). Each topic is recorded the moment its backup is done, so a
 * backup cut short loses nothing it made. */
void      jn_window_set_drive      (JnWindow *win, JnBackup *backup);
void      jn_window_drive_sign_in  (JnWindow *win);
void      jn_window_drive_sign_out (JnWindow *win);
/* FALSE if nothing started (not signed in, one already running, nothing
 * to do). */
gboolean  jn_window_backup_topic   (JnWindow *win);
gboolean  jn_window_backup_all     (JnWindow *win);
gboolean  jn_window_is_backing_up  (JnWindow *win);
/* How long closing waits for the backup (60 s; tests use less). What is
 * done by then is kept; the rest stays marked for the next backup. */
void      jn_window_set_close_limit (JnWindow *win, guint ms);
/* The open topic's backup state, as shown ("" when not signed in and
 * never backed up). */
const char *jn_window_backup_state (JnWindow *win);

/* Daily backups of the store in `dir` (notebackup.h): one now, and the
 * day's copy again when the window closes. */
void      jn_window_set_backup_dir (JnWindow *win, const char *dir);
/* Where the open note was rescued to after a failed save, or NULL. */
const char *jn_window_rescue_path  (JnWindow *win);
/* The status line's text (for tests). */
const char *jn_window_status       (JnWindow *win);

/* What is open, for tests and the program around the window. */
gint64    jn_window_topic          (JnWindow *win);
gint64    jn_window_session        (JnWindow *win); /* 0 until the first keystroke */
JnEditor *jn_window_editor         (JnWindow *win);
JnDb     *jn_window_db             (JnWindow *win);
