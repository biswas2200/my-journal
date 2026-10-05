/* Journal: a private daily journal for one person.
 *
 * Entry point: sets up GTK, the theme and the journal file, then hands
 * everything to the window. */
#include <glib-unix.h>
#include <gtk/gtk.h>
#include "journal.h"
#include "window.h"

#define APP_ID "io.github.biswas2200.Journal"

static void
load_theme (void)
{
  GtkSettings *settings = gtk_settings_get_default ();
  /* Dark base theme so anything we do not style is dark too. */
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (settings), "gtk-interface-color-scheme"))
    g_object_set (settings, "gtk-interface-color-scheme", 3 /* GTK_INTERFACE_COLOR_SCHEME_DARK */, NULL);
  else
    g_object_set (settings, "gtk-application-prefer-dark-theme", TRUE, NULL);

  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/io/github/biswas2200/Journal/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
}

/* $JOURNAL_DB overrides the file (handy for trying things out). */
static char *
database_path (void)
{
  const char *env = g_getenv ("JOURNAL_DB");
  if (env != NULL && *env != '\0')
    return g_strdup (env);
  return g_build_filename (g_get_user_data_dir (), "journal", "journal.db", NULL);
}

static void
on_error_closed (GObject *src, GAsyncResult *res, gpointer app)
{
  gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (src), res, NULL);
  g_application_release (app);
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

  load_theme ();
  gtk_window_set_default_icon_name (APP_ID);
  g_autofree char *path = database_path ();
  GError *error = NULL;
  JrJournal *journal = jr_journal_open (path, &error);
  if (journal == NULL)
    {
      GtkAlertDialog *alert = gtk_alert_dialog_new ("The journal could not be opened");
      gtk_alert_dialog_set_detail (alert, error->message);
      g_application_hold (app);
      gtk_alert_dialog_choose (alert, NULL, NULL, on_error_closed, app);
      g_object_unref (alert);
      g_error_free (error);
      return;
    }
  gtk_window_present (GTK_WINDOW (jr_window_new (GTK_APPLICATION (app), journal)));
}

/* SIGTERM / SIGINT (logout, Ctrl+C): close windows so everything is saved. */
static gboolean
on_signal (gpointer app)
{
  /* Closing a window removes it from the application's list, so walk a
   * referenced copy rather than the live list. */
  GList *windows = g_list_copy_deep (gtk_application_get_windows (GTK_APPLICATION (app)),
                                     (GCopyFunc) (void (*) (void)) g_object_ref, NULL);
  for (GList *l = windows; l != NULL; l = l->next)
    gtk_window_close (l->data);
  g_list_free_full (windows, g_object_unref);
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  /* The software renderer uses far less memory than the GL/Vulkan ones
   * and is plenty for text. Set GSK_RENDERER to override. */
  g_setenv ("GSK_RENDERER", "cairo", FALSE);

  if (!jr_crypto_init ())
    {
      g_printerr ("libsodium failed to initialise\n");
      return 1;
    }

  GtkApplication *app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_unix_signal_add (SIGTERM, on_signal, app);
  g_unix_signal_add (SIGINT, on_signal, app);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
