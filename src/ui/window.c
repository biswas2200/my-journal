/* window: see window.h. */
#include "window.h"

#include "active_time.h"
#include "activity_view.h"
#include "day_view.h"
#include "lock_view.h"
#include "security_view.h"
#include "sleep_watch.h"

struct _JrWindow {
  GtkApplicationWindow parent_instance;

  JrJournal    *journal;
  JrActiveTimer timer;
  guint         tick_id;
  JrSleepWatch *sleep;
  JrDayView    *day_view; /* the current screen if it is a day, else NULL */
  GtkWindow    *dialog;   /* not owned; cleared when it is destroyed */
};

G_DEFINE_FINAL_TYPE (JrWindow, jr_window, GTK_TYPE_APPLICATION_WINDOW)

gint64
jr_now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static void
flush_seconds (const char *day, gint64 seconds, gpointer user)
{
  JrWindow *win = user;
  jr_journal_add_active_seconds (win->journal, day, seconds);
}

static gboolean
on_tick (gpointer data)
{
  JrWindow *win = data;
  gint64 now = jr_now ();
  jr_active_timer_tick (&win->timer, now);
  if (win->day_view != NULL && jr_day_view_tick (win->day_view, now))
    jr_window_show_today (win); /* the date changed while idle */
  return G_SOURCE_CONTINUE;
}

static void
start_ticking (JrWindow *win)
{
  if (win->tick_id == 0)
    win->tick_id = g_timeout_add_seconds (1, on_tick, win);
}

static void
stop_ticking (JrWindow *win)
{
  g_clear_handle_id (&win->tick_id, g_source_remove);
  jr_active_timer_stop (&win->timer); /* flushes counted seconds */
}

static void
save_current (JrWindow *win)
{
  if (win->day_view != NULL)
    jr_day_view_save (win->day_view);
}

static void
close_dialog (JrWindow *win)
{
  GtkWindow *dialog = win->dialog;
  win->dialog = NULL;
  if (dialog != NULL)
    gtk_window_destroy (dialog);
}

static void
on_dialog_destroy (GtkWidget *dialog, gpointer data)
{
  JrWindow *win = data;
  if (win->dialog == GTK_WINDOW (dialog))
    win->dialog = NULL;
}

/* Replaces header and content. The old ones are destroyed. */
static void
set_screen (JrWindow *win, GtkWidget *header, GtkWidget *content)
{
  save_current (win);
  win->day_view = NULL;
  gtk_window_set_titlebar (GTK_WINDOW (win), header);
  gtk_window_set_child (GTK_WINDOW (win), content);
}

JrDayView *
jr_window_get_day_view (JrWindow *win)
{
  return win->day_view;
}

JrJournal *
jr_window_get_journal (JrWindow *win)
{
  return win->journal;
}

void
jr_window_show_day (JrWindow *win, JrDay day)
{
  if (!jr_journal_is_unlocked (win->journal))
    return;
  GtkWidget *header = NULL;
  JrDayView *view = jr_day_view_new (win, day, &header);
  set_screen (win, header, GTK_WIDGET (view));
  win->day_view = view;
  jr_day_view_focus (view);
}

typedef struct {
  JrWindow *win;
  JrDay     day;
} ShowDay;

static void
show_day_idle (gpointer data)
{
  ShowDay *sd = data;
  jr_window_show_day (sd->win, sd->day);
  g_object_unref (sd->win);
  g_free (sd);
}

void
jr_window_show_day_soon (JrWindow *win, JrDay day)
{
  ShowDay *sd = g_new (ShowDay, 1);
  sd->win = g_object_ref (win);
  sd->day = day;
  g_idle_add_once (show_day_idle, sd);
}

void
jr_window_show_today (JrWindow *win)
{
  jr_window_show_day (win, jr_day_today ());
}

void
jr_window_show_activity (JrWindow *win)
{
  if (!jr_journal_is_unlocked (win->journal))
    return;
  GtkWidget *header = NULL;
  GtkWidget *view = jr_activity_view_new (win, &header);
  set_screen (win, header, view);
}

void
jr_window_show_security (JrWindow *win)
{
  if (!jr_journal_is_unlocked (win->journal))
    return;
  GtkWidget *header = NULL;
  GtkWidget *view = jr_security_view_new (win, &header);
  set_screen (win, header, view);
}

static void
show_lock_screen (JrWindow *win, JrLockReason reason)
{
  GtkWidget *header = NULL;
  GtkWidget *view = jr_lock_view_new (win, reason, jr_now (), &header);
  set_screen (win, header, view);
}

void
jr_window_lock (JrWindow *win, JrLockReason reason)
{
  if (!jr_journal_lock_enabled (win->journal))
    {
      /* Nothing to lock with yet: Ctrl+L leads to the PIN settings. */
      if (reason == JR_LOCK_MANUAL)
        jr_window_show_security (win);
      return;
    }
  if (!jr_journal_is_unlocked (win->journal))
    return; /* already locked */

  save_current (win);
  stop_ticking (win);
  close_dialog (win);
  show_lock_screen (win, reason); /* destroys the old screen and its buffers */
  jr_journal_lock (win->journal);  /* zeroes the key */
}

void
jr_window_unlocked (JrWindow *win, gboolean via_recovery)
{
  jr_active_timer_init (&win->timer, flush_seconds, win);
  start_ticking (win);
  if (via_recovery)
    jr_window_show_security (win); /* likely time to choose a new PIN */
  else
    jr_window_show_today (win);
}

void
jr_window_note_keystroke (JrWindow *win)
{
  jr_active_timer_keystroke (&win->timer, jr_now ());
}

gint64
jr_window_session_seconds (JrWindow *win)
{
  return jr_active_timer_session_seconds (&win->timer);
}

gint64
jr_window_day_seconds (JrWindow *win, const char *day_iso)
{
  return jr_journal_active_seconds (win->journal, day_iso) +
         jr_active_timer_pending_for (&win->timer, day_iso);
}

static void
on_before_sleep (gpointer user)
{
  jr_window_lock (JR_WINDOW (user), JR_LOCK_SLEEP);
}

void
jr_window_settings_changed (JrWindow *win)
{
  jr_sleep_watch_set_enabled (win->sleep, jr_journal_lock_enabled (win->journal) &&
                                          jr_journal_lock_on_sleep (win->journal));
}

void
jr_window_set_dialog (JrWindow *win, GtkWindow *dialog)
{
  close_dialog (win);
  win->dialog = dialog;
  /* Dialogs are destroyed with the window, so `win` outlives this handler. */
  g_signal_connect (dialog, "destroy", G_CALLBACK (on_dialog_destroy), win);
}

/* Keyboard shortcuts. Capture phase, so they work while typing. */

static gboolean
sc_day_menu (GtkWidget *widget, GVariant *args, gpointer data)
{
  (void) args; (void) data;
  JrWindow *win = JR_WINDOW (widget);
  if (win->day_view != NULL)
    jr_day_view_open_days (win->day_view);
  else if (jr_journal_is_unlocked (win->journal))
    {
      jr_window_show_today (win);
      if (win->day_view != NULL)
        jr_day_view_open_days (win->day_view);
    }
  return TRUE;
}

static gboolean
sc_lock (GtkWidget *widget, GVariant *args, gpointer data)
{
  (void) args; (void) data;
  jr_window_lock (JR_WINDOW (widget), JR_LOCK_MANUAL);
  return TRUE;
}

static gboolean
sc_new_entry (GtkWidget *widget, GVariant *args, gpointer data)
{
  (void) args; (void) data;
  JrWindow *win = JR_WINDOW (widget);
  if (!jr_journal_is_unlocked (win->journal))
    return FALSE;
  /* New stamps only go on today's page, never on a past day. */
  if (win->day_view == NULL || !jr_day_view_is_current_today (win->day_view))
    jr_window_show_today (win);
  if (win->day_view != NULL)
    jr_day_view_new_entry (win->day_view);
  return TRUE;
}

static gboolean
sc_quit (GtkWidget *widget, GVariant *args, gpointer data)
{
  (void) args; (void) data;
  gtk_window_close (GTK_WINDOW (widget));
  return TRUE;
}

static void
add_shortcut (GtkShortcutController *c, const char *accel, GtkShortcutFunc fn)
{
  gtk_shortcut_controller_add_shortcut (
    c, gtk_shortcut_new (gtk_shortcut_trigger_parse_string (accel),
                         gtk_callback_action_new (fn, NULL, NULL)));
}

static void
install_shortcuts (JrWindow *win)
{
  GtkEventController *c = gtk_shortcut_controller_new ();
  gtk_event_controller_set_propagation_phase (c, GTK_PHASE_CAPTURE);
  GtkShortcutController *sc = GTK_SHORTCUT_CONTROLLER (c);
  add_shortcut (sc, "<Control>k", sc_day_menu);
  add_shortcut (sc, "<Control>l", sc_lock);
  add_shortcut (sc, "<Control>Return", sc_new_entry);
  add_shortcut (sc, "<Control>KP_Enter", sc_new_entry);
  add_shortcut (sc, "<Control>q", sc_quit);
  gtk_widget_add_controller (GTK_WIDGET (win), c);
}

/* Save when the window loses focus (spec: autosave on blur). */
static void
on_active_changed (GObject *obj, GParamSpec *pspec, gpointer data)
{
  (void) pspec; (void) data;
  JrWindow *win = JR_WINDOW (obj);
  if (!gtk_window_is_active (GTK_WINDOW (win)))
    {
      save_current (win);
      jr_active_timer_flush (&win->timer);
    }
}

/* Quitting locks: save, flush, then drop every text buffer and the key. */
static gboolean
on_close_request (GtkWindow *window)
{
  JrWindow *win = JR_WINDOW (window);
  save_current (win);
  stop_ticking (win);
  close_dialog (win);
  win->day_view = NULL;
  gtk_window_set_child (window, NULL);
  jr_journal_lock (win->journal);
  return FALSE; /* let it close */
}

static void
jr_window_dispose (GObject *object)
{
  JrWindow *win = JR_WINDOW (object);
  g_clear_handle_id (&win->tick_id, g_source_remove);
  g_clear_pointer (&win->sleep, jr_sleep_watch_free);
  win->day_view = NULL;
  /* Destroy the screen before the journal it may still point at. */
  gtk_window_set_child (GTK_WINDOW (win), NULL);
  g_clear_pointer (&win->journal, jr_journal_close);
  G_OBJECT_CLASS (jr_window_parent_class)->dispose (object);
}

static void
jr_window_class_init (JrWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = jr_window_dispose;
  GTK_WINDOW_CLASS (klass)->close_request = on_close_request;
}

static void
jr_window_init (JrWindow *win)
{
  gtk_window_set_default_size (GTK_WINDOW (win), 960, 600);
  gtk_window_set_title (GTK_WINDOW (win), "Journal");
  install_shortcuts (win);
  g_signal_connect (win, "notify::is-active", G_CALLBACK (on_active_changed), NULL);
}

JrWindow *
jr_window_new (GtkApplication *app, JrJournal *journal)
{
  JrWindow *win = g_object_new (JR_TYPE_WINDOW, "application", app, NULL);
  win->journal = journal;
  jr_active_timer_init (&win->timer, flush_seconds, win);
  win->sleep = jr_sleep_watch_new (on_before_sleep, win);
  jr_window_settings_changed (win);

  /* With a PIN, nothing is read until it is entered. */
  if (jr_journal_is_unlocked (journal))
    jr_window_unlocked (win, FALSE);
  else
    show_lock_screen (win, JR_LOCK_STARTUP);
  return win;
}
