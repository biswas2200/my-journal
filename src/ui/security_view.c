/* security_view: see security_view.h.
 *
 * Every change that weakens or reveals the lock (turning it off, changing
 * it, making a new recovery key) asks for the current PIN or passphrase
 * first, so someone at an unlocked laptop cannot take over the journal.
 * The slow Argon2id work runs on a worker thread as a lock job while the
 * dialog says "Working…"; the window stays responsive. */
#include "security_view.h"

#include <string.h>
#include "secret_dialog.h"
#include "widgets.h"

/* ---- running a lock job ---------------------------------------------- */

/* Survives the dialog: the job may finish after the dialog (or even the
 * window) is gone, e.g. if the lid closes meanwhile. */
typedef struct {
  JrWindow  *win;    /* weak */
  GtkWindow *dialog; /* weak */
} JobCtx;

static void
job_ctx_free (JobCtx *c)
{
  if (c->win != NULL)
    g_object_remove_weak_pointer (G_OBJECT (c->win), (gpointer *) &c->win);
  if (c->dialog != NULL)
    g_object_remove_weak_pointer (G_OBJECT (c->dialog), (gpointer *) &c->dialog);
  g_free (c);
}

static const char *
kind_word (JrJournal *j)
{
  return jr_journal_lock_kind (j) == JR_LOCK_PASSPHRASE ? "passphrase" : "PIN";
}

static void
result_message (JrJournal *j, JrJobResult r, char *buf, gsize len)
{
  gint64 wait = jr_journal_lockout_remaining (j, jr_now ());
  if ((r == JR_JOB_WRONG || r == JR_JOB_WAIT) && wait > 0)
    g_snprintf (buf, len, "Too many wrong tries. Wait %" G_GINT64_FORMAT " seconds.", wait);
  else if (r == JR_JOB_WRONG)
    g_snprintf (buf, len, "Wrong %s. %d tries left.", kind_word (j), jr_journal_tries_left (j));
  else if (r == JR_JOB_INVALID)
    g_strlcpy (buf, "That is not allowed here. Start again.", len);
  else
    g_strlcpy (buf, "Could not save. Try again.", len);
}

/* Success: close the dialog, rebuild the screen, show a new recovery key. */
static void
finish (JrWindow *win, GtkWindow *dialog, char *recovery)
{
  if (dialog != NULL)
    gtk_window_destroy (dialog);
  jr_window_settings_changed (win);
  jr_window_show_security (win);
  if (recovery != NULL)
    {
      jr_recovery_dialog_show (win, recovery);
      jr_secret_free (recovery);
    }
}

static void
job_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  (void) source; (void) cancel;
  jr_lock_job_run (data);
  g_task_return_boolean (task, TRUE);
}

static void
on_job_done (GObject *source, GAsyncResult *res, gpointer data)
{
  (void) source;
  JobCtx *c = data;
  JrLockJob *job = g_task_get_task_data (G_TASK (res));
  if (g_application_get_default () != NULL)
    g_application_release (g_application_get_default ());

  JrJournal *j = c->win != NULL ? jr_window_get_journal (c->win) : NULL;
  if (j == NULL)
    {
      jr_lock_job_free (job); /* the window is gone: drop it */
      job_ctx_free (c);
      return;
    }
  char *recovery = NULL;
  JrJobResult r = jr_journal_finish_lock_job (j, job, &recovery);
  if (r == JR_JOB_OK)
    finish (c->win, c->dialog, recovery);
  else if (c->dialog != NULL)
    {
      char msg[96];
      result_message (j, r, msg, sizeof msg);
      jr_secret_dialog_restart (c->dialog, msg);
    }
  job_ctx_free (c);
}

static void
start_job (JrWindow *win, GtkWindow *dialog, JrLockJob *job)
{
  jr_secret_dialog_set_busy (dialog, "Working… this takes about a second.");
  JobCtx *c = g_new0 (JobCtx, 1);
  c->win = win;
  c->dialog = dialog;
  g_object_add_weak_pointer (G_OBJECT (win), (gpointer *) &c->win);
  g_object_add_weak_pointer (G_OBJECT (dialog), (gpointer *) &c->dialog);
  /* Hold the app so quitting waits for the worker to finish. */
  if (g_application_get_default () != NULL)
    g_application_hold (g_application_get_default ());
  GTask *task = g_task_new (NULL, NULL, on_job_done, c);
  g_task_set_task_data (task, job, NULL);
  g_task_run_in_thread (task, job_thread);
  g_object_unref (task);
}

/* ---- what each dialog asks for --------------------------------------- */

typedef struct {
  JrWindow     *win;
  JrLockJobKind what;
  JrLockKind    new_kind;
  int           current_step; /* index of the current secret, or -1 */
  int           new_step;     /* index of the new secret (repeated next), or -1 */
} Plan;

static void
on_secrets (GtkWindow *dialog, const char *const *secrets, gpointer user)
{
  Plan *p = user;
  JrJournal *j = jr_window_get_journal (p->win);
  const char *current = p->current_step >= 0 ? secrets[p->current_step] : NULL;
  const char *fresh = p->new_step >= 0 ? secrets[p->new_step] : NULL;
  if (fresh != NULL && strcmp (fresh, secrets[p->new_step + 1]) != 0)
    {
      jr_secret_dialog_restart (dialog, "The two did not match. Start again.");
      return;
    }
  JrJobResult r;
  JrLockJob *job = jr_journal_begin_lock_job (j, p->what, current, p->new_kind, fresh,
                                              jr_now (), &r);
  if (job == NULL)
    {
      char msg[96];
      result_message (j, r, msg, sizeof msg);
      jr_secret_dialog_restart (dialog, msg);
      return;
    }
  start_job (p->win, dialog, job);
}

static void
ask (JrWindow *win, const char *title, const JrSecretStep *steps, int n, JrLockJobKind what,
     JrLockKind new_kind, int current_step, int new_step)
{
  Plan *p = g_new0 (Plan, 1);
  *p = (Plan){ win, what, new_kind, current_step, new_step };
  GtkWindow *dialog = jr_secret_dialog_new (win, title, steps, n, on_secrets, p);
  g_object_set_data_full (G_OBJECT (dialog), "jr-plan", p, g_free);
}

static JrSecretStep
current_step (JrJournal *j, const char *prompt_pin, const char *prompt_pass)
{
  JrLockKind k = jr_journal_lock_kind (j);
  return (JrSecretStep){ k == JR_LOCK_PIN ? prompt_pin : prompt_pass, k, FALSE };
}

static void
ask_new_lock (JrWindow *win, const char *title, JrLockJobKind what, JrLockKind kind)
{
  JrJournal *j = jr_window_get_journal (win);
  gboolean pin = kind == JR_LOCK_PIN;
  JrSecretStep steps[3];
  int n = 0, cur = -1;
  if (what == JR_JOB_CHANGE)
    {
      steps[n] = current_step (j, "Enter your current PIN", "Enter your current passphrase");
      cur = n++;
    }
  steps[n++] = (JrSecretStep){ pin ? "Choose a new 6-digit PIN" : "Choose a passphrase", kind, TRUE };
  steps[n++] = (JrSecretStep){ pin ? "Type the same PIN again" : "Type the same passphrase again",
                               kind, TRUE };
  ask (win, title, steps, n, what, kind, cur, n - 2);
}

static void
on_kind_chosen (JrWindow *win, JrLockKind kind)
{
  ask_new_lock (win, kind == JR_LOCK_PIN ? "Set a PIN" : "Set a passphrase", JR_JOB_ENABLE, kind);
}

static void
on_toggle (GtkButton *b, gpointer data)
{
  (void) b;
  JrWindow *win = data;
  JrJournal *j = jr_window_get_journal (win);
  if (!jr_journal_lock_enabled (j))
    {
      jr_lock_kind_dialog_show (win, on_kind_chosen);
      return;
    }
  JrSecretStep step = current_step (j, "Enter your current PIN to turn the lock off",
                                    "Enter your current passphrase to turn the lock off");
  ask (win, "Turn off the lock", &step, 1, JR_JOB_DISABLE, JR_LOCK_PIN, 0, -1);
}

static void
on_change (GtkButton *b, gpointer data)
{
  (void) b;
  JrWindow *win = data;
  JrLockKind kind = jr_journal_lock_kind (jr_window_get_journal (win));
  ask_new_lock (win, kind == JR_LOCK_PIN ? "Change PIN" : "Change passphrase", JR_JOB_CHANGE, kind);
}

static void
on_switch_kind (GtkButton *b, gpointer data)
{
  (void) b;
  JrWindow *win = data;
  gboolean to_pass = jr_journal_lock_kind (jr_window_get_journal (win)) == JR_LOCK_PIN;
  ask_new_lock (win, to_pass ? "Use a passphrase" : "Use a PIN", JR_JOB_CHANGE,
                to_pass ? JR_LOCK_PASSPHRASE : JR_LOCK_PIN);
}

static void
on_new_key (GtkButton *b, gpointer data)
{
  (void) b;
  JrWindow *win = data;
  JrSecretStep step = current_step (jr_window_get_journal (win),
                                    "Enter your PIN to make a new recovery key",
                                    "Enter your passphrase to make a new recovery key");
  ask (win, "New recovery key", &step, 1, JR_JOB_NEW_RECOVERY, JR_LOCK_PIN, 0, -1);
}

static void
on_sleep_toggled (GtkCheckButton *check, gpointer data)
{
  JrWindow *win = data;
  jr_journal_set_lock_on_sleep (jr_window_get_journal (win), gtk_check_button_get_active (check));
  jr_window_settings_changed (win);
}

static void
on_back (GtkButton *b, gpointer data)
{
  (void) b;
  jr_window_show_today (data);
}

static void
on_lock_now (GtkButton *b, gpointer data)
{
  (void) b;
  jr_window_lock (data, JR_LOCK_MANUAL);
}

/* ---- the screen ------------------------------------------------------ */

static GtkWidget *
card (void)
{
  GtkWidget *c = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class (c, "card");
  return c;
}

static GtkWidget *
titled (const char *title, const char *sub)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), jr_label (title, "card-title", "mono", NULL));
  if (sub != NULL)
    {
      GtkWidget *s = jr_label (sub, "card-sub", "mono", NULL);
      gtk_label_set_wrap (GTK_LABEL (s), TRUE);
      gtk_box_append (GTK_BOX (box), s);
    }
  return box;
}

/* A switch drawn with a button and a knob. (GtkSwitch in GTK 4.22 logs a
 * baseline warning on every layout, and this is lighter anyway.) The
 * button only opens a dialog; its look follows the saved state when the
 * screen is rebuilt after the dialog succeeds. */
static GtkWidget *
toggle_new (gboolean on, const char *label)
{
  GtkWidget *b = gtk_button_new ();
  gtk_widget_add_css_class (b, "jr-switch");
  if (on)
    gtk_widget_add_css_class (b, "on");
  GtkWidget *knob = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (knob, "knob");
  gtk_widget_set_halign (knob, on ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_widget_set_valign (knob, GTK_ALIGN_CENTER);
  gtk_button_set_child (GTK_BUTTON (b), knob);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  char text[96];
  g_snprintf (text, sizeof text, "%s: %s", label, on ? "on" : "off");
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
  return b;
}

static GtkWidget *
lock_card (JrWindow *win, JrLockKind kind)
{
  gboolean pin = kind == JR_LOCK_PIN;
  GtkWidget *c = card ();
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_hexpand (left, TRUE);
  gtk_box_append (GTK_BOX (left), jr_label (pin ? "PIN" : "Passphrase", "card-title", "mono", NULL));
  if (pin)
    {
      GtkWidget *dots = jr_pin_dots_new ();
      gtk_widget_add_css_class (dots, "small");
      gtk_widget_set_halign (dots, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (left), dots);
      GtkWidget *warn = jr_label ("Someone with a copy of the file could guess a 6-digit PIN in "
                                  "about a day. A passphrase cannot be guessed that way.",
                                  "card-sub", "mono", NULL);
      gtk_label_set_wrap (GTK_LABEL (warn), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (warn), 44);
      gtk_box_append (GTK_BOX (left), warn);
    }
  else
    gtk_box_append (GTK_BOX (left), jr_label ("Set · 12 or more characters", "card-sub", "mono", NULL));
  gtk_box_append (GTK_BOX (c), left);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign (buttons, GTK_ALIGN_CENTER);
  GtkWidget *change = gtk_button_new_with_label (pin ? "Change PIN" : "Change passphrase");
  g_signal_connect (change, "clicked", G_CALLBACK (on_change), win);
  gtk_box_append (GTK_BOX (buttons), change);
  GtkWidget *other = gtk_button_new_with_label (pin ? "Use a passphrase instead" : "Use a PIN instead");
  g_signal_connect (other, "clicked", G_CALLBACK (on_switch_kind), win);
  gtk_box_append (GTK_BOX (buttons), other);
  gtk_box_append (GTK_BOX (c), buttons);
  return c;
}

GtkWidget *
jr_security_view_new (JrWindow *win, GtkWidget **header_out)
{
  JrJournal *j = jr_window_get_journal (win);
  gboolean locked = jr_journal_lock_enabled (j);

  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_size_request (column, 520, -1);
  gtk_widget_set_halign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (column, 24);
  gtk_widget_set_margin_bottom (column, 24);
  gtk_widget_set_margin_start (column, 24);
  gtk_widget_set_margin_end (column, 24);

  GtkWidget *c1 = card ();
  gtk_box_append (GTK_BOX (c1), titled ("Lock the journal",
                                        "A PIN or passphrase, asked every time you open the app"));
  GtkWidget *sw = toggle_new (locked, "Lock the journal");
  g_signal_connect (sw, "clicked", G_CALLBACK (on_toggle), win);
  gtk_box_append (GTK_BOX (c1), sw);
  gtk_box_append (GTK_BOX (column), c1);

  if (locked)
    gtk_box_append (GTK_BOX (column), lock_card (win, jr_journal_lock_kind (j)));

  /* When to lock. */
  GtkWidget *c3 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (c3, "card");
  gtk_box_append (GTK_BOX (c3), jr_label ("LOCK AUTOMATICALLY WHEN", "section-title", NULL));
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *close_check = gtk_check_button_new_with_label ("I close the app");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (close_check), TRUE);
  gtk_widget_set_sensitive (close_check, FALSE);
  gtk_widget_set_hexpand (close_check, TRUE);
  gtk_box_append (GTK_BOX (row), close_check);
  gtk_box_append (GTK_BOX (row), jr_label ("always on", "muted", NULL));
  gtk_box_append (GTK_BOX (c3), row);
  GtkWidget *sleep_check = gtk_check_button_new_with_label ("The laptop lid closes or it goes to sleep");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (sleep_check), jr_journal_lock_on_sleep (j));
  gtk_widget_set_sensitive (sleep_check, locked);
  g_signal_connect (sleep_check, "toggled", G_CALLBACK (on_sleep_toggled), win);
  gtk_box_append (GTK_BOX (c3), sleep_check);
  gtk_box_append (GTK_BOX (column), c3);

  if (locked)
    {
      GtkWidget *c4 = card ();
      gtk_box_append (GTK_BOX (c4), titled ("Recovery key",
                                            "Shown once when you set the lock. If you forget "
                                            "your PIN or passphrase, this key opens your entries."));
      GtkWidget *mk = gtk_button_new_with_label ("Make a new key");
      gtk_widget_set_valign (mk, GTK_ALIGN_CENTER);
      g_signal_connect (mk, "clicked", G_CALLBACK (on_new_key), win);
      gtk_box_append (GTK_BOX (c4), mk);
      gtk_box_append (GTK_BOX (column), c4);
    }

  GtkWidget *note = jr_label ("After 5 wrong tries the app waits 30 seconds before the next try. "
                              "Entries are encrypted and stay on this laptop only.",
                              "muted", "mono", NULL);
  gtk_label_set_wrap (GTK_LABEL (note), TRUE);
  gtk_widget_set_margin_top (note, 4);
  gtk_box_append (GTK_BOX (column), note);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), column);

  GtkWidget *header = jr_header_new (NULL);
  GtkWidget *back = jr_button ("jr-arrow-left-symbolic", "Journal");
  g_signal_connect (back, "clicked", G_CALLBACK (on_back), win);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), back);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), jr_label ("Lock & security", "title-text", NULL));
  if (locked)
    {
      gtk_header_bar_pack_end (GTK_HEADER_BAR (header), jr_header_sep ());
      GtkWidget *lock = jr_button ("jr-lock-symbolic", "Lock now");
      gtk_widget_add_css_class (lock, "flat");
      gtk_widget_set_tooltip_text (lock, "Lock now (Ctrl+L)");
      g_signal_connect (lock, "clicked", G_CALLBACK (on_lock_now), win);
      gtk_header_bar_pack_end (GTK_HEADER_BAR (header), lock);
    }
  *header_out = header;
  return scroller;
}
