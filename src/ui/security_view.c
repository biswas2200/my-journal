/* security_view: see security_view.h.
 *
 * Every change that weakens or reveals the lock (turning the PIN off,
 * changing it, making a new recovery key) asks for the current PIN first,
 * so someone at an unlocked laptop cannot take over the journal. */
#include "security_view.h"

#include <string.h>
#include "lockout.h"
#include "pin_dialog.h"
#include "widgets.h"

/* Dialog callbacks only need the window: the view may be rebuilt meanwhile. */

/* Checks the current PIN; on failure restarts the dialog with a reason. */
static gboolean
check_current_pin (JrWindow *win, GtkWindow *dialog, const char *pin)
{
  JrJournal *j = jr_window_get_journal (win);
  gint64 now = jr_now ();
  char msg[96];
  switch (jr_journal_unlock_pin (j, pin, now))
    {
    case JR_UNLOCK_OK:
      return TRUE;
    case JR_UNLOCK_WRONG:
      if (jr_journal_lockout_remaining (j, now) > 0)
        g_snprintf (msg, sizeof msg, "Too many wrong tries. Wait %d seconds.", JR_LOCKOUT_SECS);
      else
        g_snprintf (msg, sizeof msg, "Wrong PIN. %d tries left.", jr_journal_tries_left (j));
      break;
    case JR_UNLOCK_WAIT:
    default:
      g_snprintf (msg, sizeof msg, "Too many wrong tries. Wait %" G_GINT64_FORMAT " seconds.",
                  jr_journal_lockout_remaining (j, now));
      break;
    }
  jr_pin_dialog_restart (dialog, msg);
  return FALSE;
}

static void
finish (JrWindow *win, GtkWindow *dialog, char *recovery)
{
  gtk_window_destroy (dialog);
  jr_window_settings_changed (win);
  jr_window_show_security (win); /* rebuild with the new state */
  if (recovery != NULL)
    {
      jr_recovery_dialog_show (win, recovery);
      jr_secret_free (recovery);
    }
}

static void
done_enable (GtkWindow *dialog, const char *const *pins, gpointer user)
{
  JrWindow *win = user;
  if (strcmp (pins[0], pins[1]) != 0)
    {
      jr_pin_dialog_restart (dialog, "The two PINs did not match. Try again.");
      return;
    }
  char *recovery = jr_journal_enable_pin (jr_window_get_journal (win), pins[0]);
  if (recovery == NULL)
    {
      jr_pin_dialog_restart (dialog, "Could not save the PIN. Try again.");
      return;
    }
  finish (win, dialog, recovery);
}

static void
done_disable (GtkWindow *dialog, const char *const *pins, gpointer user)
{
  JrWindow *win = user;
  if (!check_current_pin (win, dialog, pins[0]))
    return;
  jr_journal_disable_pin (jr_window_get_journal (win));
  finish (win, dialog, NULL);
}

static void
done_change (GtkWindow *dialog, const char *const *pins, gpointer user)
{
  JrWindow *win = user;
  if (strcmp (pins[1], pins[2]) != 0)
    {
      jr_pin_dialog_restart (dialog, "The new PINs did not match. Start again.");
      return;
    }
  if (!check_current_pin (win, dialog, pins[0]))
    return;
  if (!jr_journal_change_pin (jr_window_get_journal (win), pins[1]))
    {
      jr_pin_dialog_restart (dialog, "Could not save the PIN. Try again.");
      return;
    }
  finish (win, dialog, NULL);
}

static void
done_new_key (GtkWindow *dialog, const char *const *pins, gpointer user)
{
  JrWindow *win = user;
  if (!check_current_pin (win, dialog, pins[0]))
    return;
  char *recovery = jr_journal_new_recovery_key (jr_window_get_journal (win));
  if (recovery == NULL)
    {
      jr_pin_dialog_restart (dialog, "Could not make a new key. Try again.");
      return;
    }
  finish (win, dialog, recovery);
}

/* The PIN toggle only opens a dialog; its look follows the saved state
 * when the screen is rebuilt after the dialog succeeds. */
static void
on_toggle (GtkButton *b, gpointer data)
{
  (void) b;
  JrWindow *win = data;
  if (!jr_journal_pin_enabled (jr_window_get_journal (win)))
    {
      static const char *const p[] = { "Choose a 6-digit PIN", "Type the same PIN again", NULL };
      jr_pin_dialog_new (win, "Set a PIN", p, done_enable, win);
    }
  else
    {
      static const char *const p[] = { "Enter your current PIN to turn it off", NULL };
      jr_pin_dialog_new (win, "Turn off the PIN", p, done_disable, win);
    }
}

/* A switch drawn with a button and a knob. (GtkSwitch in GTK 4.22 logs a
 * baseline warning on every layout, and this is lighter anyway.) */
static GtkWidget *
toggle_new (gboolean on, const char *label)
{
  GtkWidget *b = gtk_button_new ();
  gtk_widget_add_css_class (b, "toggle");
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

static void
on_change_pin (GtkButton *b, gpointer data)
{
  (void) b;
  static const char *const p[] = { "Enter your current PIN", "Choose a new 6-digit PIN",
                                   "Type the new PIN again", NULL };
  jr_pin_dialog_new (data, "Change PIN", p, done_change, data);
}

static void
on_new_key (GtkButton *b, gpointer data)
{
  (void) b;
  static const char *const p[] = { "Enter your PIN to make a new recovery key", NULL };
  jr_pin_dialog_new (data, "New recovery key", p, done_new_key, data);
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

GtkWidget *
jr_security_view_new (JrWindow *win, GtkWidget **header_out)
{
  JrJournal *j = jr_window_get_journal (win);
  gboolean pin_on = jr_journal_pin_enabled (j);

  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_size_request (column, 520, -1);
  gtk_widget_set_halign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (column, 24);
  gtk_widget_set_margin_bottom (column, 24);
  gtk_widget_set_margin_start (column, 24);
  gtk_widget_set_margin_end (column, 24);

  /* PIN on/off. */
  GtkWidget *c1 = card ();
  gtk_box_append (GTK_BOX (c1), titled ("Lock the journal with a PIN",
                                        "Asked every time you open the app"));
  GtkWidget *sw = toggle_new (pin_on, "Lock the journal with a PIN");
  g_signal_connect (sw, "clicked", G_CALLBACK (on_toggle), win);
  gtk_box_append (GTK_BOX (c1), sw);
  gtk_box_append (GTK_BOX (column), c1);

  if (pin_on)
    {
      GtkWidget *c2 = card ();
      GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
      gtk_widget_set_hexpand (left, TRUE);
      gtk_box_append (GTK_BOX (left), jr_label ("PIN", "card-title", "mono", NULL));
      GtkWidget *dots = jr_pin_dots_new ();
      gtk_widget_add_css_class (dots, "small");
      gtk_widget_set_halign (dots, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (left), dots);
      gtk_box_append (GTK_BOX (c2), left);
      GtkWidget *change = gtk_button_new_with_label ("Change PIN");
      gtk_widget_set_valign (change, GTK_ALIGN_CENTER);
      g_signal_connect (change, "clicked", G_CALLBACK (on_change_pin), win);
      gtk_box_append (GTK_BOX (c2), change);
      gtk_box_append (GTK_BOX (column), c2);
    }

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
  gtk_widget_set_sensitive (sleep_check, pin_on);
  g_signal_connect (sleep_check, "toggled", G_CALLBACK (on_sleep_toggled), win);
  gtk_box_append (GTK_BOX (c3), sleep_check);
  gtk_box_append (GTK_BOX (column), c3);

  if (pin_on)
    {
      GtkWidget *c4 = card ();
      gtk_box_append (GTK_BOX (c4), titled ("Recovery key",
                                            "Shown once when you set the PIN. If you forget the "
                                            "PIN, this key opens your entries."));
      GtkWidget *mk = gtk_button_new_with_label ("Make a new key");
      gtk_widget_set_valign (mk, GTK_ALIGN_CENTER);
      g_signal_connect (mk, "clicked", G_CALLBACK (on_new_key), win);
      gtk_box_append (GTK_BOX (c4), mk);
      gtk_box_append (GTK_BOX (column), c4);
    }

  GtkWidget *note = jr_label ("After 5 wrong PINs the app waits 30 seconds before the next try. "
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
  if (pin_on)
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
