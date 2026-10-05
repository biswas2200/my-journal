/* widgets: see widgets.h. */
#include "widgets.h"

#include <stdarg.h>
#include <string.h>

GtkWidget *
jr_label (const char *text, const char *first_class, ...)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  va_list ap;
  va_start (ap, first_class);
  for (const char *c = first_class; c != NULL; c = va_arg (ap, const char *))
    gtk_widget_add_css_class (label, c);
  va_end (ap);
  return label;
}

GtkWidget *
jr_button (const char *icon_name, const char *text)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  if (icon_name != NULL)
    gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name (icon_name));
  if (text != NULL)
    gtk_box_append (GTK_BOX (box), gtk_label_new (text));
  gtk_button_set_child (GTK_BUTTON (button), box);
  return button;
}

GtkWidget *
jr_pin_dots_new (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (box, "pin-dots");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  for (int i = 0; i < 6; i++)
    {
      GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (dot, "pin-dot");
      gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), dot);
    }
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "PIN digits entered: 0 of 6", -1);
  return box;
}

void
jr_pin_dots_set (GtkWidget *dots, int n)
{
  int i = 0;
  for (GtkWidget *d = gtk_widget_get_first_child (dots); d != NULL;
       d = gtk_widget_get_next_sibling (d), i++)
    {
      if (i < n)
        gtk_widget_add_css_class (d, "filled");
      else
        gtk_widget_remove_css_class (d, "filled");
    }
  char text[48];
  g_snprintf (text, sizeof text, "PIN digits entered: %d of 6", n);
  gtk_accessible_update_property (GTK_ACCESSIBLE (dots), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
}

GtkWidget *
jr_stat_new (const char *title, GtkWidget **value_out)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *t = jr_label (title, "stat-title", NULL);
  GtkWidget *v = jr_label ("", "stat-value", NULL);
  gtk_label_set_xalign (GTK_LABEL (t), 1.0f);
  gtk_label_set_xalign (GTK_LABEL (v), 1.0f);
  gtk_box_append (GTK_BOX (box), t);
  gtk_box_append (GTK_BOX (box), v);
  *value_out = v;
  return box;
}

GtkWidget *
jr_header_new (const char *title)
{
  GtkWidget *header = gtk_header_bar_new ();
  /* An empty box as title widget keeps the centre free; titles sit at the start. */
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  if (title != NULL)
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), jr_label (title, "title-text", NULL));
  return header;
}

GtkWidget *
jr_header_sep (void)
{
  GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (sep, "vsep");
  return sep;
}

void
jr_wipe_editable (GtkEditable *editable)
{
  /* Overwrite with spaces first so the old buffer's bytes are replaced
   * in place where GTK reuses it, then clear. */
  const char *text = gtk_editable_get_text (editable);
  gsize n = text ? strlen (text) : 0;
  if (n > 0)
    {
      char *blank = g_malloc (n + 1);
      memset (blank, ' ', n);
      blank[n] = '\0';
      gtk_editable_set_text (editable, blank);
      g_free (blank);
    }
  gtk_editable_set_text (editable, "");
}
