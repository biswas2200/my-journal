/* Load test for Notes: years of notes at once, and the things you feel
 * timed against fixed limits.
 *
 * The store: 100 subjects, 10 000 topics, 100 000 sessions of a few
 * hundred bytes, study time on every topic, plus three topics whose
 * notes are 1 MB each. Built with plain SQL in one transaction (Notes'
 * own calls would take minutes for this much).
 *
 * Limits (on a laptop; CI machines are slower, so JN_LOAD_SLACK
 * multiplies them):
 *   open Notes (store opened, checked, window shown)   1000 ms
 *   open a topic, open a session                        100 ms
 *   the longest freeze while typing (autosave included)  50 ms
 *   a save in a 1 MB note                               200 ms
 *   switch between topics                               100 ms
 *   export a topic                                      200 ms
 *   close Notes (the day's backup included)            3000 ms
 *
 * Each time is printed, so a run doubles as a measurement. */
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <sqlite3.h>
#include "notebackup.h"
#include "notes_window.h"
#include "notesjail.h"

#define SUBJECTS 100
#define TOPICS_PER_SUBJECT 100
#define SESSIONS_PER_TOPIC 10
#define BIG_NOTE (1024 * 1024)

static double slack = 1.0;

/* Lets GTK finish whatever is pending (layout, drawing), without waiting
 * longer than that. */
static void
settle (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

/* Runs the main loop for `ms` as the app would, and returns the longest
 * single stretch it was busy: a freeze you would feel while typing. */
static double
longest_freeze (int ms)
{
  gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;
  double worst = 0;
  while (g_get_monotonic_time () < end)
    {
      gint64 t = g_get_monotonic_time ();
      g_main_context_iteration (NULL, FALSE);
      worst = MAX (worst, (double) (g_get_monotonic_time () - t) / 1000.0);
      g_usleep (1000);
    }
  return worst;
}

static gint64 t_start;

static void
start (void)
{
  t_start = g_get_monotonic_time ();
}

/* Milliseconds since start(); checked against the limit (times slack). */
static double
lap (const char *what, double limit_ms)
{
  double ms = (double) (g_get_monotonic_time () - t_start) / 1000.0;
  g_test_message ("%-44s %8.1f ms  (limit %.0f)", what, ms, limit_ms * slack);
  g_printerr ("  %-44s %8.1f ms  (limit %.0f)\n", what, ms, limit_ms * slack);
  if (ms > limit_ms * slack)
    {
      g_printerr ("  ^ too slow\n");
      g_test_fail ();
    }
  return ms;
}

static void
exec (sqlite3 *h, const char *sql)
{
  char *err = NULL;
  if (sqlite3_exec (h, sql, NULL, NULL, &err) != SQLITE_OK)
    {
      /* Not g_error: its endless loop after the message trips GCC's analyzer. */
      g_printerr ("%s: %s\n", sql, err);
      abort ();
    }
}

/* Years of notes, written straight into a store Notes made. */
static void
build_store (const char *path, gint64 *big_topic, gint64 *big_session, gint64 *small_topic, gint64 *small_session)
{
  JnDb *db = jn_db_open (path, NULL);
  g_assert_nonnull (db);
  jn_db_close (db);
  sqlite3 *h;
  g_assert_cmpint (sqlite3_open (path, &h), ==, SQLITE_OK);
  exec (h, "PRAGMA synchronous = OFF; BEGIN;");
  sqlite3_stmt *subj, *top, *sess, *study;
  sqlite3_prepare_v2 (h, "INSERT INTO subjects (name, key, created) VALUES (?1, lower(?1), 1)", -1, &subj, NULL);
  sqlite3_prepare_v2 (h, "INSERT INTO topics (subject, name, key, created) VALUES (?1, ?2, lower(?2), 1)", -1, &top,
                      NULL);
  sqlite3_prepare_v2 (h, "INSERT INTO sessions (topic, started, ended, body, sources) VALUES (?1, ?2, ?3, ?4, ?5)", -1,
                      &sess, NULL);
  sqlite3_prepare_v2 (h, "INSERT INTO study_log (topic, at, seconds) VALUES (?1, ?2, 1500)", -1, &study, NULL);
  const char *body = "# Idea\n## Why it works\n- pre[i] = a[0] + ... + a[i-1]\n  - pre[0] = 0\n"
                     "\xe2\x86\x92 range sum is pre[r+1] - pre[l]\n- O(n) to build, O(1) per query\n"
                     "plain text with a little more to read, as notes usually have";
  gint64 t = 1700000000;
  for (int s = 1; s <= SUBJECTS; s++)
    {
      g_autofree char *sn = g_strdup_printf ("Subject %03d", s);
      sqlite3_bind_text (subj, 1, sn, -1, SQLITE_TRANSIENT);
      sqlite3_step (subj);
      sqlite3_reset (subj);
      gint64 sid = sqlite3_last_insert_rowid (h);
      for (int k = 1; k <= TOPICS_PER_SUBJECT; k++)
        {
          g_autofree char *tn = g_strdup_printf ("Topic %03d-%03d", s, k);
          sqlite3_bind_int64 (top, 1, sid);
          sqlite3_bind_text (top, 2, tn, -1, SQLITE_TRANSIENT);
          sqlite3_step (top);
          sqlite3_reset (top);
          gint64 tid = sqlite3_last_insert_rowid (h);
          for (int n = 0; n < SESSIONS_PER_TOPIC; n++)
            {
              t += 3600;
              sqlite3_bind_int64 (sess, 1, tid);
              sqlite3_bind_int64 (sess, 2, t);
              sqlite3_bind_int64 (sess, 3, t + 2400);
              sqlite3_bind_text (sess, 4, body, -1, SQLITE_STATIC);
              sqlite3_bind_text (sess, 5, "https://cp-algorithms.com/data_structures/prefix_sum.html", -1, SQLITE_STATIC);
              sqlite3_step (sess);
              sqlite3_reset (sess);
            }
          sqlite3_bind_int64 (study, 1, tid);
          sqlite3_bind_int64 (study, 2, t);
          sqlite3_step (study);
          sqlite3_reset (study);
          if (s == 50 && k == 50)
            {
              *small_topic = tid;
              *small_session = sqlite3_last_insert_rowid (h) - 1; /* its newest session */
            }
        }
    }
  /* Three topics with a 1 MB note each, in the last subject. */
  GString *big = g_string_new ("# A long note");
  while (big->len < BIG_NOTE)
    g_string_append (big, "\n- a point that goes on for a while, as long notes do, with pre[i] and links");
  for (int b = 0; b < 3; b++)
    {
      g_autofree char *tn = g_strdup_printf ("Big topic %d", b);
      sqlite3_bind_int64 (top, 1, SUBJECTS);
      sqlite3_bind_text (top, 2, tn, -1, SQLITE_TRANSIENT);
      sqlite3_step (top);
      sqlite3_reset (top);
      gint64 tid = sqlite3_last_insert_rowid (h);
      sqlite3_bind_int64 (sess, 1, tid);
      sqlite3_bind_int64 (sess, 2, t);
      sqlite3_bind_int64 (sess, 3, t + 60);
      sqlite3_bind_text (sess, 4, big->str, -1, SQLITE_STATIC);
      sqlite3_bind_text (sess, 5, "", -1, SQLITE_STATIC);
      sqlite3_step (sess);
      sqlite3_reset (sess);
      *big_topic = tid;
      *big_session = sqlite3_last_insert_rowid (h);
    }
  sqlite3_finalize (subj);
  sqlite3_finalize (top);
  sqlite3_finalize (sess);
  sqlite3_finalize (study);
  exec (h, "COMMIT;");
  sqlite3_close (h);
  g_string_free (big, TRUE);
}

static void
type_at_end (JnEditor *e, const char *text)
{
  GtkTextBuffer *b = gtk_text_view_get_buffer (jn_editor_view (e));
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (b, &end);
  gtk_text_buffer_place_cursor (b, &end);
  gtk_text_buffer_insert_interactive_at_cursor (b, text, -1, TRUE);
}

static void
test_load (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-load-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *exports = g_build_filename (dir, "exports", NULL);
  g_autofree char *backups = g_build_filename (dir, "backups", NULL);
  gint64 big_topic = 0, big_session = 0, topic = 0, session = 0;
  gint64 t0 = g_get_monotonic_time ();
  build_store (path, &big_topic, &big_session, &topic, &session);
  GStatBuf st;
  g_stat (path, &st);
  g_printerr ("  store: %d subjects, %d topics, %d sessions, %.0f MB (built in %.1f s)\n", SUBJECTS,
              SUBJECTS * TOPICS_PER_SUBJECT + 3, SUBJECTS * TOPICS_PER_SUBJECT * SESSIONS_PER_TOPIC + 3,
              (double) st.st_size / 1e6, (double) (g_get_monotonic_time () - t0) / 1e6);

  /* Opening Notes: the store opened and checked, the backup, the window. */
  start ();
  JnDb *db = jn_db_open (path, NULL);
  g_assert_nonnull (db);
  JnWindow *win = jn_window_new (NULL, db, exports);
  jn_window_set_backup_dir (win, backups);
  gtk_window_present (GTK_WINDOW (win));
  settle ();
  lap ("open Notes", 1000);

  start ();
  jn_window_open_topic (win, topic);
  settle ();
  lap ("open a topic", 100);

  start ();
  jn_window_open_session (win, session);
  settle ();
  lap ("open a session", 100);

  /* Typing, then the autosave a second later, as in use: the longest
   * the window is busy at any one moment. */
  JnEditor *e = jn_window_editor (win);
  double worst = 0;
  for (int i = 0; i < 3; i++)
    {
      type_at_end (e, "\n- one more point");
      double freeze = longest_freeze (1300); /* not inside MAX: it would run twice */
      worst = MAX (worst, freeze);
    }
  g_assert_nonnull (strstr (jn_window_status (win), "Saved"));
  t_start = g_get_monotonic_time () - (gint64) (worst * 1000);
  lap ("longest freeze while typing (3 autosaves)", 50);

  start ();
  jn_window_open_topic (win, topic - 1);
  jn_window_open_topic (win, topic);
  settle ();
  lap ("switch topics, there and back (2 switches)", 200);

  start ();
  g_autofree char *file = jn_window_export (win, NULL);
  lap ("export a topic", 200);
  g_assert_nonnull (file);

  start ();
  jn_window_open_session (win, big_session);
  settle ();
  lap ("open a 1 MB note", 1000);
  type_at_end (e, "\n- one more point");
  start ();
  jn_window_save (win);
  settle ();
  lap ("a save in a 1 MB note", 200);

  start ();
  gtk_window_close (GTK_WINDOW (win));
  settle ();
  lap ("close Notes (with the day's backup)", 3000);

  g_autofree char *latest = jn_backup_latest (backups);
  g_assert_nonnull (latest);

  /* Clean up. */
  const char *subdirs[] = { backups, exports };
  for (gsize i = 0; i < G_N_ELEMENTS (subdirs); i++)
    {
      g_autoptr (GDir) d = g_dir_open (subdirs[i], 0, NULL);
      for (const char *n = d != NULL ? g_dir_read_name (d) : NULL; n != NULL; n = g_dir_read_name (d))
        {
          g_autofree char *f = g_build_filename (subdirs[i], n, NULL);
          if (g_file_test (f, G_FILE_TEST_IS_DIR))
            {
              g_autoptr (GDir) sub = g_dir_open (f, 0, NULL);
              for (const char *m = g_dir_read_name (sub); m != NULL; m = g_dir_read_name (sub))
                {
                  g_autofree char *g = g_build_filename (f, m, NULL);
                  g_remove (g);
                }
              g_rmdir (f);
            }
          else
            g_remove (f);
        }
      g_rmdir (subdirs[i]);
    }
  g_remove (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_setenv ("GSK_RENDERER", "cairo", FALSE);
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return 77;
    }
  if (g_getenv ("JN_LOAD_SLACK") != NULL)
    slack = g_ascii_strtod (g_getenv ("JN_LOAD_SLACK"), NULL);
  JnJailPaths *paths = jn_jail_default_paths ();
  g_autoptr (GError) error = NULL;
  g_assert_true (jn_jail_enter (paths, &error));
  jn_jail_paths_free (paths);
  g_test_add_func ("/notes-load/years-of-notes", test_load);
  return g_test_run ();
}
