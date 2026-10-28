/* journal-notes: study notes.
 *
 * A separate program from the Journal: it shares no code that touches
 * journal data and is started by the launcher (`journal`, see
 * programs.h). This is the empty shell; subjects, the editor and the AI
 * enhanced copy come in later steps. */
#include <glib-unix.h>
#include <gtk/gtk.h>
#include "programs.h"

#define APP_ID "io.github.biswas2200.Journal.Notes"
#define ICON   "io.github.biswas2200.Journal"

static int exit_code = JR_EXIT_QUIT;

static const char CSS[] =
  "window, .background { background: #111111; color: #EDEDED; }"
  "headerbar { background: #1A1A1A; box-shadow: none; border-bottom: 1px solid #2A2A2A; }"
  ".title-text { font-family: monospace; color: #CFCFCF; }"
  "button.nav { background: #1C1C1C; color: #EDEDED; border: 1px solid #333333; box-shadow: none; }"
  "button.nav:hover { border-color: #5C5C5C; }"
  ".soon { font-family: serif; font-size: 18px; }"
  ".faint { color: #808080; font-family: monospace; }";

static void
leave (GtkWidget *button, int code)
{
  exit_code = code;
  gtk_window_close (GTK_WINDOW (gtk_widget_get_root (button)));
}

static void
on_home (GtkButton *b, gpointer data)
{
  (void) data;
  leave (GTK_WIDGET (b), JR_EXIT_HOME);
}

static void
on_journal (GtkButton *b, gpointer data)
{
  (void) data;
  leave (GTK_WIDGET (b), JR_EXIT_TO_DIARY);
}

static GtkWidget *
label (const char *text, const char *css)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (l, css);
  return l;
}

static void
on_activate (GApplication *app)
{
  GtkWindow *existing = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (existing != NULL)
    {
      gtk_window_present (existing);
      return;
    }
  GtkSettings *settings = gtk_settings_get_default ();
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (settings), "gtk-interface-color-scheme"))
    g_object_set (settings, "gtk-interface-color-scheme", 3 /* dark */, NULL);
  else
    g_object_set (settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css, CSS);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
  gtk_window_set_default_icon_name (ICON);

  GtkWidget *win = gtk_application_window_new (GTK_APPLICATION (app));
  gtk_window_set_title (GTK_WINDOW (win), "Notes");
  gtk_window_set_default_size (GTK_WINDOW (win), 960, 600);

  GtkWidget *header = gtk_header_bar_new ();
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  if (g_strcmp0 (g_getenv ("JOURNAL_LAUNCHER"), "1") == 0)
    {
      GtkWidget *home = gtk_button_new_with_label ("Home");
      g_signal_connect (home, "clicked", G_CALLBACK (on_home), NULL);
      GtkWidget *journal = gtk_button_new_with_label ("Journal");
      gtk_widget_set_tooltip_text (journal, "Switch to the Journal. It asks for your PIN.");
      g_signal_connect (journal, "clicked", G_CALLBACK (on_journal), NULL);
      gtk_widget_add_css_class (home, "nav");
      gtk_widget_add_css_class (journal, "nav");
      gtk_header_bar_pack_start (GTK_HEADER_BAR (header), home);
      gtk_header_bar_pack_start (GTK_HEADER_BAR (header), journal);
    }
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), label ("Notes", "title-text"));
  gtk_window_set_titlebar (GTK_WINDOW (win), header);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), label ("Notes is on its way.", "soon"));
  gtk_box_append (GTK_BOX (box), label ("Subjects, the editor and the AI enhanced copy come next.", "faint"));
  gtk_window_set_child (GTK_WINDOW (win), box);
  gtk_window_present (GTK_WINDOW (win));
}

/* SIGTERM / SIGINT (logout, Ctrl+C): close the window. */
static gboolean
on_signal (gpointer app)
{
  GtkWindow *w = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (w == NULL)
    {
      GList *all = gtk_application_get_windows (GTK_APPLICATION (app));
      w = all != NULL ? all->data : NULL;
    }
  if (w != NULL)
    gtk_window_close (w);
  else
    g_application_quit (G_APPLICATION (app));
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  g_setenv ("GSK_RENDERER", "cairo", FALSE);
  GtkApplication *app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_unix_signal_add (SIGTERM, on_signal, app);
  g_unix_signal_add (SIGINT, on_signal, app);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status != 0 ? status : exit_code;
}
