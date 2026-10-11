/* UI test: drives the real window through every screen on the current
 * display and checks behaviour that matters for privacy and saving.
 * Skipped when there is no display. With JR_SHOTS_DIR set it also saves
 * a PNG of each screen (used to compare against the wireframes). */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <sqlite3.h>
#include "day_view.h"
#include "journal.h"
#include "secret_dialog.h"
#include "window.h"

#if defined(__SANITIZE_ADDRESS__)
#define RUNNING_ON_SANITIZER 1
#else
#define RUNNING_ON_SANITIZER 0
#endif

static const JrKdfCost FAST = { JR_KDF_OPS_MIN, JR_KDF_MEM_MIN };
#define MASTER "river lamp quiet oak"

/* The login keyring stand-in: in memory, so tests never touch a real one. */
static GHashTable *keyring; /* id -> JrKey */

static JrDeviceStatus
mem_load (const char *id, JrKey **out, gpointer user)
{
  (void) user;
  JrKey *k = keyring != NULL ? g_hash_table_lookup (keyring, id) : NULL;
  if (k == NULL)
    return JR_DEVICE_MISSING;
  *out = jr_key_dup (k);
  return JR_DEVICE_FOUND;
}

static gboolean
mem_save (const char *id, const JrKey *secret, gpointer user)
{
  (void) user;
  if (keyring == NULL)
    keyring = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) jr_key_free);
  g_hash_table_insert (keyring, g_strdup (id), jr_key_dup (secret));
  return TRUE;
}

static void
mem_clear (const char *id, gpointer user)
{
  (void) user;
  if (keyring != NULL)
    g_hash_table_remove (keyring, id);
}

static const JrDeviceStore STORE = { mem_load, mem_save, mem_clear, NULL };

/* The app's stylesheet and icons, as main.c loads them, so screenshots
 * look like the app. Once per process; the display keeps them. */
static void
load_style (void)
{
  static gboolean loaded;
  if (loaded)
    return;
  loaded = TRUE;
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/io/github/biswas2200/Journal/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                    "/io/github/biswas2200/Journal/icons");
}

static JrJournal *
open_journal (const char *path)
{
  JrJournal *j = jr_journal_open (path, NULL);
  jr_journal_set_kdf_cost (j, FAST);
  jr_journal_set_device_store (j, &STORE);
  return j;
}

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

static gboolean
count_frame (GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
  (void) w; (void) clock;
  (*(int *) data)++;
  return G_SOURCE_CONTINUE;
}

/* Pumps until `w` is taller than `min` px or `ms` pass; returns its height.
 * *frames gets how many frames the compositor let GTK draw meanwhile. */
static int
wait_for_height (GtkWidget *w, int min, int ms, int *frames)
{
  *frames = 0;
  guint id = gtk_widget_add_tick_callback (w, count_frame, frames, NULL);
  for (int waited = 0; gtk_widget_get_height (w) <= min && waited < ms; waited += 20)
    pump (20);
  gtk_widget_remove_tick_callback (w, id);
  return gtk_widget_get_height (w);
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

static GtkWindow *
find_toplevel (const char *title)
{
  GtkWindow *found = NULL;
  GList *all = gtk_window_list_toplevels ();
  for (GList *l = all; l != NULL; l = l->next)
    if (gtk_widget_get_visible (l->data) && g_strcmp0 (gtk_window_get_title (l->data), title) == 0)
      found = l->data;
  g_list_free (all);
  return found;
}

static GtkWidget *
find_button (GtkWidget *root, const char *label)
{
  if (root == NULL)
    return NULL;
  if (GTK_IS_BUTTON (root) && g_strcmp0 (gtk_button_get_label (GTK_BUTTON (root)), label) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_button (c, label);
      if (found != NULL)
        return found;
    }
  return NULL;
}

static GtkWidget *
find_label (GtkWidget *root, const char *text)
{
  if (root == NULL)
    return NULL;
  if (GTK_IS_LABEL (root) && g_strcmp0 (gtk_label_get_text (GTK_LABEL (root)), text) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_label (c, text);
      if (found != NULL)
        return found;
    }
  return NULL;
}

/* A visible label under root whose text contains `part`. */
static gboolean
has_label_containing (GtkWidget *root, const char *part)
{
  if (root == NULL)
    return FALSE;
  if (GTK_IS_LABEL (root) && gtk_widget_get_visible (root) &&
      strstr (gtk_label_get_text (GTK_LABEL (root)), part) != NULL)
    return TRUE;
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    if (has_label_containing (c, part))
      return TRUE;
  return FALSE;
}

/* The first visible password field under root. */
static GtkWidget *
find_password (GtkWidget *root)
{
  if (root == NULL)
    return NULL;
  if (GTK_IS_PASSWORD_ENTRY (root) && gtk_widget_get_visible (root))
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_password (c);
      if (found != NULL)
        return found;
    }
  return NULL;
}

/* Types `text` into a password field and presses Enter. */
static void
enter_text (GtkWidget *entry, const char *text)
{
  g_assert_nonnull (entry);
  gtk_editable_set_text (GTK_EDITABLE (entry), text);
  g_signal_emit_by_name (entry, "activate");
}

/* Pumps until `part` shows in a label under root, or `ms` pass. */
static gboolean
wait_for_label (GtkWidget *root, const char *part, int ms)
{
  for (int waited = 0; !has_label_containing (root, part) && waited < ms; waited += 20)
    pump (20);
  return has_label_containing (root, part);
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

  load_style ();

  JrJournal *j = open_journal (path);
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

  /* 03 Day dropdown. Without a real input event, Wayland may refuse a
   * grabbing popup, so the test opens it without autohide. */
  GtkWidget *menu = find_type (gtk_window_get_titlebar (GTK_WINDOW (win)), GTK_TYPE_MENU_BUTTON);
  GtkPopover *pop = gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu));
  gtk_popover_set_autohide (pop, FALSE);
  jr_day_view_open_days (dv);
  pump (400);
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
  jr_lock_kind_dialog_show (win, NULL); /* not clicked: only looked at */
  pump (300);
  shot (GTK_WIDGET (find_toplevel ("Lock the journal")), "05e-choose-kind");
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456", MASTER);
  jr_window_settings_changed (win);
  jr_window_show_security (win);
  pump (300);
  shot (GTK_WIDGET (win), "05-security-on");

  static const JrSecretStep steps[] = {
    { "Choose a 6-digit PIN", JR_LOCK_PIN, TRUE, NULL },
    { "Type the same PIN again", JR_LOCK_PIN, TRUE, NULL },
  };
  GtkWindow *pd = jr_secret_dialog_new (win, "Set a PIN", steps, 2, NULL, NULL);
  pump (300);
  shot (GTK_WIDGET (pd), "05b-pin-dialog");
  jr_recovery_dialog_show (win, recovery); /* replaces the PIN dialog */
  pump (300);
  /* The recovery key can be read, but not copied or selected. */
  GtkWindow *rd = find_toplevel ("Recovery key");
  g_assert_nonnull (rd);
  g_assert_null (find_button (GTK_WIDGET (rd), "Copy"));
  GtkWidget *key_label = find_label (GTK_WIDGET (rd), recovery);
  g_assert_nonnull (key_label);
  g_assert_false (gtk_label_get_selectable (GTK_LABEL (key_label)));
  shot (GTK_WIDGET (rd), "05c-recovery");

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
  g_assert_cmpint (jr_journal_unlock (j, "123456", jr_now ()), ==, JR_UNLOCK_OK);
  jr_window_unlocked (win, JR_SECRET_LOCK);
  pump (300);
  text = g_string_new (NULL);
  collect_text (GTK_WIDGET (win), text);
  g_assert_nonnull (strstr (text->str, "Evening check-in"));
  g_string_free (text, TRUE);
  /* Loading text is not an undoable edit: Ctrl+Z must not erase an entry. */
  GtkWidget *loaded = find_type (gtk_window_get_child (GTK_WINDOW (win)), GTK_TYPE_TEXT_VIEW);
  g_assert_false (gtk_text_buffer_get_can_undo (gtk_text_view_get_buffer (GTK_TEXT_VIEW (loaded))));
  /* Input methods are told not to learn or remember journal text. */
  g_assert_true (gtk_text_view_get_input_hints (GTK_TEXT_VIEW (loaded)) & GTK_INPUT_HINT_PRIVATE);
  /* Regression: loaded text wraps to its full height, not one line. */
  GtkWidget *first = find_type (gtk_window_get_child (GTK_WINDOW (win)), GTK_TYPE_TEXT_VIEW);
  int frames = 0;
  int height = wait_for_height (first, 45, 3000, &frames);
  g_test_message ("loaded block height %d after %d frames", height, frames);
  g_assert_cmpint (height, >, 45);
  shot (GTK_WIDGET (win), "02c-today-reloaded");

  /* Quit locks and wipes. */
  gtk_window_close (GTK_WINDOW (win));
  pump (200);
  jr_secret_free (recovery);

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
      GtkWidget *menu = find_type (gtk_window_get_titlebar (GTK_WINDOW (win)), GTK_TYPE_MENU_BUTTON);
      GtkPopover *pop = gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu));
      gtk_popover_set_autohide (pop, FALSE);
      jr_day_view_open_days (jr_window_get_day_view (win));
      pump (30);
      gtk_popover_popdown (pop);
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
      g_assert_cmpint (jr_journal_unlock (j, "123456", jr_now ()), ==, JR_UNLOCK_OK);
      jr_window_unlocked (win, JR_SECRET_LOCK);
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
  /* Only a sanitizer build skips (meson sets ASAN_OPTIONS for every test,
   * so the environment says nothing). */
  if (RUNNING_ON_SANITIZER && g_getenv ("JR_TOURS") == NULL)
    {
      g_test_skip ("heap numbers are not meaningful under a sanitizer");
      return;
    }
  if (g_str_equal (G_OBJECT_TYPE_NAME (gdk_display_get_default ()), "GdkBroadwayDisplay"))
    {
      g_test_skip ("Broadway keeps per-surface caches; heap numbers need a real compositor");
      return;
    }
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = open_journal (path);
  jr_journal_set_kdf_cost (j, FAST);
  seed (j);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456", MASTER);
  jr_secret_free (recovery);
  jr_journal_lock (j);
  jr_journal_unlock (j, "123456", jr_now ());

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

typedef struct {
  gboolean found;
  const char *needle;
} Find;

static void
find_text (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) id; (void) stamped_at;
  Find *f = user;
  if (strstr (text, f->needle) != NULL)
    f->found = TRUE;
}

static gboolean
saved_today (JrJournal *j, const char *needle)
{
  char iso[JR_ISO_LEN];
  jr_day_to_iso (jr_day_today (), iso);
  Find f = { FALSE, needle };
  jr_journal_foreach_entry (j, iso, find_text, &f);
  return f.found;
}

/* Spec manual checks "quit while writing" and "lid close while writing",
 * automated: text typed a moment before quitting or sleeping is saved
 * even though the 1 s autosave has not fired yet. */
static void
test_ui_saves_on_quit_and_sleep (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);

  /* Quit while writing. */
  JrJournal *j = open_journal (path);
  jr_journal_set_kdf_cost (j, FAST);
  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (300);
  type_text (win, "typed just before quitting");
  g_object_add_weak_pointer (G_OBJECT (win), (gpointer *) &win);
  gtk_window_close (GTK_WINDOW (win)); /* no pump: autosave cannot have run */
  pump (100);
  g_assert_null (win); /* destroyed, so the key is wiped and the file closed */
  j = open_journal (path);
  jr_journal_set_kdf_cost (j, FAST);
  g_assert_true (saved_today (j, "typed just before quitting"));

  /* Lid close (sleep) while writing. */
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456", MASTER);
  win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (300);
  type_text (win, " and before the lid closed");
  jr_window_lock (win, JR_LOCK_SLEEP);
  g_assert_false (jr_journal_is_unlocked (j));
  g_assert_cmpint (jr_journal_unlock (j, "123456", jr_now ()), ==, JR_UNLOCK_OK);
  g_assert_true (saved_today (j, "and before the lid closed"));
  gtk_window_close (GTK_WINDOW (win));
  pump (100);

  jr_secret_free (recovery);
  g_remove (path);
  g_rmdir (dir);
}

/* Spec manual check "very long entry", automated: a ~1 MiB paste saves
 * and the window keeps responding. */
static void
test_ui_long_entry (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = open_journal (path);
  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (300);

  GString *big = g_string_sized_new (1 << 20);
  /* Real writing has paragraphs; GTK re-wraps only the edited one. */
  for (int i = 0; big->len < (1 << 20); i++)
    g_string_append (big, i % 5 == 4 ? "The loop was shorter this time.\n\n"
                                     : "The loop showed up again around dinner. ");
  g_string_append (big, "THE-END");

  gint64 t0 = g_get_monotonic_time ();
  type_text (win, big->str);
  pump (50);
  gint64 t1 = g_get_monotonic_time ();
  jr_day_view_save (jr_window_get_day_view (win));
  gint64 t2 = g_get_monotonic_time ();
  /* Typing one more character into a huge block stays quick. */
  type_text (win, "!");
  pump (16);
  gint64 t3 = g_get_monotonic_time ();
  g_test_message ("paste+layout %.0f ms, save %.0f ms, next keystroke %.0f ms",
                  (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (t3 - t2) / 1000.0);
  g_assert_true (saved_today (j, "THE-END"));
  g_assert_cmpint (t2 - t1, <, 500 * 1000);
  g_assert_cmpint (t3 - t2, <, 150 * 1000);

  g_string_free (big, TRUE);
  gtk_window_close (GTK_WINDOW (win));
  pump (100);
  g_remove (path);
  g_rmdir (dir);
}

static gboolean
dialog_gone (const char *title, int ms)
{
  for (int waited = 0; find_toplevel (title) != NULL && waited < ms; waited += 20)
    pump (20);
  return find_toplevel (title) == NULL;
}

/* Passphrase on the lock screen, then changed through Lock & security
 * (the Argon2id work runs on a worker thread while the dialog waits). */
static void
test_ui_passphrase (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  load_style ();
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = open_journal (path);
  jr_journal_set_kdf_cost (j, FAST);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PASSPHRASE, "correct horse battery staple", NULL);
  g_assert_nonnull (recovery);

  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (200);
  GtkWidget *content = gtk_window_get_child (GTK_WINDOW (win));
  g_assert_true (has_label_containing (content, "Enter your passphrase"));
  shot (GTK_WIDGET (win), "01b-lock-passphrase");

  /* Wrong passphrase: refused, field wiped, a try counted. */
  GtkWidget *entry = find_password (content);
  enter_text (entry, "wrong horse battery staple");
  g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (entry)), ==, "");
  g_assert_true (wait_for_label (content, "Wrong passphrase", 3000));
  g_assert_false (jr_journal_is_unlocked (j));

  /* Right one: unlocks to today's page. */
  enter_text (find_password (content), "correct horse battery staple");
  for (int waited = 0; jr_window_get_day_view (win) == NULL && waited < 3000; waited += 20)
    pump (20);
  g_assert_true (jr_journal_is_unlocked (j));
  g_assert_nonnull (jr_window_get_day_view (win));

  /* Change it in Lock & security. A wrong current passphrase is refused. */
  jr_window_show_security (win);
  pump (200);
  content = gtk_window_get_child (GTK_WINDOW (win));
  shot (GTK_WIDGET (win), "05d-security-passphrase");
  g_signal_emit_by_name (find_button (content, "Change passphrase"), "clicked");
  pump (200);
  GtkWindow *dlg = find_toplevel ("Change passphrase");
  g_assert_nonnull (dlg);
  shot (GTK_WIDGET (dlg), "05f-change-passphrase");
  enter_text (find_password (GTK_WIDGET (dlg)), "not my passphrase at all");
  enter_text (find_password (GTK_WIDGET (dlg)), "a brand new passphrase here");
  enter_text (find_password (GTK_WIDGET (dlg)), "a brand new passphrase here");
  g_assert_true (wait_for_label (GTK_WIDGET (dlg), "Wrong passphrase", 3000));

  /* Too short a new passphrase is refused at that step. */
  enter_text (find_password (GTK_WIDGET (dlg)), "correct horse battery staple");
  enter_text (find_password (GTK_WIDGET (dlg)), "too short");
  g_assert_true (has_label_containing (GTK_WIDGET (dlg), "at least 12 characters"));
  enter_text (find_password (GTK_WIDGET (dlg)), "a brand new passphrase here");
  enter_text (find_password (GTK_WIDGET (dlg)), "a brand new passphrase here");
  g_assert_true (dialog_gone ("Change passphrase", 3000));

  jr_window_lock (win, JR_LOCK_MANUAL);
  g_assert_cmpint (jr_journal_unlock (j, "correct horse battery staple", jr_now ()), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock (j, "a brand new passphrase here", jr_now ()), ==, JR_UNLOCK_OK);

  gtk_window_close (GTK_WINDOW (win));
  pump (100);
  jr_secret_free (recovery);
  g_remove (path);
  g_rmdir (dir);
}

/* Presses a key on `widget` the way the keyboard would reach it. */
static void
press_key (GtkWidget *widget, guint keyval)
{
  GListModel *ctrls = gtk_widget_observe_controllers (widget);
  for (guint i = 0; i < g_list_model_get_n_items (ctrls); i++)
    {
      GtkEventController *c = g_list_model_get_item (ctrls, i);
      gboolean handled = FALSE;
      if (GTK_IS_EVENT_CONTROLLER_KEY (c))
        g_signal_emit_by_name (c, "key-pressed", keyval, 0, 0, &handled);
      g_object_unref (c);
      if (handled)
        break;
    }
  g_object_unref (ctrls);
}

static void
type_pin (GtkWidget *lock_view, const char *pin)
{
  for (const char *p = pin; *p != '\0'; p++)
    press_key (lock_view, GDK_KEY_0 + (guint) (*p - '0'));
}

static gboolean
wait_unlocked (JrJournal *j, int ms)
{
  for (int waited = 0; !jr_journal_is_unlocked (j) && waited < ms; waited += 20)
    pump (20);
  pump (50);
  return jr_journal_is_unlocked (j);
}

/* Spec manual checks "wrong PIN five times" and the recovery key, driven
 * through the real lock screen with key presses. */
static void
test_ui_pin_lock_screen (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  load_style ();
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = open_journal (path);
  jr_journal_set_kdf_cost (j, FAST);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456", MASTER);
  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);

  /* The right PIN unlocks on the sixth digit, no Enter needed. */
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (100);
  GtkWidget *view = gtk_window_get_child (GTK_WINDOW (win));
  type_pin (view, "123456");
  g_assert_true (wait_unlocked (j, 3000));
  g_assert_nonnull (jr_window_get_day_view (win));

  /* Backspace and Esc edit the digits before the sixth. */
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (100);
  view = gtk_window_get_child (GTK_WINDOW (win));
  type_pin (view, "99");
  press_key (view, GDK_KEY_BackSpace);
  press_key (view, GDK_KEY_Escape);
  type_pin (view, "123456");
  g_assert_true (wait_unlocked (j, 3000));

  /* Recovery key: a wrong one is refused, the right one unlocks and
   * opens Lock & security (time to choose a new PIN). */
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (100);
  view = gtk_window_get_child (GTK_WINDOW (win));
  g_signal_emit_by_name (find_button (view, "Use recovery key"), "clicked");
  pump (50);
  GtkWidget *rec = find_type (view, GTK_TYPE_ENTRY);
  gtk_editable_set_text (GTK_EDITABLE (rec), "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA");
  g_signal_emit_by_name (rec, "activate");
  g_assert_true (wait_for_label (view, "does not match", 3000));
  gtk_editable_set_text (GTK_EDITABLE (rec), recovery);
  g_signal_emit_by_name (rec, "activate");
  g_assert_true (wait_unlocked (j, 3000));
  g_assert_true (has_label_containing (gtk_window_get_titlebar (GTK_WINDOW (win)), "Lock & security"));

  /* The master passphrase opens it too. */
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (100);
  view = gtk_window_get_child (GTK_WINDOW (win));
  g_signal_emit_by_name (find_button (view, "Forgot PIN? Use master passphrase"), "clicked");
  pump (50);
  enter_text (find_password (view), "not the master passphrase");
  g_assert_true (wait_for_label (view, "Wrong master passphrase", 3000));
  enter_text (find_password (view), MASTER);
  g_assert_true (wait_unlocked (j, 3000));
  g_assert_nonnull (jr_window_get_day_view (win));

  /* Five wrong PINs block the PIN: no timer, the screen asks for the
   * master passphrase instead, and the PIN cannot even be typed. */
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (100);
  view = gtk_window_get_child (GTK_WINDOW (win));
  for (int i = 0; i < 4; i++)
    {
      type_pin (view, "000000");
      g_assert_true (wait_for_label (view, i == 3 ? "1 try left before the PIN is blocked"
                                                  : "tries left before the PIN is blocked", 3000));
    }
  type_pin (view, "000000");
  g_assert_true (wait_for_label (view, "the PIN is blocked", 3000));
  g_assert_true (has_label_containing (view, "Enter your master passphrase"));
  g_assert_null (find_button (view, "Use PIN instead"));
  type_pin (view, "123456"); /* ignored: not the PIN screen any more */
  pump (300);
  g_assert_false (jr_journal_is_unlocked (j));
  shot (GTK_WIDGET (win), "01c-pin-blocked");

  /* The master passphrase opens it, but only to choose a new PIN: no day
   * page, and the old PIN is refused. */
  enter_text (find_password (view), MASTER);
  g_assert_true (wait_unlocked (j, 3000));
  g_assert_null (jr_window_get_day_view (win));
  GtkWindow *dlg = find_toplevel ("Choose a new PIN");
  g_assert_nonnull (dlg);
  shot (GTK_WIDGET (dlg), "01d-new-pin");
  type_pin (GTK_WIDGET (dlg), "123456");
  type_pin (GTK_WIDGET (dlg), "123456");
  g_assert_true (wait_for_label (GTK_WIDGET (dlg), "the PIN being replaced", 3000));

  /* Closing it instead locks the journal again. */
  gtk_window_close (dlg);
  for (int waited = 0; jr_journal_is_unlocked (j) && waited < 3000; waited += 20)
    pump (20);
  g_assert_false (jr_journal_is_unlocked (j));
  view = gtk_window_get_child (GTK_WINDOW (win));
  g_assert_true (has_label_containing (view, "Enter your master passphrase"));

  /* Again, this time with a new PIN: today's page, and the new PIN works. */
  enter_text (find_password (view), MASTER);
  g_assert_true (wait_unlocked (j, 3000));
  dlg = find_toplevel ("Choose a new PIN");
  g_assert_nonnull (dlg);
  type_pin (GTK_WIDGET (dlg), "246810");
  type_pin (GTK_WIDGET (dlg), "246810");
  g_assert_true (dialog_gone ("Choose a new PIN", 3000));
  pump (100);
  g_assert_nonnull (jr_window_get_day_view (win));
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (100);
  view = gtk_window_get_child (GTK_WINDOW (win));
  type_pin (view, "246810");
  g_assert_true (wait_unlocked (j, 3000));

  gtk_window_close (GTK_WINDOW (win));
  pump (100);
  jr_secret_free (recovery);
  g_remove (path);
  g_rmdir (dir);
}

static void
set_raw_setting (const char *path, const char *key, const char *value)
{
  sqlite3 *db;
  sqlite3_stmt *st;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  sqlite3_prepare_v2 (db, value ? "INSERT OR REPLACE INTO settings (key, value) VALUES (?1, ?2)"
                                : "DELETE FROM settings WHERE key = ?1", -1, &st, NULL);
  sqlite3_bind_text (st, 1, key, -1, SQLITE_STATIC);
  if (value != NULL)
    sqlite3_bind_text (st, 2, value, -1, SQLITE_STATIC);
  g_assert_cmpint (sqlite3_step (st), ==, SQLITE_DONE);
  sqlite3_finalize (st);
  sqlite3_close (db);
}

static char *
raw_setting (const char *path, const char *key)
{
  sqlite3 *db;
  sqlite3_stmt *st;
  char *value = NULL;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  sqlite3_prepare_v2 (db, "SELECT value FROM settings WHERE key = ?1", -1, &st, NULL);
  sqlite3_bind_text (st, 1, key, -1, SQLITE_STATIC);
  if (sqlite3_step (st) == SQLITE_ROW)
    value = g_strdup ((const char *) sqlite3_column_text (st, 0));
  sqlite3_finalize (st);
  sqlite3_close (db);
  return value;
}

/* A PIN journal saved by the earlier version (PIN only, no master
 * passphrase, no keyring secret) opens with its PIN, then must get a
 * master passphrase before anything else; closing that dialog locks. */
static void
test_ui_master_upgrade (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  load_style ();
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = open_journal (path);
  jr_journal_add_entry (j, "2026-10-06", 1791316800, "written before the update", 1791316800);
  jr_journal_close (j);
  /* Save it the old way. */
  char *plain = raw_setting (path, "key_plain");
  JrKey *key = jr_key_import (plain);
  g_autofree char *wrapped = jr_key_wrap (key, "123456", FAST);
  set_raw_setting (path, "key_pin", wrapped);
  set_raw_setting (path, "key_plain", NULL);
  jr_key_free (key);
  jr_secret_free (plain);

  j = open_journal (path);
  g_assert_true (jr_journal_needs_master (j));
  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);
  GtkWidget *view = gtk_window_get_child (GTK_WINDOW (win));
  g_assert_null (find_button (view, "Forgot PIN? Use master passphrase")); /* none yet */
  type_pin (view, "123456");
  g_assert_true (wait_unlocked (j, 3000));
  GtkWindow *dlg = find_toplevel ("Set a master passphrase");
  g_assert_nonnull (dlg);
  shot (GTK_WIDGET (dlg), "05g-set-master");

  /* Closing it locks again. */
  gtk_window_close (dlg);
  for (int waited = 0; jr_journal_is_unlocked (j) && waited < 3000; waited += 20)
    pump (20);
  g_assert_false (jr_journal_is_unlocked (j));

  /* PIN once more, then the master passphrase twice. */
  view = gtk_window_get_child (GTK_WINDOW (win));
  type_pin (view, "123456");
  g_assert_true (wait_unlocked (j, 3000));
  dlg = find_toplevel ("Set a master passphrase");
  type_pin (GTK_WIDGET (dlg), "123456");
  enter_text (find_password (GTK_WIDGET (dlg)), MASTER);
  enter_text (find_password (GTK_WIDGET (dlg)), MASTER);
  g_assert_true (dialog_gone ("Set a master passphrase", 3000));
  pump (100);
  g_assert_false (jr_journal_needs_master (j));
  g_assert_nonnull (jr_window_get_day_view (win));

  /* The PIN now needs the keyring: the file alone does not open with it. */
  g_autofree char *bound = raw_setting (path, "key_pin");
  JrKey *alone = jr_key_unwrap_for (bound, "123456", NULL);
  g_assert_null (alone);

  /* And the master passphrase opens it. */
  jr_window_lock (win, JR_LOCK_MANUAL);
  pump (100);
  view = gtk_window_get_child (GTK_WINDOW (win));
  g_signal_emit_by_name (find_button (view, "Forgot PIN? Use master passphrase"), "clicked");
  pump (50);
  enter_text (find_password (view), MASTER);
  g_assert_true (wait_unlocked (j, 3000));
  g_assert_nonnull (jr_window_get_day_view (win));

  gtk_window_close (GTK_WINDOW (win));
  pump (100);
  g_remove (path);
  g_rmdir (dir);
}

/* Is the cursor of the focused block fully inside the visible part of
 * the page? */
static gboolean
cursor_on_screen (JrWindow *win)
{
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (win));
  if (!GTK_IS_TEXT_VIEW (focus))
    return FALSE;
  GtkTextView *tv = GTK_TEXT_VIEW (focus);
  GtkTextBuffer *buf = gtk_text_view_get_buffer (tv);
  GtkTextIter it;
  GdkRectangle r;
  int wx, wy;
  gtk_text_buffer_get_iter_at_mark (buf, &it, gtk_text_buffer_get_insert (buf));
  gtk_text_view_get_iter_location (tv, &it, &r);
  gtk_text_view_buffer_to_window_coords (tv, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
  GtkWidget *scroller = gtk_widget_get_ancestor (focus, GTK_TYPE_SCROLLED_WINDOW);
  graphene_point_t p = GRAPHENE_POINT_INIT ((float) wx, (float) wy), q;
  if (scroller == NULL || !gtk_widget_compute_point (focus, scroller, &p, &q))
    return FALSE;
  return q.y >= 0 && q.y + r.height <= gtk_widget_get_height (scroller);
}

static void
count_change (GtkAdjustment *adj, gpointer data)
{
  (void) adj;
  (*(int *) data)++;
}

/* Typing past the bottom of the page keeps the cursor in view, gliding
 * there rather than jumping, and moving the cursor back up scrolls up:
 * in a small window, at the default size and maximized. */
static void
test_ui_follow_cursor (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  load_style ();
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  gboolean animations = TRUE;
  g_object_get (gtk_settings_get_default (), "gtk-enable-animations", &animations, NULL);
  static const char *const SIZES[] = { "small", "default", "maximized" };

  for (int size = 0; size < 3; size++)
    {
      JrWindow *win = jr_window_new (NULL, open_journal (path));
      if (size == 0)
        gtk_window_set_default_size (GTK_WINDOW (win), 720, 420);
      else if (size == 2)
        gtk_window_maximize (GTK_WINDOW (win));
      gtk_window_present (GTK_WINDOW (win));
      pump (500);
      if (size == 2 && !gtk_window_is_maximized (GTK_WINDOW (win)))
        {
          g_test_message ("this display cannot maximize; skipped that size");
          gtk_window_close (GTK_WINDOW (win));
          pump (200);
          continue;
        }
      g_test_message ("window %s: %d x %d", SIZES[size], gtk_widget_get_width (GTK_WIDGET (win)),
                      gtk_widget_get_height (GTK_WIDGET (win)));

      /* Only the clock: no "time comes from your laptop clock". */
      GtkWidget *view = gtk_window_get_child (GTK_WINDOW (win));
      g_assert_true (has_label_containing (view, "· clock"));
      g_assert_false (has_label_containing (view, "laptop clock"));

      /* A new block at the end: its cursor is on screen at once. */
      JrDayView *dv = jr_window_get_day_view (win);
      jr_day_view_new_entry (dv);
      pump (400);
      g_assert_true (cursor_on_screen (win));
      GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (win));
      GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (
        GTK_SCROLLED_WINDOW (gtk_widget_get_ancestor (focus, GTK_TYPE_SCROLLED_WINDOW)));

      /* Typing line after line past the bottom: the page follows. */
      for (int line = 0; line < 30; line++)
        {
          type_text (win, "Another line of writing, long enough to take some room on the page.\n");
          pump (40);
          if (line == 29 || line % 10 == 9)
            g_assert_true (cursor_on_screen (win) || (pump (300), cursor_on_screen (win)));
        }
      pump (300);
      g_assert_true (cursor_on_screen (win));
      g_assert_cmpfloat (gtk_adjustment_get_value (adj), >, 0.0);
      if (size == 1)
        shot (GTK_WIDGET (win), "02d-typing-at-the-bottom");

      /* Back to the top of the block: the page glides up to it. */
      int changes = 0;
      gulong id = g_signal_connect (adj, "value-changed", G_CALLBACK (count_change), &changes);
      GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (focus));
      GtkTextIter start;
      gtk_text_buffer_get_start_iter (buf, &start);
      gtk_text_buffer_place_cursor (buf, &start);
      pump (500);
      g_signal_handler_disconnect (adj, id);
      g_assert_true (cursor_on_screen (win));
      if (animations)
        g_assert_cmpint (changes, >=, 3); /* several frames: a glide, not a jump */
      g_test_message ("window %s: scrolled up in %d steps", SIZES[size], changes);
      if (size == 1)
        shot (GTK_WIDGET (win), "02e-cursor-moved-up");

      gtk_window_close (GTK_WINDOW (win));
      pump (200);
    }
  g_remove (path);
  g_rmdir (dir);
}

/* Day dropdown: typed dates, bad input, and the month pages. */
static void
test_ui_day_dropdown (void)
{
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return;
    }
  g_autofree char *dir = g_dir_make_tmp ("journal-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = open_journal (path);
  seed (j);
  JrWindow *win = jr_window_new (NULL, j);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);

  GtkWidget *menu = find_type (gtk_window_get_titlebar (GTK_WINDOW (win)), GTK_TYPE_MENU_BUTTON);
  GtkPopover *pop = gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu));
  gtk_popover_set_autohide (pop, FALSE);
  jr_day_view_open_days (jr_window_get_day_view (win));
  pump (200);
  GtkWidget *root = gtk_popover_get_child (pop);

  /* Nonsense and future dates are refused in place. */
  GtkWidget *entry = find_type (root, GTK_TYPE_ENTRY);
  gtk_editable_set_text (GTK_EDITABLE (entry), "the 45th of Smarch");
  g_signal_emit_by_name (entry, "activate");
  g_assert_true (gtk_widget_has_css_class (entry, "error"));
  gtk_editable_set_text (GTK_EDITABLE (entry), "2999-01-01");
  g_signal_emit_by_name (entry, "activate");
  g_assert_true (gtk_widget_has_css_class (entry, "error"));

  /* An earlier month opens its list of written days. */
  GtkWidget *months = NULL;
  for (GtkWidget *w = find_type (root, GTK_TYPE_LIST_BOX); w != NULL; w = gtk_widget_get_next_sibling (w))
    if (GTK_IS_LIST_BOX (w))
      months = w; /* the last list on the main page is "Earlier" */
  g_assert_nonnull (months);
  GtkListBoxRow *month_row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (months), 0);
  g_assert_nonnull (month_row);
  g_signal_emit_by_name (months, "row-activated", month_row);
  pump (100);
  GtkWidget *stack = find_type (root, GTK_TYPE_STACK);
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (stack)), ==, "month");
  shot (GTK_WIDGET (pop), "03b-dropdown-month");

  /* "yesterday" opens yesterday's page (read-only). */
  gtk_editable_set_text (GTK_EDITABLE (entry), "yesterday");
  g_signal_emit_by_name (entry, "activate");
  pump (200);
  JrDayView *dv = jr_window_get_day_view (win);
  g_assert_nonnull (dv);
  g_assert_false (jr_day_view_is_current_today (dv));

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
  g_test_add_func ("/ui/save-on-quit-and-sleep", test_ui_saves_on_quit_and_sleep);
  g_test_add_func ("/ui/long-entry", test_ui_long_entry);
  g_test_add_func ("/ui/passphrase", test_ui_passphrase);
  g_test_add_func ("/ui/pin-lock-screen", test_ui_pin_lock_screen);
  g_test_add_func ("/ui/master-upgrade", test_ui_master_upgrade);
  g_test_add_func ("/ui/day-dropdown", test_ui_day_dropdown);
  g_test_add_func ("/ui/follow-cursor", test_ui_follow_cursor);
  int rc = g_test_run ();
  if (keyring != NULL)
    g_hash_table_unref (keyring);
  return rc;
}
