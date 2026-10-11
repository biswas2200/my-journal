/* lock_view: see lock_view.h.
 *
 * Three ways in: the PIN or passphrase, the master passphrase (PIN locks)
 * and the recovery key. After 5 wrong PINs the PIN is blocked and the
 * screen asks for the master passphrase instead. Digits are kept in a
 * fixed 7-byte array and wiped after each try. The Argon2id check runs on
 * a worker thread (GTask) so the window stays responsive; only that
 * thread's private attempt object is touched there.
 */
#include "lock_view.h"

#include <string.h>
#include "widgets.h"

#define JR_TYPE_LOCK_VIEW (jr_lock_view_get_type ())
G_DECLARE_FINAL_TYPE (JrLockView, jr_lock_view, JR, LOCK_VIEW, GtkBox)

typedef enum { MODE_LOCK, MODE_MASTER, MODE_RECOVERY } Mode;

struct _JrLockView {
  GtkBox     parent_instance;
  JrWindow  *win;          /* NULL once disposed */
  char       digits[JR_PIN_LEN + 1];
  int        count;
  gboolean   busy;         /* a check is running */
  Mode       mode;
  gboolean   passphrase;   /* locked with a passphrase, not a PIN */
  guint      countdown_id;
  char       reason[96];
  GtkWidget *prompt, *dots, *pass_entry, *message, *recovery_box, *recovery_entry, *links[2], *hint;
};

G_DEFINE_FINAL_TYPE (JrLockView, jr_lock_view, GTK_TYPE_BOX)

static JrJournal *
journal (JrLockView *self)
{
  return jr_window_get_journal (self->win);
}

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

/* The PIN has no wait (it blocks instead); everything else does. */
static gboolean
mode_waits (JrLockView *self)
{
  return self->mode != MODE_LOCK || self->passphrase;
}

static gboolean
locked_out (JrLockView *self)
{
  return mode_waits (self) && jr_journal_lockout_remaining (journal (self), jr_now ()) > 0;
}

static gboolean
update_countdown (gpointer data)
{
  JrLockView *self = data;
  gint64 left = jr_journal_lockout_remaining (journal (self), jr_now ());
  if (left <= 0 || !mode_waits (self))
    {
      self->countdown_id = 0;
      set_message (self, left <= 0 ? "You can try again now." : self->reason);
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

static const char *
unlock_hint (JrLockView *self)
{
  switch (self->mode)
    {
    case MODE_RECOVERY:
      return "Type the recovery key you wrote down, then press Enter.";
    case MODE_MASTER:
      return "Type your master passphrase and press Enter.";
    case MODE_LOCK:
    default:
      return self->passphrase ? "Type your passphrase and press Enter."
                              : "Type the digits on your keyboard. It unlocks by itself on the 6th.";
    }
}

static const char *
mode_prompt (JrLockView *self)
{
  switch (self->mode)
    {
    case MODE_RECOVERY:
      return "Enter your recovery key";
    case MODE_MASTER:
      return "Enter your master passphrase";
    case MODE_LOCK:
    default:
      return self->passphrase ? "Enter your passphrase" : "Enter your 6-digit PIN";
    }
}

static const char *
link_label (JrLockView *self, Mode target)
{
  switch (target)
    {
    case MODE_LOCK:
      return self->passphrase ? "Use passphrase instead" : "Use PIN instead";
    case MODE_MASTER:
      return self->mode == MODE_LOCK ? "Forgot PIN? Use master passphrase" : "Use master passphrase";
    case MODE_RECOVERY:
    default:
      return self->mode == MODE_LOCK && self->passphrase ? "Forgot it? Use recovery key" : "Use recovery key";
    }
}

static void
focus_input (JrLockView *self)
{
  if (self->mode == MODE_RECOVERY)
    gtk_widget_grab_focus (self->recovery_entry);
  else if (self->mode == MODE_MASTER || self->passphrase)
    gtk_widget_grab_focus (self->pass_entry);
  else
    gtk_widget_grab_focus (GTK_WIDGET (self));
}

/* Switches the way in; `message` replaces the status line (NULL keeps the
 * lock reason). */
static void
set_mode (JrLockView *self, Mode mode, const char *message)
{
  JrJournal *j = journal (self);
  self->mode = mode;
  wipe_digits (self);
  jr_wipe_editable (GTK_EDITABLE (self->recovery_entry));
  jr_wipe_editable (GTK_EDITABLE (self->pass_entry));
  gboolean pin_dots = mode == MODE_LOCK && !self->passphrase;
  gtk_widget_set_visible (self->dots, pin_dots);
  gtk_widget_set_visible (self->pass_entry, mode == MODE_MASTER || (mode == MODE_LOCK && self->passphrase));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->pass_entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  mode == MODE_MASTER ? "Master passphrase" : "Passphrase", -1);
  gtk_widget_set_visible (self->recovery_box, mode == MODE_RECOVERY);
  gtk_label_set_text (GTK_LABEL (self->prompt), mode_prompt (self));
  gtk_label_set_text (GTK_LABEL (self->hint), unlock_hint (self));

  /* The other ways in: the PIN unless blocked, the master passphrase if
   * there is one, and the recovery key. */
  Mode others[2];
  int n = 0;
  if (mode != MODE_LOCK && !jr_journal_pin_blocked (j))
    others[n++] = MODE_LOCK;
  if (mode != MODE_MASTER && jr_journal_has_master (j))
    others[n++] = MODE_MASTER;
  if (mode != MODE_RECOVERY && n < 2)
    others[n++] = MODE_RECOVERY;
  for (int i = 0; i < 2; i++)
    {
      gtk_widget_set_visible (self->links[i], i < n);
      if (i < n)
        {
          gtk_button_set_label (GTK_BUTTON (self->links[i]), link_label (self, others[i]));
          g_object_set_data (G_OBJECT (self->links[i]), "mode", GINT_TO_POINTER (others[i]));
        }
    }

  if (locked_out (self))
    start_countdown (self);
  else
    set_message (self, message != NULL ? message : self->reason);
  focus_input (self);
}

/* After 5 wrong PINs: the master passphrase, or the recovery key for a
 * PIN set before master passphrases existed. */
static void
show_blocked (JrLockView *self)
{
  gboolean master = jr_journal_has_master (journal (self));
  set_mode (self, master ? MODE_MASTER : MODE_RECOVERY,
            master ? "Too many wrong PINs: the PIN is blocked. Open the journal with your master "
                     "passphrase, then choose a new PIN."
                   : "Too many wrong PINs: the PIN is blocked. Open the journal with your recovery "
                     "key, then choose a new PIN.");
}

static void
verify_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  (void) source; (void) cancel;
  jr_unlock_attempt_run (data);
  g_task_return_boolean (task, TRUE);
}

static void
show_result (JrLockView *self, JrSecretKind kind, JrUnlockResult r)
{
  JrJournal *j = journal (self);
  char text[160];
  switch (r)
    {
    case JR_UNLOCK_OK:
      jr_window_unlocked (self->win, kind); /* replaces this view */
      return;
    case JR_UNLOCK_BLOCKED:
      show_blocked (self);
      return;
    case JR_UNLOCK_NO_DEVICE:
      if (jr_journal_has_master (j))
        set_mode (self, MODE_MASTER, "The PIN cannot be checked: this laptop's login keyring is not "
                                     "available or no longer has its key. Use your master passphrase.");
      else
        set_message (self, "The PIN cannot be checked: the login keyring is not available.");
      return;
    case JR_UNLOCK_WAIT:
      start_countdown (self);
      return;
    case JR_UNLOCK_WRONG:
    default:
      break;
    }
  if (kind == JR_SECRET_LOCK && !self->passphrase)
    {
      int left = jr_journal_pin_tries_left (j);
      g_snprintf (text, sizeof text, "Wrong PIN. %d %s left before the PIN is blocked.", left,
                  left == 1 ? "try" : "tries");
      set_message (self, text);
    }
  else if (locked_out (self))
    start_countdown (self);
  else
    {
      int left = jr_journal_tries_left (j);
      g_snprintf (text, sizeof text, "%s. %d %s left before a 30 second wait.",
                  kind == JR_SECRET_RECOVERY ? "That key does not match"
                  : kind == JR_SECRET_MASTER ? "Wrong master passphrase"
                                             : "Wrong passphrase",
                  left, left == 1 ? "try" : "tries");
      set_message (self, text);
    }
}

static void
on_verified (GObject *source, GAsyncResult *res, gpointer data)
{
  JrSecretKind kind = GPOINTER_TO_INT (data);
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
  show_result (self, kind, jr_journal_finish_unlock (journal (self), attempt));
}

/* Starts a check of `secret`; the caller wipes its own copy afterwards. */
static void
submit (JrLockView *self, JrSecretKind kind, const char *secret)
{
  JrUnlockResult refused = JR_UNLOCK_WRONG;
  JrUnlockAttempt *attempt = jr_journal_begin_unlock (journal (self), kind, secret, jr_now (), &refused);
  if (attempt == NULL)
    {
      show_result (self, kind, refused);
      return;
    }
  self->busy = TRUE;
  set_message (self, "Checking…");

  /* Hold the app so quitting waits for the worker to finish. */
  if (g_application_get_default () != NULL)
    g_application_hold (g_application_get_default ());
  GTask *task = g_task_new (self, NULL, on_verified, GINT_TO_POINTER (kind));
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
  if (self->mode != MODE_LOCK || self->passphrase || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) != 0)
    return FALSE; /* text fields take their own keys */

  guint32 ch = gdk_keyval_to_unicode (keyval);
  gboolean is_digit = ch >= '0' && ch <= '9';
  if (!is_digit && keyval != GDK_KEY_BackSpace && keyval != GDK_KEY_Escape)
    return FALSE;
  if (self->busy)
    return TRUE; /* swallow input while checking */

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
  submit (self, self->mode == MODE_MASTER ? JR_SECRET_MASTER : JR_SECRET_LOCK,
          gtk_editable_get_text (GTK_EDITABLE (entry)));
  jr_wipe_editable (GTK_EDITABLE (entry));
}

static void
on_link_clicked (GtkButton *b, gpointer data)
{
  JrLockView *self = data;
  if (!self->busy)
    set_mode (self, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "mode")), NULL);
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
  JrJournal *j = jr_window_get_journal (win);
  self->passphrase = jr_journal_lock_kind (j) == JR_LOCK_PASSPHRASE;

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

  self->prompt = jr_label ("", "muted", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (self->prompt), 0.5f);
  gtk_widget_set_margin_top (self->prompt, 8);
  gtk_box_append (GTK_BOX (center), self->prompt);
  self->dots = jr_pin_dots_new ();
  gtk_widget_set_margin_top (self->dots, 26);
  gtk_box_append (GTK_BOX (center), self->dots);
  /* Passphrases are typed into a hidden field (input methods do not learn it). */
  self->pass_entry = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (self->pass_entry), TRUE);
  gtk_widget_set_size_request (self->pass_entry, 340, -1);
  gtk_widget_set_margin_top (self->pass_entry, 22);
  g_signal_connect (self->pass_entry, "activate", G_CALLBACK (on_passphrase_activate), self);
  gtk_box_append (GTK_BOX (center), self->pass_entry);

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
  gtk_box_append (GTK_BOX (center), self->recovery_box);

  self->message = jr_label (self->reason, "muted", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (self->message), 0.5f);
  gtk_label_set_wrap (GTK_LABEL (self->message), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (self->message), 60);
  gtk_label_set_justify (GTK_LABEL (self->message), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_top (self->message, 26);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->message),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Lock status", -1);
  gtk_box_append (GTK_BOX (center), self->message);

  for (int i = 0; i < 2; i++)
    {
      self->links[i] = gtk_button_new_with_label ("");
      gtk_widget_add_css_class (self->links[i], "link-button");
      gtk_widget_set_halign (self->links[i], GTK_ALIGN_CENTER);
      gtk_widget_set_margin_top (self->links[i], i == 0 ? 14 : 2);
      gtk_widget_set_focus_on_click (self->links[i], FALSE);
      g_signal_connect (self->links[i], "clicked", G_CALLBACK (on_link_clicked), self);
      gtk_box_append (GTK_BOX (center), self->links[i]);
    }

  gtk_box_append (GTK_BOX (self), center);

  GtkWidget *status = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (status, "statusbar");
  self->hint = jr_label ("", NULL, NULL);
  gtk_widget_set_hexpand (self->hint, TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->hint), 0.5f);
  gtk_box_append (GTK_BOX (status), self->hint);
  gtk_box_append (GTK_BOX (self), status);

  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);

  if (jr_journal_pin_blocked (j))
    show_blocked (self);
  else
    set_mode (self, MODE_LOCK, NULL);

  *header_out = jr_header_new ("Journal");
  return GTK_WIDGET (self);
}
