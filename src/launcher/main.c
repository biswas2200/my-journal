/* journal: the start screen.
 *
 * Shows two choices and runs the Journal (`journal-diary`) or Notes
 * (`journal-notes`) as a separate process, one at a time. It never opens
 * either one's files and links none of their code: all it learns from a
 * program is its exit status (programs.h). The Journal locks because its
 * process ends; coming back starts a new one at the lock screen.
 *
 * `journal --open=journal|notes` skips the start screen (desktop menu
 * actions, tests). $JOURNAL_PROGRAM_DIR overrides where the two programs
 * are looked for (the build tree, in tests). */
#include <glib-unix.h>
#include <gtk/gtk.h>
#include "programs.h"

#define APP_ID "io.github.biswas2200.Journal"

typedef struct {
  GtkApplication *app;
  GtkWindow      *home;
  GtkWidget      *message;
  GSubprocess    *child;
  JrNext          running;  /* DIARY or NOTES while a child runs */
  JrNext          first;    /* from --open, else HOME */
  gboolean        started;
  gboolean        quitting; /* SIGTERM: quit once the child is gone */
} Launcher;

static const char CSS[] =
  "window.home, window.home .background { background: #111111; color: #EDEDED; }"
  "window.home headerbar { background: #1A1A1A; box-shadow: none; border-bottom: 1px solid #2A2A2A; }"
  ".home-title { font-family: serif; font-size: 22px; }"
  ".home-card { background: #1C1C1C; border: 1px solid #3A3A3A; border-radius: 10px; padding: 22px; }"
  ".home-card:hover { border-color: #8A8A8A; background: #202020; }"
  ".home-card.notes { border-color: #3F5470; }"
  ".home-card.notes:hover { border-color: #6C8EBF; }"
  ".card-name { font-family: serif; font-size: 20px; font-weight: bold; color: #EDEDED; }"
  ".card-text { color: #CFCFCF; }"
  ".card-note, .home-faint { color: #8A8A8A; font-family: monospace; font-size: 12px; }"
  ".home-error { color: #E5A0A0; font-family: monospace; }";

static char *
program_path (const char *file)
{
  const char *dir = g_getenv ("JOURNAL_PROGRAM_DIR");
  if (dir == NULL || *dir == '\0')
    dir = JR_PROGRAM_DIR;
  return g_build_filename (dir, file, NULL);
}

static void
show_home (Launcher *l, const char *error)
{
  if (l->home == NULL)
    return;
  gtk_label_set_text (GTK_LABEL (l->message), error != NULL ? error : "");
  gtk_widget_set_visible (l->message, error != NULL);
  gtk_window_present (l->home);
}

static void
quit (Launcher *l)
{
  GtkWindow *home = g_steal_pointer (&l->home);
  if (home != NULL)
    gtk_window_destroy (home); /* last window: the app ends */
}

/* A token from the compositor lets the new program's window take focus
 * (Wayland xdg-activation; startup notification on X11). */
static char *
activation_token (Launcher *l, const char *path)
{
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (l->home));
  GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context (display);
  GAppInfo *info = g_app_info_create_from_commandline (path, NULL, G_APP_INFO_CREATE_NONE, NULL);
  char *token = info != NULL ? g_app_launch_context_get_startup_notify_id (G_APP_LAUNCH_CONTEXT (ctx), info, NULL)
                             : NULL;
  g_clear_object (&info);
  g_object_unref (ctx);
  return token;
}

static void start (Launcher *l, JrNext which);

static void
after (Launcher *l, JrNext next, JrNext was)
{
  switch (next)
    {
    case JR_NEXT_DIARY:
    case JR_NEXT_NOTES:
      start (l, next);
      break;
    case JR_NEXT_HOME:
      show_home (l, NULL);
      break;
    case JR_NEXT_HOME_AFTER_ERROR:
      show_home (l, was == JR_NEXT_NOTES ? "Notes closed unexpectedly. Your notes up to the last save are kept."
                                         : "The journal closed unexpectedly. It is locked; entries up to the last save are kept.");
      break;
    case JR_NEXT_QUIT:
    default:
      quit (l);
      break;
    }
}

static void
on_child_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Launcher *l = data;
  GSubprocess *child = G_SUBPROCESS (source);
  JrNext next = JR_NEXT_HOME_AFTER_ERROR;
  if (g_subprocess_wait_finish (child, result, NULL))
    next = jr_programs_next (g_subprocess_get_status (child));
  JrNext was = l->running;
  g_clear_object (&l->child);
  l->running = JR_NEXT_HOME;
  after (l, l->quitting ? JR_NEXT_QUIT : next, was);
  g_application_release (G_APPLICATION (l->app));
}

static void
start (Launcher *l, JrNext which)
{
  const char *file = jr_programs_file (which);
  g_return_if_fail (file != NULL && l->child == NULL);
  g_autofree char *path = program_path (file);

  GSubprocessLauncher *sl = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (sl, "JOURNAL_LAUNCHER", "1", TRUE);
  g_subprocess_launcher_unsetenv (sl, "JOURNAL_PROGRAM_DIR");
  g_autofree char *token = activation_token (l, path);
  if (token != NULL)
    {
      g_subprocess_launcher_setenv (sl, "XDG_ACTIVATION_TOKEN", token, TRUE);
      g_subprocess_launcher_setenv (sl, "DESKTOP_STARTUP_ID", token, TRUE);
    }
  GError *error = NULL;
  l->child = g_subprocess_launcher_spawn (sl, &error, path, NULL);
  g_object_unref (sl);
  if (l->child == NULL)
    {
      g_autofree char *msg = g_strdup_printf ("Could not start %s: %s", file, error->message);
      g_error_free (error);
      show_home (l, msg);
      return;
    }
  l->running = which;
  g_application_hold (G_APPLICATION (l->app));
  g_subprocess_wait_async (l->child, NULL, on_child_done, l);
  gtk_widget_set_visible (GTK_WIDGET (l->home), FALSE);
}

static void
on_card (GtkButton *button, gpointer data)
{
  Launcher *l = data;
  JrNext which = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "jr-next"));
  if (l->child == NULL)
    start (l, which);
}

static GtkWidget *
text (const char *s, const char *css)
{
  GtkWidget *w = gtk_label_new (s);
  gtk_label_set_wrap (GTK_LABEL (w), TRUE);
  gtk_label_set_xalign (GTK_LABEL (w), 0.0f);
  gtk_widget_add_css_class (w, css);
  return w;
}

static GtkWidget *
card (Launcher *l, JrNext which, const char *name, const char *what, const char *note)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append (GTK_BOX (box), text (name, "card-name"));
  gtk_box_append (GTK_BOX (box), text (what, "card-text"));
  GtkWidget *n = text (note, "card-note");
  gtk_widget_set_vexpand (n, TRUE);
  gtk_widget_set_valign (n, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (box), n);

  GtkWidget *b = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (b), box);
  gtk_widget_add_css_class (b, "home-card");
  if (which == JR_NEXT_NOTES)
    gtk_widget_add_css_class (b, "notes");
  gtk_widget_set_size_request (b, 250, 190);
  g_object_set_data (G_OBJECT (b), "jr-next", GINT_TO_POINTER (which));
  g_signal_connect (b, "clicked", G_CALLBACK (on_card), l);
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return b;
}

static void
build_home (Launcher *l)
{
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
  gtk_window_set_default_icon_name (APP_ID);

  l->home = GTK_WINDOW (gtk_application_window_new (l->app));
  gtk_widget_add_css_class (GTK_WIDGET (l->home), "home");
  gtk_window_set_title (l->home, "Journal & Notes");
  gtk_window_set_default_size (l->home, 720, 460);
  GtkWidget *header = gtk_header_bar_new ();
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_window_set_titlebar (l->home, header);

  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 22);
  gtk_widget_set_halign (col, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (col, GTK_ALIGN_CENTER);
  GtkWidget *title = gtk_label_new ("What do you want to do today?");
  gtk_widget_add_css_class (title, "home-title");
  gtk_box_append (GTK_BOX (col), title);

  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_set_halign (row, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (row), card (l, JR_NEXT_DIARY, "Journal", "Private diary.\nOffline, no AI.",
                                       "PIN asked every time"));
  gtk_box_append (GTK_BOX (row), card (l, JR_NEXT_NOTES, "Notes", "College study notes.\nAI enhance, online.",
                                       "Never sees the journal"));
  gtk_box_append (GTK_BOX (col), row);

  l->message = gtk_label_new ("");
  gtk_widget_add_css_class (l->message, "home-error");
  gtk_label_set_wrap (GTK_LABEL (l->message), TRUE);
  gtk_widget_set_visible (l->message, FALSE);
  gtk_box_append (GTK_BOX (col), l->message);
  GtkWidget *faint = gtk_label_new ("Two separate programs. This screen only starts one of them.");
  gtk_widget_add_css_class (faint, "home-faint");
  gtk_box_append (GTK_BOX (col), faint);
  gtk_window_set_child (l->home, col);
}

static void
on_activate (GApplication *app, gpointer data)
{
  (void) app;
  Launcher *l = data;
  if (!l->started)
    {
      l->started = TRUE;
      build_home (l);
      if (l->first == JR_NEXT_DIARY || l->first == JR_NEXT_NOTES)
        {
          start (l, l->first);
          return;
        }
    }
  if (l->child == NULL)
    show_home (l, NULL);
}

static int
on_local_options (GApplication *app, GVariantDict *options, gpointer data)
{
  (void) app;
  Launcher *l = data;
  if (g_variant_dict_contains (options, "version"))
    {
      g_print ("journal %s\n", JR_VERSION);
      return 0;
    }
  const char *open = NULL;
  if (g_variant_dict_lookup (options, "open", "&s", &open))
    {
      if (g_strcmp0 (open, "journal") == 0)
        l->first = JR_NEXT_DIARY;
      else if (g_strcmp0 (open, "notes") == 0)
        l->first = JR_NEXT_NOTES;
      else
        {
          g_printerr ("journal: --open takes journal or notes\n");
          return 2;
        }
    }
  return -1; /* go on */
}

/* SIGTERM / SIGINT (logout, Ctrl+C): let the running program save and
 * close first, then quit. */
static gboolean
on_signal (gpointer data)
{
  Launcher *l = data;
  l->quitting = TRUE;
  if (l->child != NULL)
    g_subprocess_send_signal (l->child, SIGTERM);
  else
    quit (l);
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  g_setenv ("GSK_RENDERER", "cairo", FALSE);
  Launcher l = { .running = JR_NEXT_HOME, .first = JR_NEXT_HOME };
  l.app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option (G_APPLICATION (l.app), "open", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                 "Skip the start screen", "journal|notes");
  g_application_add_main_option (G_APPLICATION (l.app), "version", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                 "Show the version and quit", NULL);
  g_signal_connect (l.app, "handle-local-options", G_CALLBACK (on_local_options), &l);
  g_signal_connect (l.app, "activate", G_CALLBACK (on_activate), &l);
  g_unix_signal_add (SIGTERM, on_signal, &l);
  g_unix_signal_add (SIGINT, on_signal, &l);
  int status = g_application_run (G_APPLICATION (l.app), argc, argv);
  g_object_unref (l.app);
  return status;
}
