/* pin_dialog: see pin_dialog.h. */
#include "pin_dialog.h"

#include <string.h>
#include "vault.h"
#include "widgets.h"

#define MAX_STEPS 3

typedef struct {
  char        pins[MAX_STEPS][JR_PIN_LEN + 1];
  const char *prompts[MAX_STEPS];
  int         steps, step, count;
  JrPinDone   done;
  gpointer    user;
  GtkWidget  *prompt, *dots, *message;
} PinDialog;

static void
pin_dialog_free (gpointer data)
{
  PinDialog *d = data;
  memset (d->pins, 0, sizeof d->pins);
  g_free (d);
}

static PinDialog *
get (GtkWindow *dialog)
{
  return g_object_get_data (G_OBJECT (dialog), "jr-pin-dialog");
}

static void
show_step (PinDialog *d)
{
  gtk_label_set_text (GTK_LABEL (d->prompt), d->prompts[d->step]);
  jr_pin_dots_set (d->dots, d->count);
}

void
jr_pin_dialog_restart (GtkWindow *dialog, const char *error)
{
  PinDialog *d = get (dialog);
  memset (d->pins, 0, sizeof d->pins);
  d->step = d->count = 0;
  gtk_label_set_text (GTK_LABEL (d->message), error ? error : "");
  show_step (d);
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state,
        gpointer data)
{
  (void) c; (void) keycode;
  GtkWindow *dialog = data;
  PinDialog *d = get (dialog);
  if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) != 0)
    return FALSE;
  if (keyval == GDK_KEY_BackSpace)
    {
      if (d->count > 0)
        d->pins[d->step][--d->count] = '\0';
      show_step (d);
      return TRUE;
    }
  guint32 ch = gdk_keyval_to_unicode (keyval);
  if (ch < '0' || ch > '9')
    return FALSE; /* Escape etc. go on to the window */

  d->pins[d->step][d->count++] = (char) ch;
  if (d->count < JR_PIN_LEN)
    {
      show_step (d);
      return TRUE;
    }
  d->count = 0;
  if (++d->step < d->steps)
    {
      gtk_label_set_text (GTK_LABEL (d->message), "");
      show_step (d);
      return TRUE;
    }

  /* All steps done. Keep the dialog alive while `done` runs, even if it
   * destroys it, then wipe the PINs. */
  jr_pin_dots_set (d->dots, JR_PIN_LEN);
  const char *pins[MAX_STEPS + 1] = { NULL };
  for (int i = 0; i < d->steps; i++)
    pins[i] = d->pins[i];
  g_object_ref (dialog);
  d->done (dialog, pins, d->user);
  memset (d->pins, 0, sizeof d->pins);
  g_object_unref (dialog);
  return TRUE;
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
  gtk_window_set_default_size (w, 440, -1);
  gtk_window_set_title (w, title);
  gtk_window_set_titlebar (w, jr_header_new (title));
  return w;
}

GtkWindow *
jr_pin_dialog_new (JrWindow *parent, const char *title, const char *const *prompts,
                   JrPinDone done, gpointer user)
{
  GtkWindow *dialog = new_modal (parent, title);
  PinDialog *d = g_new0 (PinDialog, 1);
  for (d->steps = 0; d->steps < MAX_STEPS && prompts[d->steps] != NULL; d->steps++)
    d->prompts[d->steps] = prompts[d->steps];
  d->done = done;
  d->user = user;
  g_object_set_data_full (G_OBJECT (dialog), "jr-pin-dialog", d, pin_dialog_free);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (box, 28);
  gtk_widget_set_margin_bottom (box, 20);
  gtk_widget_set_margin_start (box, 28);
  gtk_widget_set_margin_end (box, 28);
  d->prompt = jr_label ("", "dialog-title", NULL);
  gtk_label_set_xalign (GTK_LABEL (d->prompt), 0.5f);
  gtk_box_append (GTK_BOX (box), d->prompt);
  d->dots = jr_pin_dots_new ();
  gtk_widget_set_margin_top (d->dots, 22);
  gtk_box_append (GTK_BOX (box), d->dots);
  d->message = jr_label ("", "muted", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (d->message), 0.5f);
  gtk_label_set_wrap (GTK_LABEL (d->message), TRUE);
  gtk_label_set_justify (GTK_LABEL (d->message), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_top (d->message, 18);
  gtk_box_append (GTK_BOX (box), d->message);
  GtkWidget *hint = jr_label ("Type the digits on your keyboard.", "faint", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.5f);
  gtk_widget_set_margin_top (hint, 8);
  gtk_box_append (GTK_BOX (box), hint);
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

  /* Esc closes. */
  GtkEventController *sc = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_add_shortcut (
    GTK_SHORTCUT_CONTROLLER (sc),
    gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                      gtk_named_action_new ("window.close")));
  gtk_widget_add_controller (GTK_WIDGET (dialog), sc);

  show_step (d);
  jr_window_set_dialog (parent, dialog);
  gtk_window_present (dialog);
  return dialog;
}

/* ---- recovery key ---------------------------------------------------- */

static void
on_copy (GtkButton *b, gpointer data)
{
  GtkLabel *key = data;
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (b)), gtk_label_get_text (key));
  gtk_button_set_label (b, "Copied");
}

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

  GtkWidget *title = jr_label ("Your recovery key", "dialog-title", NULL);
  gtk_box_append (GTK_BOX (box), title);
  GtkWidget *text = jr_label ("Write it down and keep it somewhere safe. It is shown only this "
                              "once. If you forget your PIN, this key opens your entries. Without "
                              "the PIN or this key, the entries cannot be read by anyone.",
                              "muted", "mono", NULL);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (text), 52);
  gtk_box_append (GTK_BOX (box), text);

  GtkWidget *label = jr_label (key, "recovery-key", "mono", NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5f);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_set_margin_top (label, 8);
  gtk_widget_set_margin_bottom (label, 8);
  gtk_box_append (GTK_BOX (box), label);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  GtkWidget *copy = gtk_button_new_with_label ("Copy");
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy), label);
  GtkWidget *saved = gtk_button_new_with_label ("I have saved it");
  gtk_widget_add_css_class (saved, "primary");
  g_signal_connect (saved, "clicked", G_CALLBACK (on_saved), dialog);
  gtk_box_append (GTK_BOX (buttons), copy);
  gtk_box_append (GTK_BOX (buttons), saved);
  gtk_box_append (GTK_BOX (box), buttons);
  gtk_window_set_child (dialog, box);
  /* The window drops its children before "destroy", so keep the label
   * alive until the handler has blanked it. */
  g_signal_connect_data (dialog, "destroy", G_CALLBACK (on_recovery_destroy),
                         g_object_ref (label), (GClosureNotify) (void (*) (void)) g_object_unref, 0);

  jr_window_set_dialog (parent, dialog);
  gtk_window_present (dialog);
  gtk_widget_grab_focus (saved);
}
