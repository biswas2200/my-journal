/* secret_dialog: see secret_dialog.h. */
#include "secret_dialog.h"

#include <string.h>
#include "widgets.h"

typedef struct {
  JrSecretStep steps[JR_SECRET_DIALOG_MAX_STEPS];
  int          n_steps, step;
  char         digits[JR_PIN_LEN + 1];       /* the PIN step being typed */
  int          count;
  char        *secrets[JR_SECRET_DIALOG_MAX_STEPS]; /* finished steps */
  gboolean     busy;
  JrSecretDone done;
  gpointer     user;
  GtkWidget   *prompt, *dots, *entry, *message, *hint;
} SecretDialog;

static void
wipe_secrets (SecretDialog *d)
{
  for (int i = 0; i < JR_SECRET_DIALOG_MAX_STEPS; i++)
    g_clear_pointer (&d->secrets[i], jr_secret_free);
  memset (d->digits, 0, sizeof d->digits);
  d->count = 0;
}

static void
secret_dialog_free (gpointer data)
{
  SecretDialog *d = data;
  wipe_secrets (d);
  g_free (d);
}

static SecretDialog *
get (GtkWindow *dialog)
{
  return g_object_get_data (G_OBJECT (dialog), "jr-secret-dialog");
}

static void
show_step (SecretDialog *d)
{
  const JrSecretStep *s = &d->steps[d->step];
  gboolean pin = s->kind == JR_LOCK_PIN;
  gtk_label_set_text (GTK_LABEL (d->prompt), s->prompt);
  gtk_widget_set_visible (d->dots, pin);
  gtk_widget_set_visible (d->entry, !pin);
  jr_pin_dots_set (d->dots, d->count);
  if (pin)
    gtk_label_set_text (GTK_LABEL (d->hint), "Type the digits on your keyboard.");
  else if (s->is_new)
    gtk_label_set_text (GTK_LABEL (d->hint),
                        "At least 12 characters. A few random words work well. Press Enter.");
  else
    gtk_label_set_text (GTK_LABEL (d->hint), "Press Enter when done.");
  if (!pin)
    gtk_widget_grab_focus (d->entry);
}

void
jr_secret_dialog_restart (GtkWindow *dialog, const char *error)
{
  SecretDialog *d = get (dialog);
  wipe_secrets (d);
  d->step = 0;
  d->busy = FALSE;
  jr_wipe_editable (GTK_EDITABLE (d->entry));
  gtk_widget_set_sensitive (d->entry, TRUE);
  gtk_label_set_text (GTK_LABEL (d->message), error ? error : "");
  show_step (d);
}

void
jr_secret_dialog_set_busy (GtkWindow *dialog, const char *message)
{
  SecretDialog *d = get (dialog);
  d->busy = TRUE;
  gtk_widget_set_sensitive (d->entry, FALSE);
  gtk_label_set_text (GTK_LABEL (d->message), message);
}

/* Stores the finished step; hands everything over after the last one. */
static void
step_done (GtkWindow *dialog, SecretDialog *d, const char *secret)
{
  const JrSecretStep *s = &d->steps[d->step];
  if (s->is_new)
    {
      char *ok = jr_lock_secret_normalize (s->kind, secret);
      if (ok == NULL)
        {
          gtk_label_set_text (GTK_LABEL (d->message),
                              s->kind == JR_LOCK_PIN ? "A PIN is exactly 6 digits."
                                                     : "Use at least 12 characters, not only spaces.");
          return;
        }
      jr_secret_free (ok);
    }
  d->secrets[d->step] = g_strdup (secret);
  memset (d->digits, 0, sizeof d->digits);
  d->count = 0;
  jr_wipe_editable (GTK_EDITABLE (d->entry));

  if (++d->step < d->n_steps)
    {
      gtk_label_set_text (GTK_LABEL (d->message), "");
      show_step (d);
      return;
    }

  /* All steps done. Keep the dialog alive while `done` runs, even if it
   * destroys it, then wipe the secrets. */
  d->step = d->n_steps - 1;
  const char *secrets[JR_SECRET_DIALOG_MAX_STEPS + 1] = { NULL };
  for (int i = 0; i < d->n_steps; i++)
    secrets[i] = d->secrets[i];
  g_object_ref (dialog);
  d->done (dialog, secrets, d->user);
  wipe_secrets (d);
  g_object_unref (dialog);
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state,
        gpointer data)
{
  (void) c; (void) keycode;
  GtkWindow *dialog = data;
  SecretDialog *d = get (dialog);
  if (d->steps[d->step].kind != JR_LOCK_PIN || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) != 0)
    return FALSE; /* passphrase steps type into the entry */
  if (d->busy)
    return TRUE;
  if (keyval == GDK_KEY_BackSpace)
    {
      if (d->count > 0)
        d->digits[--d->count] = '\0';
      jr_pin_dots_set (d->dots, d->count);
      return TRUE;
    }
  guint32 ch = gdk_keyval_to_unicode (keyval);
  if (ch < '0' || ch > '9')
    return FALSE; /* Escape etc. go on to the window */
  d->digits[d->count++] = (char) ch;
  jr_pin_dots_set (d->dots, d->count);
  if (d->count == JR_PIN_LEN)
    step_done (dialog, d, d->digits);
  return TRUE;
}

static void
on_entry_activate (GtkWidget *entry, gpointer data)
{
  GtkWindow *dialog = data;
  SecretDialog *d = get (dialog);
  if (!d->busy)
    step_done (dialog, d, gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_cancel (GtkButton *b, gpointer data)
{
  (void) b;
  gtk_window_destroy (GTK_WINDOW (data));
}

static GtkWindow *
new_modal (JrWindow *parent, const char *title)
{
  GtkWindow *w = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_transient_for (w, GTK_WINDOW (parent));
  gtk_window_set_modal (w, TRUE);
  gtk_window_set_destroy_with_parent (w, TRUE);
  gtk_window_set_resizable (w, FALSE);
  gtk_window_set_default_size (w, 460, -1);
  gtk_window_set_title (w, title);
  gtk_window_set_titlebar (w, jr_header_new (title));

  /* Esc closes. */
  GtkEventController *sc = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_add_shortcut (
    GTK_SHORTCUT_CONTROLLER (sc),
    gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                      gtk_named_action_new ("window.close")));
  gtk_widget_add_controller (GTK_WIDGET (w), sc);
  return w;
}

static GtkWidget *
centered (GtkWidget *label)
{
  gtk_label_set_xalign (GTK_LABEL (label), 0.5f);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  return label;
}

GtkWindow *
jr_secret_dialog_new (JrWindow *parent, const char *title, const JrSecretStep *steps,
                      int n_steps, JrSecretDone done, gpointer user)
{
  GtkWindow *dialog = new_modal (parent, title);
  SecretDialog *d = g_new0 (SecretDialog, 1);
  d->n_steps = CLAMP (n_steps, 1, JR_SECRET_DIALOG_MAX_STEPS);
  memcpy (d->steps, steps, sizeof (JrSecretStep) * (gsize) d->n_steps);
  d->done = done;
  d->user = user;
  g_object_set_data_full (G_OBJECT (dialog), "jr-secret-dialog", d, secret_dialog_free);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (box, 28);
  gtk_widget_set_margin_bottom (box, 20);
  gtk_widget_set_margin_start (box, 28);
  gtk_widget_set_margin_end (box, 28);
  d->prompt = centered (jr_label ("", "dialog-title", NULL));
  gtk_box_append (GTK_BOX (box), d->prompt);

  d->dots = jr_pin_dots_new ();
  gtk_widget_set_margin_top (d->dots, 22);
  gtk_box_append (GTK_BOX (box), d->dots);

  /* GtkPasswordEntry hides the text and tells input methods not to learn it. */
  d->entry = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (d->entry), TRUE);
  gtk_widget_set_margin_top (d->entry, 18);
  gtk_accessible_update_property (GTK_ACCESSIBLE (d->entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Passphrase", -1);
  g_signal_connect (d->entry, "activate", G_CALLBACK (on_entry_activate), dialog);
  gtk_box_append (GTK_BOX (box), d->entry);

  d->message = centered (jr_label ("", "muted", "mono", NULL));
  gtk_widget_set_margin_top (d->message, 18);
  gtk_box_append (GTK_BOX (box), d->message);
  d->hint = centered (jr_label ("", "faint", "mono", NULL));
  gtk_widget_set_margin_top (d->hint, 8);
  gtk_box_append (GTK_BOX (box), d->hint);

  GtkWidget *cancel = gtk_button_new_with_label ("Cancel");
  gtk_widget_set_halign (cancel, GTK_ALIGN_END);
  gtk_widget_set_margin_top (cancel, 20);
  gtk_widget_set_focus_on_click (cancel, FALSE);
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), dialog);
  gtk_box_append (GTK_BOX (box), cancel);
  gtk_window_set_child (dialog, box);

  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), dialog);
  gtk_widget_add_controller (GTK_WIDGET (dialog), keys);

  show_step (d);
  jr_window_set_dialog (parent, dialog);
  gtk_window_present (dialog);
  return dialog;
}

/* ---- PIN or passphrase ----------------------------------------------- */

static void
on_kind_clicked (GtkButton *b, gpointer data)
{
  GtkWindow *dialog = data;
  JrWindow *win = JR_WINDOW (gtk_window_get_transient_for (dialog));
  JrKindChosen chosen = (JrKindChosen) (void (*) (void)) g_object_get_data (G_OBJECT (dialog), "chosen");
  JrLockKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "kind"));
  gtk_window_destroy (dialog);
  chosen (win, kind);
}

static GtkWidget *
kind_button (GtkWindow *dialog, JrLockKind kind, const char *title, const char *detail)
{
  GtkWidget *b = gtk_button_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_box_append (GTK_BOX (box), jr_label (title, "card-title", NULL));
  GtkWidget *d = jr_label (detail, "card-sub", NULL);
  gtk_label_set_wrap (GTK_LABEL (d), TRUE);
  gtk_box_append (GTK_BOX (box), d);
  gtk_button_set_child (GTK_BUTTON (b), box);
  g_object_set_data (G_OBJECT (b), "kind", GINT_TO_POINTER (kind));
  g_signal_connect (b, "clicked", G_CALLBACK (on_kind_clicked), dialog);
  return b;
}

void
jr_lock_kind_dialog_show (JrWindow *parent, JrKindChosen chosen)
{
  GtkWindow *dialog = new_modal (parent, "Lock the journal");
  g_object_set_data (G_OBJECT (dialog), "chosen", (gpointer) (void (*) (void)) chosen);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (box, 24);
  gtk_widget_set_margin_bottom (box, 20);
  gtk_widget_set_margin_start (box, 28);
  gtk_widget_set_margin_end (box, 28);
  gtk_box_append (GTK_BOX (box), jr_label ("How do you want to unlock it?", "dialog-title", NULL));
  gtk_box_append (GTK_BOX (box),
                  kind_button (dialog, JR_LOCK_PASSPHRASE, "Passphrase (recommended)",
                               "12 or more characters, typed and Enter. Too many possibilities "
                               "to guess, even if someone copies the file."));
  gtk_box_append (GTK_BOX (box),
                  kind_button (dialog, JR_LOCK_PIN, "6-digit PIN",
                               "Quick to type. Stops casual snooping, but someone with a copy "
                               "of the file could guess it in about a day."));
  GtkWidget *cancel = gtk_button_new_with_label ("Cancel");
  gtk_widget_set_halign (cancel, GTK_ALIGN_END);
  gtk_widget_set_margin_top (cancel, 8);
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), dialog);
  gtk_box_append (GTK_BOX (box), cancel);
  gtk_window_set_child (dialog, box);
  jr_window_set_dialog (parent, dialog);
  gtk_window_present (dialog);
}

/* ---- recovery key ---------------------------------------------------- */

static void
on_saved (GtkButton *b, gpointer data)
{
  (void) b;
  gtk_window_destroy (GTK_WINDOW (data));
}

static void
on_recovery_destroy (GtkWidget *w, gpointer data)
{
  (void) w;
  gtk_label_set_text (GTK_LABEL (data), "");
}

void
jr_recovery_dialog_show (JrWindow *parent, const char *key)
{
  GtkWindow *dialog = new_modal (parent, "Recovery key");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_margin_top (box, 24);
  gtk_widget_set_margin_bottom (box, 20);
  gtk_widget_set_margin_start (box, 28);
  gtk_widget_set_margin_end (box, 28);

  gtk_box_append (GTK_BOX (box), jr_label ("Your recovery key", "dialog-title", NULL));
  GtkWidget *text = jr_label ("Write it down on paper and keep it somewhere safe. It is shown "
                              "only this once and cannot be copied. If you forget your PIN or "
                              "passphrase, this key opens your entries. Without either, nobody "
                              "can read them.",
                              "muted", "mono", NULL);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (text), 52);
  gtk_box_append (GTK_BOX (box), text);

  /* Not selectable, so it never reaches the clipboard. */
  GtkWidget *label = jr_label (key, "recovery-key", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5f);
  gtk_label_set_selectable (GTK_LABEL (label), FALSE);
  gtk_widget_set_margin_top (label, 8);
  gtk_widget_set_margin_bottom (label, 8);
  gtk_box_append (GTK_BOX (box), label);

  GtkWidget *saved = gtk_button_new_with_label ("I have written it down");
  gtk_widget_add_css_class (saved, "primary");
  gtk_widget_set_halign (saved, GTK_ALIGN_END);
  g_signal_connect (saved, "clicked", G_CALLBACK (on_saved), dialog);
  gtk_box_append (GTK_BOX (box), saved);
  gtk_window_set_child (dialog, box);
  /* The window drops its children before "destroy", so keep the label
   * alive until the handler has blanked it. */
  g_signal_connect_data (dialog, "destroy", G_CALLBACK (on_recovery_destroy),
                         g_object_ref (label), (GClosureNotify) (void (*) (void)) g_object_unref, 0);

  jr_window_set_dialog (parent, dialog);
  gtk_window_present (dialog);
  gtk_widget_grab_focus (saved);
}
