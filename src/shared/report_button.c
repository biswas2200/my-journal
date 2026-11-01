/* report_button: see report_button.h. */
#include "report_button.h"

#include "openuri.h"

typedef struct {
  char        *part, *version;
  JrReportOpen open;
  gpointer     open_user;
} Report;

static const struct {
  JrReportKind kind;
  const char  *label;
} ITEMS[] = {
  { JR_REPORT_BUG, "Something is broken" },
  { JR_REPORT_UI, "Something looks wrong" },
  { JR_REPORT_IDEA, "An idea or improvement" },
};

static void
report_free (gpointer data)
{
  Report *r = data;
  g_free (r->part);
  g_free (r->version);
  g_free (r);
}

static gboolean
open_in_browser (const char *uri, gpointer user, GError **error)
{
  (void) user;
  return jr_open_uri (uri, error);
}

gboolean
jr_report_button_choose (GtkWidget *button, JrReportKind kind)
{
  Report *r = g_object_get_data (G_OBJECT (button), "jr-report");
  g_autofree char *gtk = g_strdup_printf ("GTK %u.%u.%u", gtk_get_major_version (), gtk_get_minor_version (),
                                          gtk_get_micro_version ());
  g_autofree char *system = jr_report_system (gtk);
  g_autofree char *url = jr_report_url (kind, r->part, r->version, system);
  g_autoptr (GError) error = NULL;
  if (r->open (url, r->open_user, &error))
    return TRUE;
  /* No browser: the address, ready to paste. */
  gdk_clipboard_set_text (gtk_widget_get_clipboard (button), url);
  GtkAlertDialog *d = gtk_alert_dialog_new ("Your web browser could not be opened");
  g_autofree char *detail = g_strdup_printf ("The report form's address is copied: paste it into your browser's "
                                             "address bar.\n\n(%s)", error != NULL ? error->message : "no reason given");
  gtk_alert_dialog_set_detail (d, detail);
  GtkRoot *root = gtk_widget_get_root (button);
  gtk_alert_dialog_show (d, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL);
  g_object_unref (d);
  return FALSE;
}

/* A menu item: closes the menu, then opens its form. */
static void
on_item (GtkButton *item, gpointer data)
{
  GtkWidget *button = data;
  gtk_menu_button_popdown (GTK_MENU_BUTTON (button));
  JrReportKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "jr-kind"));
  jr_report_button_choose (button, kind);
}

GtkWidget *
jr_report_button_new (const char *part, const char *version)
{
  GtkWidget *button = gtk_menu_button_new ();
  Report *r = g_new0 (Report, 1);
  r->part = g_strdup (part);
  r->version = g_strdup (version);
  r->open = open_in_browser;
  g_object_set_data_full (G_OBJECT (button), "jr-report", r, report_free);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (box, "report-menu");
  GtkWidget *line = gtk_label_new ("Opens a form on GitHub. Add what happened and screenshots there.");
  gtk_label_set_wrap (GTK_LABEL (line), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (line), 30);
  gtk_label_set_xalign (GTK_LABEL (line), 0.0f);
  gtk_widget_add_css_class (line, "dim-label");
  gtk_box_append (GTK_BOX (box), line);
  for (gsize i = 0; i < G_N_ELEMENTS (ITEMS); i++)
    {
      GtkWidget *item = gtk_button_new_with_label (ITEMS[i].label);
      gtk_widget_add_css_class (item, "flat");
      gtk_widget_set_halign (gtk_button_get_child (GTK_BUTTON (item)), GTK_ALIGN_START);
      g_object_set_data (G_OBJECT (item), "jr-kind", GINT_TO_POINTER (ITEMS[i].kind));
      g_signal_connect (item, "clicked", G_CALLBACK (on_item), button);
      gtk_box_append (GTK_BOX (box), item);
    }
  GtkWidget *pop = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), pop);
  gtk_menu_button_set_label (GTK_MENU_BUTTON (button), "\xe2\x9a\x91 Report");
  gtk_widget_set_tooltip_text (button, "Report a problem or suggest an idea");
  return button;
}

void
jr_report_button_set_opener (GtkWidget *button, JrReportOpen open, gpointer user)
{
  Report *r = g_object_get_data (G_OBJECT (button), "jr-report");
  r->open = open;
  r->open_user = user;
}
