/* journal-notes: study notes.
 *
 * A separate program from the Journal: it shares no code that touches
 * journal data and is started by the launcher (`journal`, see
 * programs.h). First thing, it locks itself away from the Journal's
 * files (notesjail.h); without that lock it does not open any notes. */
#include <glib-unix.h>
#include <gtk/gtk.h>
#include "noteexport.h"
#include "notesdb.h"
#include "notesjail.h"
#include "notes_window.h"
#include "programs.h"

#define APP_ID    "io.github.biswas2200.Journal.Notes"
#define ICON      "io.github.biswas2200.Journal"
#define RESOURCES "/io/github/biswas2200/Journal/Notes"

static int exit_code = JR_EXIT_QUIT;
static GError *jail_error; /* set if the lock could not be entered */

static void
on_leave (int code, gpointer data)
{
  (void) data;
  exit_code = code;
}

static void
load_theme (void)
{
  GtkSettings *settings = gtk_settings_get_default ();
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (settings), "gtk-interface-color-scheme"))
    g_object_set (settings, "gtk-interface-color-scheme", 3 /* dark */, NULL);
  else
    g_object_set (settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, RESOURCES "/notes.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
  gtk_window_set_default_icon_name (ICON);
}

static void
on_error_closed (GObject *src, GAsyncResult *res, gpointer app)
{
  gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (src), res, NULL);
  g_application_release (app);
}

static void
fail (GApplication *app, const char *what, const char *why)
{
  exit_code = 1;
  GtkAlertDialog *alert = gtk_alert_dialog_new ("%s", what);
  gtk_alert_dialog_set_detail (alert, why);
  g_application_hold (app);
  gtk_alert_dialog_choose (alert, NULL, NULL, on_error_closed, app);
  g_object_unref (alert);
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
  if (jail_error != NULL)
    {
      fail (app, "Notes did not start", jail_error->message);
      return;
    }
  g_autofree char *path = g_build_filename (g_get_user_data_dir (), "journal-notes", "notes.db", NULL);
  g_autoptr (GError) error = NULL;
  JnDb *db = jn_db_open (path, &error);
  if (db == NULL)
    {
      fail (app, "The notes could not be opened", error->message);
      return;
    }
  g_autofree char *exports = jn_export_dir ();
  JnWindow *win = jn_window_new (GTK_APPLICATION (app), db, exports);
  /* The AI helper next to Notes ($JOURNAL_AI_HELPER: a build tree). */
  const char *helper = g_getenv ("JOURNAL_AI_HELPER");
  JnGeminiConfig ai = { helper != NULL && *helper != '\0' ? helper : JN_HELPER_PATH, NULL, NULL, NULL, NULL, { 0 }, 0 };
  jn_window_set_enhancer (win, jn_gemini_enhancer_new (&ai));
  if (g_strcmp0 (g_getenv ("JOURNAL_LAUNCHER"), "1") == 0)
    jn_window_set_navigation (win, on_leave, NULL);
  gtk_window_present (GTK_WINDOW (win));
}

/* SIGTERM / SIGINT (logout, Ctrl+C): close windows so everything saves. */
static gboolean
on_signal (gpointer app)
{
  GList *windows = g_list_copy_deep (gtk_application_get_windows (GTK_APPLICATION (app)),
                                     (GCopyFunc) (void (*) (void)) g_object_ref, NULL);
  for (GList *l = windows; l != NULL; l = l->next)
    gtk_window_close (l->data);
  if (windows == NULL)
    g_application_quit (G_APPLICATION (app));
  g_list_free_full (windows, g_object_unref);
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  g_setenv ("GSK_RENDERER", "cairo", FALSE);
  /* Before any thread or file: away from the journal, for good. */
  JnJailPaths *paths = jn_jail_default_paths ();
  if (!jn_jail_enter (paths, &jail_error))
    g_printerr ("journal-notes: %s; not opening any notes\n", jail_error->message);
  jn_jail_paths_free (paths);

  GtkApplication *app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_application_set_resource_base_path (G_APPLICATION (app), RESOURCES);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_unix_signal_add (SIGTERM, on_signal, app);
  g_unix_signal_add (SIGINT, on_signal, app);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  g_clear_error (&jail_error);
  return status != 0 ? status : exit_code;
}
