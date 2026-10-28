/* window: the single app window. Owns the journal, switches screens,
 * runs the 1-second ticker and handles locking.
 *
 * Only the visible screen exists. Switching screens destroys the old one,
 * which frees its widgets and text buffers. */
#pragma once

#include <gtk/gtk.h>
#include "jdate.h"
#include "journal.h"

typedef enum {
  JR_LOCK_STARTUP,
  JR_LOCK_MANUAL,
  JR_LOCK_SLEEP,
} JrLockReason;

#define JR_TYPE_WINDOW (jr_window_get_type ())
G_DECLARE_FINAL_TYPE (JrWindow, jr_window, JR, WINDOW, GtkApplicationWindow)

/* Called once when the person leaves through Home or Notes, with the
 * exit code for the launcher (programs.h), right before the window
 * saves, locks and closes. */
typedef void (*JrLeaveFunc) (int code, gpointer user);

/* Takes ownership of `journal`; it is closed when the window goes away. */
JrWindow  *jr_window_new              (GtkApplication *app, JrJournal *journal);
JrJournal *jr_window_get_journal      (JrWindow *win);

void       jr_window_show_today       (JrWindow *win);
void       jr_window_show_day         (JrWindow *win, JrDay day);
/* Same, from an idle callback: safe inside a handler of the old screen. */
void       jr_window_show_day_soon    (JrWindow *win, JrDay day);
void       jr_window_show_activity    (JrWindow *win);
void       jr_window_show_security    (JrWindow *win);

/* Saves, wipes everything on screen and in memory, shows the lock screen. */
void       jr_window_lock             (JrWindow *win, JrLockReason reason);
/* Called by the lock screen after a correct secret. Asks for a new PIN
 * or a master passphrase first when the journal needs one. */
void       jr_window_unlocked         (JrWindow *win, JrSecretKind via);

/* Active-time counter. */
void       jr_window_note_keystroke   (JrWindow *win);
gint64     jr_window_session_seconds  (JrWindow *win);
/* Saved plus not-yet-flushed seconds for a day. */
gint64     jr_window_day_seconds      (JrWindow *win, const char *day_iso);

/* Re-reads lock settings (PIN on/off, lock on sleep). */
void       jr_window_settings_changed (JrWindow *win);
/* A modal window that must disappear when the journal locks. */
void       jr_window_set_dialog       (JrWindow *win, GtkWindow *dialog);

/* Started by the launcher: adds Home and Notes to every screen. */
void       jr_window_set_navigation   (JrWindow *win, JrLeaveFunc leave, gpointer user);
/* Reports `code` to the leave function, then saves, locks and closes. */
void       jr_window_leave            (JrWindow *win, int code);

/* The day screen if it is showing, else NULL (used by tests). */
struct _JrDayView *jr_window_get_day_view (JrWindow *win);

gint64     jr_now                     (void);
