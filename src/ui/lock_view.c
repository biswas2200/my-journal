/* lock_view: see lock_view.h.
 *
 * Digits are kept in a fixed 7-byte array and wiped after each try. The
 * Argon2id check runs on a worker thread (GTask) so the window stays
 * responsive; only that thread's private attempt object is touched there.
 */
#include "lock_view.h"

#include <string.h>
#include "widgets.h"

#define JR_TYPE_LOCK_VIEW (jr_lock_view_get_type ())
G_DECLARE_FINAL_TYPE (JrLockView, jr_lock_view, JR, LOCK_VIEW, GtkBox)

struct _JrLockView {
  GtkBox     parent_instance;
  JrWindow  *win;          /* NULL once disposed */
  char       digits[JR_PIN_LEN + 1];
  int        count;
  gboolean   busy;         /* a check is running */
  gboolean   recovery_mode;
  gboolean   passphrase;   /* locked with a passphrase, not a PIN */
  guint      countdown_id;
  char       reason[96];
  GtkWidget *dots, *pass_entry, *message, *pin_box, *recovery_box, *recovery_entry, *mode_button, *hint;
};

G_DEFINE_FINAL_TYPE (JrLockView, jr_lock_view, GTK_TYPE_BOX)

static void
wipe_digits (JrLockView *self)
{
  jr_wipe (self->digits, sizeof self->digits);
  self->count = 0;
  jr_pin_dots_set (self->dots, 0);
}

static void
set_message (JrLockView *self, const char *text)
{
  gtk_label_set_text (GTK_LABEL (self->message), text);
}

static gboolean
update_countdown (gpointer data)
{
  JrLockView *self = data;
  gint64 left = jr_journal_lockout_remaining (jr_window_get_journal (self->win), jr_now ());
  if (left <= 0)
    {
      self->countdown_id = 0;
      set_message (self, "You can try again now.");
      return G_SOURCE_REMOVE;
    }
  char text[96];
  g_snprintf (text, sizeof text, "Too many wrong tries. Try again in %" G_GINT64_FORMAT " s.", left);
  set_message (self, text);
  return G_SOURCE_CONTINUE;
}

static void
start_countdown (JrLockView *self)
{
  if (self->countdown_id == 0)
    self->countdown_id = g_timeout_add_seconds (1, update_countdown, self);
  update_countdown (self);
}

static gboolean
locked_out (JrLockView *self)
{
  return jr_journal_lockout_remaining (jr_window_get_journal (self->win), jr_now ()) > 0;
}

static void
verify_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  (void) source; (void) cancel;
  jr_unlock_attempt_run (data);
  g_task_return_boolean (task, TRUE);
}

static void
on_verified (GObject *source, GAsyncResult *res, gpointer data)
{
  gboolean via_recovery = GPOINTER_TO_INT (data);
  JrLockView *self = JR_LOCK_VIEW (source);
  JrUnlockAttempt *attempt = g_task_get_task_data (G_TASK (res));
  if (g_application_get_default () != NULL)
    g_application_release (g_application_get_default ());

  if (self->win == NULL)
    {
      /* The window closed while checking: drop the result. */
      jr_unlock_attempt_free (attempt);
      return;
    }

  self->busy = FALSE;
  JrJournal *journal = jr_window_get_journal (self->win);
  switch (jr_journal_finish_unlock (journal, attempt))
    {
    case JR_UNLOCK_OK:
      jr_window_unlocked (self->win, via_recovery); /* replaces this view */
      return;
    case JR_UNLOCK_WRONG:
      if (locked_out (self))
        start_countdown (self);
      else
        {
          char text[96];
          int left = jr_journal_tries_left (journal);
          g_snprintf (text, sizeof text, "%s. %d %s left before a 30 second wait.",
                      via_recovery ? "That key does not match"
                                   : self->passphrase ? "Wrong passphrase" : "Wrong PIN",
                      left, left == 1 ? "try" : "tries");
          set_message (self, text);
        }
      break;
    case JR_UNLOCK_WAIT:
      start_countdown (self);
      break;
    }
}

/* Starts a check of `secret`; the caller wipes its own copy afterwards. */
static void
submit (JrLockView *self, JrSecretKind kind, const char *secret)
{
  JrJournal *journal = jr_window_get_journal (self->win);
  JrUnlockAttempt *attempt = jr_journal_begin_unlock (journal, kind, secret, jr_now ());
  if (attempt == NULL)
    {
      start_countdown (self);
      return;
    }
  self->busy = TRUE;
  set_message (self, "Checking…");

  /* Hold the app so quitting waits for the worker to finish. */
  if (g_application_get_default () != NULL)
    g_application_hold (g_application_get_default ());
  GTask *task = g_task_new (self, NULL, on_verified,
                            GINT_TO_POINTER (kind == JR_SECRET_RECOVERY));
  g_task_set_task_data (task, attempt, NULL);
  g_task_run_in_thread (task, verify_thread);
  g_object_unref (task);
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state,
        gpointer data)
{
  (void) c; (void) keycode;
  JrLockView *self = data;
  if (self->recovery_mode || self->passphrase || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) != 0)
    return FALSE; /* text fields take their own keys */

  guint32 ch = gdk_keyval_to_unicode (keyval);
  gboolean is_digit = ch >= '0' && ch <= '9';
  if (!is_digit && keyval != GDK_KEY_BackSpace && keyval != GDK_KEY_Escape)
    return FALSE;
  if (self->busy || locked_out (self))
    return TRUE; /* swallow input while checking or waiting */

  if (keyval == GDK_KEY_BackSpace)
    {
      if (self->count > 0)
        self->digits[--self->count] = '\0';
      jr_pin_dots_set (self->dots, self->count);
      return TRUE;
    }
  if (keyval == GDK_KEY_Escape)
    {
      wipe_digits (self);
      return TRUE;
    }

  self->digits[self->count++] = (char) ch;
  jr_pin_dots_set (self->dots, self->count);
  if (self->count == JR_PIN_LEN)
    {
      submit (self, JR_SECRET_LOCK, self->digits);
      jr_wipe (self->digits, sizeof self->digits);
      self->count = 0;
      /* Let the sixth dot show briefly, then clear while checking. */
      jr_pin_dots_set (self->dots, 0);
    }
  return TRUE;
}

static void
on_recovery_activate (GtkEntry *entry, gpointer data)
{
  JrLockView *self = data;
  if (self->busy)
    return;
  submit (self, JR_SECRET_RECOVERY, gtk_editable_get_text (GTK_EDITABLE (entry)));
  jr_wipe_editable (GTK_EDITABLE (entry));
}

static void
on_passphrase_activate (GtkWidget *entry, gpointer data)
{
  JrLockView *self = data;
  if (self->busy)
    return;
  submit (self, JR_SECRET_LOCK, gtk_editable_get_text (GTK_EDITABLE (entry)));
  jr_wipe_editable (GTK_EDITABLE (entry));
}

static const char *
unlock_hint (JrLockView *self)
{
  if (self->recovery_mode)
    return "Type the recovery key you wrote down, then press Enter.";
  return self->passphrase ? "Type your passphrase and press Enter."
                          : "Type the digits on your keyboard. It unlocks by itself on the 6th.";
}

static void
focus_input (JrLockView *self)
{
  if (self->recovery_mode)
    gtk_widget_grab_focus (self->recovery_entry);
  else if (self->passphrase)
    gtk_widget_grab_focus (self->pass_entry);
  else
    gtk_widget_grab_focus (GTK_WIDGET (self));
}

static void
set_recovery_mode (JrLockView *self, gboolean on)
{
  self->recovery_mode = on;
  wipe_digits (self);
  jr_wipe_editable (GTK_EDITABLE (self->recovery_entry));
  gtk_widget_set_visible (self->pin_box, !on);
  gtk_widget_set_visible (self->recovery_box, on);
  jr_wipe_editable (GTK_EDITABLE (self->pass_entry));
  gtk_button_set_label (GTK_BUTTON (self->mode_button),
                        on ? (self->passphrase ? "Use passphrase instead" : "Use PIN instead")
                           : (self->passphrase ? "Forgot it? Use recovery key"
                                               : "Forgot PIN? Use recovery key"));
  gtk_label_set_text (GTK_LABEL (self->hint), unlock_hint (self));
  if (!locked_out (self))
    set_message (self, self->reason);
  focus_input (self);
}

static void
on_mode_clicked (GtkButton *b, gpointer data)
{
  (void) b;
  JrLockView *self = data;
  set_recovery_mode (self, !self->recovery_mode);
}

static void
on_map (GtkWidget *widget, gpointer data)
{
  (void) data;
  focus_input (JR_LOCK_VIEW (widget));
}

static void
jr_lock_view_dispose (GObject *object)
{
  JrLockView *self = JR_LOCK_VIEW (object);
  g_clear_handle_id (&self->countdown_id, g_source_remove);
  jr_wipe (self->digits, sizeof self->digits);
  if (self->recovery_entry != NULL)
    jr_wipe_editable (GTK_EDITABLE (self->recovery_entry));
  if (self->pass_entry != NULL)
    jr_wipe_editable (GTK_EDITABLE (self->pass_entry));
  self->recovery_entry = self->pass_entry = NULL;
  self->win = NULL;
  G_OBJECT_CLASS (jr_lock_view_parent_class)->dispose (object);
}

static void
jr_lock_view_class_init (JrLockViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = jr_lock_view_dispose;
}

static void
jr_lock_view_init (JrLockView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

static void
format_reason (char *buf, gsize len, JrLockReason reason, gint64 at)
{
  char clock[16];
  jr_format_clock (at, clock, sizeof clock);
  switch (reason)
    {
    case JR_LOCK_SLEEP:
      g_snprintf (buf, len, "Locked automatically · laptop lid closed or went to sleep at %s", clock);
      break;
    case JR_LOCK_MANUAL:
      g_snprintf (buf, len, "Locked at %s", clock);
      break;
    case JR_LOCK_STARTUP:
    default:
      g_snprintf (buf, len, "Locked · the journal always opens locked");
      break;
    }
}

GtkWidget *
jr_lock_view_new (JrWindow *win, JrLockReason reason, gint64 locked_at, GtkWidget **header_out)
{
  JrLockView *self = g_object_new (JR_TYPE_LOCK_VIEW, NULL);
  self->win = win;
  format_reason (self->reason, sizeof self->reason, reason, locked_at);

  GtkWidget *center = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (center, TRUE);
  gtk_widget_set_valign (center, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (center, GTK_ALIGN_CENTER);

  GtkWidget *icon = gtk_image_new_from_icon_name ("jr-lock-symbolic");
  gtk_widget_add_css_class (icon, "lock-icon");
  gtk_box_append (GTK_BOX (center), icon);

  GtkWidget *title = jr_label ("Journal", "lock-title", NULL);
  gtk_label_set_xalign (GTK_LABEL (title), 0.5f);
  gtk_widget_set_margin_top (title, 18);
  gtk_box_append (GTK_BOX (center), title);

  self->passphrase = jr_journal_lock_kind (jr_window_get_journal (win)) == JR_LOCK_PASSPHRASE;
  self->pin_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *prompt = jr_label (self->passphrase ? "Enter your passphrase" : "Enter your 6-digit PIN",
                                "muted", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (prompt), 0.5f);
  gtk_widget_set_margin_top (prompt, 8);
  gtk_box_append (GTK_BOX (self->pin_box), prompt);
  self->dots = jr_pin_dots_new ();
  gtk_widget_set_margin_top (self->dots, 26);
  gtk_box_append (GTK_BOX (self->pin_box), self->dots);
  /* A passphrase is typed into a hidden field (input methods do not learn it). */
  self->pass_entry = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (self->pass_entry), TRUE);
  gtk_widget_set_size_request (self->pass_entry, 340, -1);
  gtk_widget_set_margin_top (self->pass_entry, 22);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->pass_entry),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Passphrase", -1);
  g_signal_connect (self->pass_entry, "activate", G_CALLBACK (on_passphrase_activate), self);
  gtk_box_append (GTK_BOX (self->pin_box), self->pass_entry);
  gtk_widget_set_visible (self->dots, !self->passphrase);
  gtk_widget_set_visible (self->pass_entry, self->passphrase);
  gtk_box_append (GTK_BOX (center), self->pin_box);

  self->recovery_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (self->recovery_box, 18);
  self->recovery_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->recovery_entry), "XXXX-XXXX-XXXX-XXXX-XXXX-XXXX");
  gtk_entry_set_max_length (GTK_ENTRY (self->recovery_entry), 40);
  gtk_entry_set_input_hints (GTK_ENTRY (self->recovery_entry),
                             GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_UPPERCASE_CHARS |
                             GTK_INPUT_HINT_PRIVATE);
  gtk_widget_set_size_request (self->recovery_entry, 340, -1);
  g_signal_connect (self->recovery_entry, "activate", G_CALLBACK (on_recovery_activate), self);
  gtk_box_append (GTK_BOX (self->recovery_box), self->recovery_entry);
  gtk_widget_set_visible (self->recovery_box, FALSE);
  gtk_box_append (GTK_BOX (center), self->recovery_box);

  self->message = jr_label (self->reason, "muted", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (self->message), 0.5f);
  gtk_label_set_wrap (GTK_LABEL (self->message), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->message), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_top (self->message, 26);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->message),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Lock status", -1);
  gtk_box_append (GTK_BOX (center), self->message);

  self->mode_button = gtk_button_new_with_label (self->passphrase ? "Forgot it? Use recovery key"
                                                                  : "Forgot PIN? Use recovery key");
  gtk_widget_add_css_class (self->mode_button, "link-button");
  gtk_widget_set_halign (self->mode_button, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (self->mode_button, 14);
  gtk_widget_set_focus_on_click (self->mode_button, FALSE);
  g_signal_connect (self->mode_button, "clicked", G_CALLBACK (on_mode_clicked), self);
  gtk_box_append (GTK_BOX (center), self->mode_button);

  gtk_box_append (GTK_BOX (self), center);

  GtkWidget *status = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (status, "statusbar");
  self->hint = jr_label (unlock_hint (self), NULL, NULL);
  gtk_widget_set_hexpand (self->hint, TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->hint), 0.5f);
  gtk_box_append (GTK_BOX (status), self->hint);
  gtk_box_append (GTK_BOX (self), status);

  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);

  if (locked_out (self))
    start_countdown (self);

  *header_out = jr_header_new ("Journal");
  return GTK_WIDGET (self);
}
