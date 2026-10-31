/* Tests for Notes' backups and rescue copies (src/notes/core/notebackup.c
 * and the copy and check in notesdb.c), LeetCode style (see cases.h).
 *
 * Constraints: a backup is always a whole, sound copy that opens with
 * every note in it; making one never touches anything in its folder but
 * backups; a damaged store is never copied over a good backup, and never
 * opened. A rescue copy is the note's text on disk, whatever the store
 * does. */
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include "cases.h"
#include "notebackup.h"
#include "notesdb.h"

/* 1 Nov 2026 12:00 in Kolkata. */
#define NOV1 G_GINT64_CONSTANT (1793514600)
#define DAY  (24 * 60 * 60)

/* ---- names --------------------------------------------------------------- */

static void
test_names (void)
{
  static const struct { const char *name; gint64 now; const char *want; } t[] = {
    { "a day",                      NOV1,                "notes-2026-11-01.db" },
    { "just after local midnight",  NOV1 - 12 * 3600 + 60, "notes-2026-11-01.db" },
    { "just before local midnight", NOV1 - 12 * 3600 - 60, "notes-2026-10-31.db" },
    { "a year later",               NOV1 + 365 * DAY,    "notes-2027-11-01.db" },
  };
  JrCases c = { "backup name", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      g_autofree char *got = jn_backup_name (t[i].now);
      jr_case (&c, t[i].name, g_strcmp0 (got, t[i].want) == 0, "Expected %s  Got %s", t[i].want, got);
    }
  jr_cases_done (&c);

  static const struct { const char *name, *subject, *topic; const char *want; } r[] = {
    { "plain",           "DSA", "Prefix sum", "DSA - Prefix sum 2026-11-01 12.00.00.md" },
    { "slashes go",      "C/C++", "a/b", "C-C++ - a-b 2026-11-01 12.00.00.md" },
    { "no leading dot",  ".hidden", "x", "hidden - x 2026-11-01 12.00.00.md" },
  };
  JrCases d = { "rescue name", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (r); i++)
    {
      g_autofree char *got = jn_rescue_file_name (r[i].subject, r[i].topic, NOV1);
      jr_case (&d, r[i].name, g_strcmp0 (got, r[i].want) == 0, "Expected %s  Got %s", r[i].want, got);
    }
  jr_cases_done (&d);
}

/* ---- pruning --------------------------------------------------------------- */

static void
test_prune (void)
{
  static const struct {
    const char *name;
    const char *names; /* a folder listing, space-separated */
    guint       keep;
    const char *want;  /* removed, oldest first */
  } t[] = {
    { "under the limit",     "notes-2026-11-01.db notes-2026-10-31.db", 3, "" },
    { "at the limit",        "notes-2026-11-01.db notes-2026-10-31.db notes-2026-10-30.db", 3, "" },
    { "the oldest go",       "notes-2026-11-01.db notes-2026-10-01.db notes-2026-10-31.db notes-2026-09-30.db", 2,
                             "notes-2026-09-30.db notes-2026-10-01.db" },
    { "across years",        "notes-2027-01-01.db notes-2026-12-31.db", 1, "notes-2026-12-31.db" },
    { "other files stay",    "notes-2026-11-01.db readme.txt notes.db notes-2026-10-31.db.tmp notes-26-1-1.db "
                             "notes-2026-13-01.db.bak my-notes-2026-01-01.db notes-2026-10-30.db", 1,
                             "notes-2026-10-30.db" },
    { "keep none is keep one", "notes-2026-11-01.db notes-2026-10-31.db", 0, "notes-2026-10-31.db" },
    { "empty folder",        "", 3, "" },
  };
  JrCases c = { "prune", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      /* g_strsplit gives { "" } for "": an empty folder has no names. */
      g_auto (GStrv) names = *t[i].names != '\0' ? g_strsplit (t[i].names, " ", -1) : g_new0 (char *, 1);
      g_auto (GStrv) gone = jn_backup_prune ((const char *const *) names, t[i].keep);
      g_autofree char *got = g_strjoinv (" ", gone);
      jr_case (&c, t[i].name, g_strcmp0 (got, t[i].want) == 0, "Input %s keep %u  Expected \"%s\"  Got \"%s\"",
               t[i].names, t[i].keep, t[i].want, got);
    }
  jr_cases_done (&c);
}

/* ---- the store's check and copy ------------------------------------------- */

static JnDb *
store_with_a_note (const char *path, gint64 *session)
{
  JnDb *db = jn_db_open (path, NULL);
  g_assert_nonnull (db);
  gint64 subject = jn_db_add_subject (db, "DSA");
  gint64 topic = jn_db_add_topic (db, subject, "Prefix sum");
  *session = jn_db_start_session (db, topic, 100);
  g_assert_true (jn_db_save_session (db, *session, "# Idea\n- pre[0] = 0", "https://cp-algorithms.com", 160));
  return db;
}

static char *
body_in (const char *path, gint64 session)
{
  JnDb *db = jn_db_open (path, NULL);
  if (db == NULL)
    return NULL;
  JnSession s = { 0 };
  char *body = jn_db_get_session (db, session, &s) ? g_strdup (s.body) : NULL;
  jn_session_clear (&s);
  jn_db_close (db);
  return body;
}

/* Bytes in the middle of the file, as a failing disk or a bad copy
 * would leave them; the file is still an SQLite file. */
static void
damage (const char *path)
{
  g_autofree char *data = NULL;
  gsize len = 0;
  g_assert_true (g_file_get_contents (path, &data, &len, NULL));
  g_assert_cmpuint (len, >, 8192);
  for (gsize i = 4096 + 8; i < 4096 + 200; i++)
    data[i] = (char) 0xA5;
  g_assert_true (g_file_set_contents (path, data, (gssize) len, NULL));
}

static void
test_copy (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-backup-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *copy = g_build_filename (dir, "copy.db", NULL);
  gint64 session = 0;
  JnDb *db = store_with_a_note (path, &session);
  g_assert_true (jn_db_check (db));

  /* A copy while the store is open: whole, private, every note in it. */
  g_autoptr (GError) error = NULL;
  g_assert_true (jn_db_copy_to (db, copy, &error));
  g_assert_no_error (error);
  struct stat st;
  g_assert_cmpint (g_stat (copy, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_autofree char *tmp = g_strconcat (copy, ".tmp", NULL);
  g_assert_false (g_file_test (tmp, G_FILE_TEST_EXISTS));
  g_autofree char *in_copy = body_in (copy, session);
  g_assert_cmpstr (in_copy, ==, "# Idea\n- pre[0] = 0");

  /* A copy cut off before (a kill) left its file and SQLite's journal
   * beside it: the next copy clears them and is sound. */
  g_autofree char *stale_journal = g_strconcat (copy, ".tmp-journal", NULL);
  g_assert_true (g_file_set_contents (tmp, "half a copy", -1, NULL));
  g_assert_true (g_file_set_contents (stale_journal, "a journal from a copy that was cut off", -1, NULL));
  g_assert_true (jn_db_copy_to (db, copy, NULL));
  g_assert_false (g_file_test (tmp, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (stale_journal, G_FILE_TEST_EXISTS));
  g_autofree char *after_stale = body_in (copy, session);
  g_assert_cmpstr (after_stale, ==, "# Idea\n- pre[0] = 0");

  /* A later copy replaces it with the later text. */
  g_assert_true (jn_db_save_session (db, session, "# Idea\n- pre[0] = 0\n- more", "", 200));
  g_assert_true (jn_db_copy_to (db, copy, NULL));
  g_autofree char *in_copy2 = body_in (copy, session);
  g_assert_cmpstr (in_copy2, ==, "# Idea\n- pre[0] = 0\n- more");

  /* Read-only (as on a full disk): changes fail, reading still works. */
  jn_db_set_read_only (db, TRUE);
  g_test_expect_message ("journal", G_LOG_LEVEL_WARNING, "SQLite:*");
  g_assert_false (jn_db_save_session (db, session, "lost?", "", 300));
  g_test_assert_expected_messages ();
  JnSession s = { 0 };
  g_assert_true (jn_db_get_session (db, session, &s));
  g_assert_cmpstr (s.body, ==, "# Idea\n- pre[0] = 0\n- more");
  jn_session_clear (&s);
  jn_db_set_read_only (db, FALSE);
  g_assert_true (jn_db_save_session (db, session, "# Idea\n- back", "", 300));
  jn_db_close (db);

  /* A damaged store is not opened, and is left as it was. */
  g_autofree char *before = NULL;
  damage (path);
  gsize n = 0;
  g_assert_true (g_file_get_contents (path, &before, &n, NULL));
  g_clear_error (&error);
  g_assert_null (jn_db_open (path, &error));
  g_assert_nonnull (error);
  g_assert_nonnull (strstr (error->message, "damaged"));
  g_autofree char *after = NULL;
  gsize m = 0;
  g_assert_true (g_file_get_contents (path, &after, &m, NULL));
  g_assert_true (n == m && memcmp (before, after, n) == 0);

  /* The good copy still opens. */
  g_autofree char *in_copy3 = body_in (copy, session);
  g_assert_cmpstr (in_copy3, ==, "# Idea\n- pre[0] = 0\n- more");

  g_remove (path);
  g_remove (copy);
  g_rmdir (dir);
}

/* A damaged store is refused when Notes opens, so it never reaches the
 * backups: the last good copy stays as it was. */
static void
test_no_damaged_copy (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-backup-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *copy = g_build_filename (dir, "copy.db", NULL);
  gint64 session = 0;
  JnDb *db = store_with_a_note (path, &session);
  /* Enough rows for a second page. */
  for (int i = 0; i < 40; i++)
    {
      g_autofree char *name = g_strdup_printf ("Topic %d with a long enough name to fill pages", i);
      jn_db_add_topic (db, 1, name);
    }
  g_assert_true (jn_db_copy_to (db, copy, NULL));
  jn_db_close (db);
  damage (path);
  g_autofree char *good = NULL;
  gsize n = 0;
  g_assert_true (g_file_get_contents (copy, &good, &n, NULL));
  g_autoptr (GError) error = NULL;
  db = jn_db_open (path, &error);
  g_assert_null (db); /* refused, so there is nothing to copy */
  g_autofree char *still = NULL;
  gsize m = 0;
  g_assert_true (g_file_get_contents (copy, &still, &m, NULL));
  g_assert_true (n == m && memcmp (good, still, n) == 0);
  g_remove (path);
  g_remove (copy);
  g_rmdir (dir);
}

/* ---- daily backups in a folder ------------------------------------------- */

static void
test_backup_now (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-backup-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *backups = g_build_filename (dir, "backups", NULL);
  gint64 session = 0;
  JnDb *db = store_with_a_note (path, &session);

  /* Something else in the folder is never touched. */
  g_assert_cmpint (g_mkdir (backups, 0755), ==, 0);
  g_autofree char *other = g_build_filename (backups, "keep-me.txt", NULL);
  g_assert_true (g_file_set_contents (other, "mine", -1, NULL));

  /* Twenty days of use, keeping 14. */
  g_autoptr (GError) error = NULL;
  for (int d = 0; d < 20; d++)
    {
      g_assert_true (jn_backup_now (db, backups, NOV1 + d * DAY, 14, &error));
      g_assert_no_error (error);
    }
  /* The same day again replaces that day's copy. */
  g_assert_true (jn_db_save_session (db, session, "# Idea\n- last", "", 999));
  g_assert_true (jn_backup_now (db, backups, NOV1 + 19 * DAY + 60, 14, NULL));

  int count = 0;
  g_autoptr (GDir) list = g_dir_open (backups, 0, NULL);
  for (const char *name = g_dir_read_name (list); name != NULL; name = g_dir_read_name (list))
    count += g_str_has_prefix (name, "notes-");
  g_assert_cmpint (count, ==, 14);
  g_assert_true (g_file_test (other, G_FILE_TEST_EXISTS));
  g_autofree char *oldest = g_build_filename (backups, "notes-2026-11-07.db", NULL);
  g_autofree char *gone = g_build_filename (backups, "notes-2026-11-06.db", NULL);
  g_assert_true (g_file_test (oldest, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (gone, G_FILE_TEST_EXISTS));

  g_autofree char *latest = jn_backup_latest (backups);
  g_autofree char *want = g_build_filename (backups, "notes-2026-11-20.db", NULL);
  g_assert_cmpstr (latest, ==, want);
  g_autofree char *body = body_in (latest, session);
  g_assert_cmpstr (body, ==, "# Idea\n- last");
  g_autofree char *older_body = body_in (oldest, session);
  g_assert_cmpstr (older_body, ==, "# Idea\n- pre[0] = 0");

  /* A folder that cannot be made: an error, the store untouched. */
  g_autofree char *blocked = g_build_filename (dir, "file-not-folder", NULL);
  g_assert_true (g_file_set_contents (blocked, "", 0, NULL));
  g_autofree char *inside = g_build_filename (blocked, "backups", NULL);
  g_clear_error (&error);
  g_assert_false (jn_backup_now (db, inside, NOV1, 14, &error));
  g_assert_nonnull (error);
  JnSession s = { 0 };
  g_assert_true (jn_db_get_session (db, session, &s));
  g_assert_cmpstr (s.body, ==, "# Idea\n- last");
  jn_session_clear (&s);
  jn_db_close (db);

  g_assert_null (jn_backup_latest (inside));
  /* Clean up. */
  g_autoptr (GDir) again = g_dir_open (backups, 0, NULL);
  for (const char *name = g_dir_read_name (again); name != NULL; name = g_dir_read_name (again))
    {
      g_autofree char *f = g_build_filename (backups, name, NULL);
      g_remove (f);
    }
  g_rmdir (backups);
  g_remove (blocked);
  g_remove (path);
  g_rmdir (dir);
}

/* ---- rescue copies --------------------------------------------------------- */

static void
test_rescue (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-rescue-XXXXXX", NULL);
  g_autofree char *unsaved = g_build_filename (dir, "Unsaved", NULL);
  g_autoptr (GError) error = NULL;
  g_assert_true (jn_rescue_write (unsaved, "DSA - x.md", "# Idea\n- keep me", &error));
  g_assert_no_error (error);
  g_autofree char *file = g_build_filename (unsaved, "DSA - x.md", NULL);
  g_autofree char *text = NULL;
  g_assert_true (g_file_get_contents (file, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "# Idea\n- keep me");
  struct stat st;
  g_assert_cmpint (g_stat (file, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_assert_cmpint (g_stat (unsaved, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);

  /* Written again: the newer text, whole. */
  g_assert_true (jn_rescue_write (unsaved, "DSA - x.md", "# Idea\n- keep me\n- and me", NULL));
  g_autofree char *text2 = NULL;
  g_assert_true (g_file_get_contents (file, &text2, NULL, NULL));
  g_assert_cmpstr (text2, ==, "# Idea\n- keep me\n- and me");

  /* Nowhere to write: an error, nothing pretended. */
  g_autofree char *blocked = g_build_filename (dir, "file", NULL);
  g_assert_true (g_file_set_contents (blocked, "", 0, NULL));
  g_autofree char *inside = g_build_filename (blocked, "Unsaved", NULL);
  g_clear_error (&error);
  g_assert_false (jn_rescue_write (inside, "a.md", "x", &error));
  g_assert_nonnull (error);

  g_remove (file);
  g_rmdir (unsaved);
  g_remove (blocked);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Asia/Kolkata", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/notebackup/names", test_names);
  g_test_add_func ("/notebackup/prune", test_prune);
  g_test_add_func ("/notebackup/copy", test_copy);
  g_test_add_func ("/notebackup/no-damaged-copy", test_no_damaged_copy);
  g_test_add_func ("/notebackup/backup-now", test_backup_now);
  g_test_add_func ("/notebackup/rescue", test_rescue);
  return g_test_run ();
}
