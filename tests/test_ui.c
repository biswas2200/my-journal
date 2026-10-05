/* UI test: drives the real window through every screen on the current
 * display and checks behaviour that matters for privacy and saving.
 * Skipped when there is no display. With JR_SHOTS_DIR set it also saves
 * a PNG of each screen (used to compare against the wireframes). */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include "day_view.h"
#include "journal.h"
#include "pin_dialog.h"
#include "window.h"

#if defined(__SANITIZE_ADDRESS__)
#define RUNNING_ON_SANITIZER 1
#else
#define RUNNING_ON_SANITIZER 0
#endif

static const JrKdfCost FAST = { JR_KDF_OPS_MIN, JR_KDF_MEM_MIN };

static gboolean
stop_waiting (gpointer flag)
{
  *(gboolean *) flag = TRUE;
  return G_SOURCE_REMOVE;
}

/* Runs the main loop for `ms` milliseconds (layout, timers, autosave). */
static void
pump (int ms)
{
  gboolean done = FALSE;
  g_timeout_add (ms, stop_waiting, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
}

static void
shot (GtkWidget *w, const char *name)
{
  const char *dir = g_getenv ("JR_SHOTS_DIR");
  if (dir == NULL)
    return;
  int width = gtk_widget_get_width (w), height = gtk_widget_get_height (w);
  GdkPaintable *p = gtk_widget_paintable_new (w);
  GtkSnapshot *s = gtk_snapshot_new ();
  gdk_paintable_snapshot (p, s, width, height);
  GskRenderNode *node = gtk_snapshot_free_to_node (s);
  if (node != NULL)
    {
      GskRenderer *r = gtk_native_get_renderer (gtk_widget_get_native (w));
      GdkTexture *t = gsk_renderer_render_texture (r, node, &GRAPHENE_RECT_INIT (0, 0, width, height));
      g_autofree char *path = g_strdup_printf ("%s/%s.png", dir, name);
      gdk_texture_save_to_png (t, path);
      g_object_unref (t);
      gsk_render_node_unref (node);
    }
  g_object_unref (p);
}

static GtkWidget *
find_type (GtkWidget *root, GType type)
{
  if (root == NULL)
    return NULL;
  if (G_TYPE_CHECK_INSTANCE_TYPE (root, type))
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_type (c, type);
      if (found != NULL)
        return found;
    }
  return NULL;
}

/* All text of every GtkTextView under root, joined. */
static void
collect_text (GtkWidget *root, GString *out)
{
  if (GTK_IS_TEXT_VIEW (root))
    {
      GtkTextBuffer *b = gtk_text_view_get_buffer (GTK_TEXT_VIEW (root));
      GtkTextIter s, e;
      gtk_text_buffer_get_bounds (b, &s, &e);
      char *t = gtk_text_buffer_get_text (b, &s, &e, FALSE);
      g_string_append (out, t);
      g_string_append_c (out, '|');
      g_free (t);
    }
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    collect_text (c, out);
}

static void
count_entry (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) id; (void) stamped_at; (void) text;
  (*(int *) user)++;
}

static void
type_text (JrWindow *win, const char *text)
{
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (win));
  g_assert_true (GTK_IS_TEXT_VIEW (focus)); /* cursor is ready */
  gtk_text_buffer_insert_at_cursor (gtk_text_view_get_buffer (GTK_TEXT_VIEW (focus)), text, -1);
}

/* Fills a few past days so the dropdown and activity screens have data. */
static void
seed (JrJournal *j)
{
  JrDay today = jr_day_today ();
  static const int minutes[] = { 0, 24, 8, 0, 25, 12, 9 };
  for (int back = 1; back <= 6; back++)
    {
      if (minutes[back] == 0)
        continue;
      JrDay d = jr_day_add (today, -back);
      char iso[JR_ISO_LEN];
      jr_day_to_iso (d, iso);
      gint64 t = jr_now () - back * 86400;
      jr_journal_add_entry (j, iso, t, "Slow start today. Two chapters done before noon, then my "
                                      "mind drifted to what-ifs again.", t);
      jr_journal_add_active_seconds (j, iso, minutes[back] * 60);
    }
  for (int back = 30; back <= 50; back += 3)
    {
      char iso[JR_ISO_LEN];
      jr_day_to_iso (jr_day_add (today, -back), iso);
      jr_journal_add_entry (j, iso, jr_now () - back * 86400, "older", jr_now ());
    }
}

static void
test_ui_flow (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);

  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/io/github/biswas2200/Journal/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                    "/io/github/biswas2200/Journal/icons");

  JrJournal *j = jr_journal_open (path, NULL);
  jr_journal_set_kdf_cost (j, FAST);
  seed (j);

  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (600);

  /* 02 Today: opens with the cursor ready in an empty stamped draft. */
  JrDayView *dv = jr_window_get_day_view (win);
  g_assert_nonnull (dv);
  g_assert_true (jr_day_view_is_current_today (dv));
  type_text (win, "Slow start today. Two chapters done before noon, then my mind drifted to "
                  "what-ifs again. Noticing it now instead of following it.");
  jr_day_view_new_entry (dv); /* Ctrl+Enter */
  type_text (win, "Evening check-in. The loop showed up again around dinner, but it was "
                  "shorter this time.");
  pump (1400); /* autosave fires ~1 s after typing stops */
  char today_iso[JR_ISO_LEN];
  jr_day_to_iso (jr_day_today (), today_iso);
  int n = 0;
  jr_journal_foreach_entry (j, today_iso, count_entry, &n);
  g_assert_cmpint (n, ==, 2);
  shot (GTK_WIDGET (win), "02-today");

  /* Ctrl+Enter on an empty last block reuses it instead of piling up drafts. */
  jr_day_view_new_entry (dv);
  jr_day_view_new_entry (dv);
  jr_day_view_save (dv);
  n = 0;
  jr_journal_foreach_entry (j, today_iso, count_entry, &n);
  g_assert_cmpint (n, ==, 2);

  /* 03 Day dropdown. */
  jr_day_view_open_days (dv);
  pump (400);
  GtkWidget *menu = find_type (gtk_window_get_titlebar (GTK_WINDOW (win)), GTK_TYPE_MENU_BUTTON);
  GtkPopover *pop = gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu));
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (pop)));
  g_assert_nonnull (gtk_popover_get_child (pop));
  shot (GTK_WIDGET (win), "03-dropdown-behind");
  shot (GTK_WIDGET (pop), "03-dropdown");
  gtk_popover_popdown (pop);
  pump (100);
  g_assert_null (gtk_popover_get_child (pop)); /* content freed when closed */
  /* Closing gives the cursor back to the writing block. */
  g_assert_true (GTK_IS_TEXT_VIEW (gtk_root_get_focus (GTK_ROOT (win))));
  /* Leaving the screen must free the popover itself. */
  g_object_add_weak_pointer (G_OBJECT (pop), (gpointer *) &pop);

  /* Past day: read-only until Edit. */
  jr_window_show_day (win, jr_day_add (jr_day_today (), -1));
  pump (1500);
  g_assert_null (pop); /* regression: focus in the popover kept it alive */
  GtkWidget *tv = find_type (gtk_window_get_child (GTK_WINDOW (win)), GTK_TYPE_TEXT_VIEW);
  g_assert_nonnull (tv);
  g_assert_false (gtk_text_view_get_editable (GTK_TEXT_VIEW (tv)));
  shot (GTK_WIDGET (win), "02b-past-day");

  /* 04 Activity and time. */
  jr_window_show_activity (win);
  pump (400);
  g_assert_null (jr_window_get_day_view (win));
  shot (GTK_WIDGET (win), "04-activity");

  /* 05 Lock and security, without and with a PIN. */
  jr_window_show_security (win);
  pump (300);
  shot (GTK_WIDGET (win), "05-security-off");
  char *recovery = jr_journal_enable_pin (j, "123456");
  jr_window_settings_changed (win);
  jr_window_show_security (win);
  pump (300);
  shot (GTK_WIDGET (win), "05-security-on");

  static const char *const prompts[] = { "Choose a 6-digit PIN", "Type the same PIN again", NULL };
  GtkWindow *pd = jr_pin_dialog_new (win, "Set a PIN", prompts, NULL, NULL);
  pump (300);
  shot (GTK_WIDGET (pd), "05b-pin-dialog");
  jr_recovery_dialog_show (win, recovery); /* replaces the PIN dialog */
  pump (300);

  /* 01 Lock: back to today first so there is text on screen to clear. */
  jr_window_show_today (win);
  pump (200);
  jr_window_lock (win, JR_LOCK_SLEEP);
  pump (400);
  g_assert_false (jr_journal_is_unlocked (j));
  g_assert_null (jr_window_get_day_view (win));
  /* Nothing of the entries is left in the window. */
  GString *text = g_string_new (NULL);
  collect_text (GTK_WIDGET (win), text);
  g_assert_cmpuint (text->len, ==, 0);
  g_string_free (text, TRUE);
  shot (GTK_WIDGET (win), "01-lock");

  /* Unlock: the saved text comes back. */
  g_assert_cmpint (jr_journal_unlock_pin (j, "123456", jr_now ()), ==, JR_UNLOCK_OK);
  jr_window_unlocked (win, FALSE);
  pump (300);
  text = g_string_new (NULL);
  collect_text (GTK_WIDGET (win), text);
  g_assert_nonnull (strstr (text->str, "Evening check-in"));
  g_string_free (text, TRUE);
  /* Regression: loaded text wraps to its full height, not one line. */
  GtkWidget *first = find_type (gtk_window_get_child (GTK_WINDOW (win)), GTK_TYPE_TEXT_VIEW);
  g_assert_cmpint (gtk_widget_get_height (first), >, 45);
  shot (GTK_WIDGET (win), "02c-today-reloaded");

  /* Quit locks and wipes. */
  gtk_window_close (GTK_WINDOW (win));
  pump (200);
  jr_secret_free (recovery);
  g_object_unref (css);

  g_remove (path);
  g_rmdir (dir);
}

/* One tour of every screen, then lock and unlock. JR_TOUR_PARTS (a bit
 * mask) limits it to some parts when hunting a leak. */
static void
tour (JrWindow *win, JrJournal *j)
{
  static int parts = -1;
  if (parts < 0)
    parts = g_getenv ("JR_TOUR_PARTS") ? atoi (g_getenv ("JR_TOUR_PARTS")) : 0xff;
  if (parts & 1)
    {
      jr_window_show_today (win);
      pump (30);
    }
  if (parts & 2)
    type_text (win, "x");
  if (parts & 4)
    {
      jr_day_view_open_days (jr_window_get_day_view (win));
      pump (30);
      GtkWidget *menu = find_type (gtk_window_get_titlebar (GTK_WINDOW (win)), GTK_TYPE_MENU_BUTTON);
      gtk_popover_popdown (gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu)));
    }
  if (parts & 8)
    {
      jr_window_show_day (win, jr_day_add (jr_day_today (), -1));
      pump (30);
    }
  if (parts & 16)
    {
      jr_window_show_activity (win);
      pump (30);
    }
  if (parts & 32)
    {
      jr_window_show_security (win);
      pump (30);
    }
  if (parts & 64)
    {
      jr_window_lock (win, JR_LOCK_MANUAL);
      pump (30);
      g_assert_cmpint (jr_journal_unlock_pin (j, "123456", jr_now ()), ==, JR_UNLOCK_OK);
      jr_window_unlocked (win, FALSE);
      pump (30);
    }
}

static gsize
heap_in_use (void)
{
  malloc_trim (0);
  struct mallinfo2 mi = mallinfo2 ();
  return mi.uordblks + mi.hblkhd;
}

/* Leak check for the UI: after warming up GTK's own caches, repeating
 * every screen and a lock cycle many times must not keep growing the heap. */
static void
test_ui_no_growth (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  if ((g_getenv ("ASAN_OPTIONS") != NULL || RUNNING_ON_SANITIZER) && g_getenv ("JR_TOURS") == NULL)
    {
      g_test_skip ("heap numbers are not meaningful under a sanitizer");
      return;
    }
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = jr_journal_open (path, NULL);
  jr_journal_set_kdf_cost (j, FAST);
  seed (j);
  char *recovery = jr_journal_enable_pin (j, "123456");
  jr_secret_free (recovery);
  jr_journal_lock (j);
  jr_journal_unlock_pin (j, "123456", jr_now ());

  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (300);
  for (int i = 0; i < 5; i++)
    tour (win, j);
  gsize before = heap_in_use ();
  int tours = g_getenv ("JR_TOURS") ? atoi (g_getenv ("JR_TOURS")) : 25;
  for (int i = 0; i < tours; i++)
    tour (win, j);
  gsize after = heap_in_use ();
  g_test_message ("heap before %zu KiB, after %d tours %zu KiB", before / 1024, tours, after / 1024);
  /* 25 tours create and destroy ~200 screens; allow only cache noise
   * (measured: ~17 KiB, and the same after 60 tours). */
  g_assert_cmpint ((gssize) after - (gssize) before, <, 128 * 1024);

  gtk_window_close (GTK_WINDOW (win));
  pump (100);
  g_remove (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_setenv ("GSK_RENDERER", "cairo", FALSE);
  g_test_init (&argc, &argv, NULL);
  g_assert_true (jr_crypto_init ());
  g_test_add_func ("/ui/flow", test_ui_flow);
  g_test_add_func ("/ui/no-growth", test_ui_no_growth);
  return g_test_run ();
}
