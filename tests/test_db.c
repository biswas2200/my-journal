/* Tests for the SQLite layer (src/core/db.c). */
#include <string.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "db.h"

typedef struct {
  int count;
  gint64 ids[8];
  gint64 stamps[8];
  char bodies[8][32];
} Rows;

static void
collect_entry (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  Rows *r = user;
  g_assert_cmpint (r->count, <, 8);
  r->ids[r->count] = id;
  r->stamps[r->count] = stamped_at;
  g_assert_cmpuint (len, <, sizeof r->bodies[0]);
  memcpy (r->bodies[r->count], body, len);
  r->bodies[r->count][len] = '\0';
  r->count++;
}

static JrDb *
open_memory (void)
{
  GError *error = NULL;
  JrDb *db = jr_db_open (":memory:", &error);
  g_assert_no_error (error);
  g_assert_nonnull (db);
  return db;
}

static void
test_settings (void)
{
  JrDb *db = open_memory ();
  g_assert_null (jr_db_get_setting (db, "missing"));
  g_assert_true (jr_db_set_setting (db, "a", "1"));
  g_assert_true (jr_db_set_setting (db, "a", "2"));
  char *v = jr_db_get_setting (db, "a");
  g_assert_cmpstr (v, ==, "2");
  g_free (v);
  g_assert_true (jr_db_delete_setting (db, "a"));
  g_assert_null (jr_db_get_setting (db, "a"));
  jr_db_close (db);
}

static void
test_entries_crud_and_order (void)
{
  JrDb *db = open_memory ();
  gint64 later = jr_db_insert_entry (db, "2026-10-06", 2000, "evening", 7, 2000);
  gint64 early = jr_db_insert_entry (db, "2026-10-06", 1000, "morning", 7, 1000);
  jr_db_insert_entry (db, "2026-10-05", 500, "other", 5, 500);
  g_assert_cmpint (later, >, 0);
  g_assert_cmpint (early, >, 0);

  Rows r = { 0 };
  jr_db_foreach_entry (db, "2026-10-06", collect_entry, &r);
  g_assert_cmpint (r.count, ==, 2);
  /* Ordered by stamp time, not insertion. */
  g_assert_cmpint (r.ids[0], ==, early);
  g_assert_cmpstr (r.bodies[0], ==, "morning");
  g_assert_cmpstr (r.bodies[1], ==, "evening");

  g_assert_true (jr_db_update_entry (db, early, "morning!", 8, 3000));
  g_assert_true (jr_db_delete_entry (db, later));
  memset (&r, 0, sizeof r);
  jr_db_foreach_entry (db, "2026-10-06", collect_entry, &r);
  g_assert_cmpint (r.count, ==, 1);
  g_assert_cmpstr (r.bodies[0], ==, "morning!");
  g_assert_cmpint (r.stamps[0], ==, 1000); /* the stamp never changes */
  jr_db_close (db);
}

static void
test_binary_body (void)
{
  /* Encrypted bodies contain NUL bytes; they must round-trip intact. */
  JrDb *db = open_memory ();
  const char blob[5] = { 'a', '\0', 'b', '\0', 'c' };
  jr_db_insert_entry (db, "2026-10-06", 1, blob, sizeof blob, 1);
  Rows r = { 0 };
  jr_db_foreach_entry (db, "2026-10-06", collect_entry, &r);
  g_assert_cmpint (memcmp (r.bodies[0], blob, sizeof blob), ==, 0);
  jr_db_close (db);
}

static void
test_active_seconds_accumulate (void)
{
  JrDb *db = open_memory ();
  g_assert_cmpint (jr_db_active_seconds (db, "2026-10-06"), ==, 0);
  g_assert_true (jr_db_add_active_seconds (db, "2026-10-06", 15));
  g_assert_true (jr_db_add_active_seconds (db, "2026-10-06", 15));
  g_assert_cmpint (jr_db_active_seconds (db, "2026-10-06"), ==, 30);
  jr_db_close (db);
}

typedef struct {
  int count;
  char days[8][JR_DB_DAY_LEN];
  int entries[8];
  gint64 seconds[8];
} Days;

static void
collect_day (const char *day, int entries, gint64 seconds, gpointer user)
{
  Days *d = user;
  g_assert_cmpint (d->count, <, 8);
  g_strlcpy (d->days[d->count], day, JR_DB_DAY_LEN);
  d->entries[d->count] = entries;
  d->seconds[d->count] = seconds;
  d->count++;
}

static void
test_day_summary_range (void)
{
  JrDb *db = open_memory ();
  jr_db_insert_entry (db, "2026-10-01", 1, "x", 1, 1);
  jr_db_insert_entry (db, "2026-10-01", 2, "y", 1, 2);
  jr_db_add_active_seconds (db, "2026-10-01", 720);
  jr_db_add_active_seconds (db, "2026-10-03", 60); /* time but no entry */
  jr_db_insert_entry (db, "2026-10-04", 3, "z", 1, 3);
  jr_db_insert_entry (db, "2026-09-30", 4, "w", 1, 4); /* outside range */

  Days d = { 0 };
  jr_db_foreach_day (db, "2026-10-01", "2026-10-31", collect_day, &d);
  g_assert_cmpint (d.count, ==, 3);
  g_assert_cmpstr (d.days[0], ==, "2026-10-01");
  g_assert_cmpint (d.entries[0], ==, 2);
  g_assert_cmpint (d.seconds[0], ==, 720);
  g_assert_cmpstr (d.days[1], ==, "2026-10-03");
  g_assert_cmpint (d.entries[1], ==, 0);
  g_assert_cmpint (d.seconds[1], ==, 60);
  g_assert_cmpstr (d.days[2], ==, "2026-10-04");
  g_assert_cmpint (d.entries[2], ==, 1);
  g_assert_cmpint (d.seconds[2], ==, 0);
  jr_db_close (db);
}

typedef struct {
  int count;
  char months[4][8];
  int days[4];
} Months;

static void
collect_month (const char *month, int days_written, gpointer user)
{
  Months *m = user;
  g_strlcpy (m->months[m->count], month, sizeof m->months[0]);
  m->days[m->count] = days_written;
  m->count++;
}

static void
test_months_written (void)
{
  JrDb *db = open_memory ();
  jr_db_insert_entry (db, "2026-08-01", 1, "a", 1, 1);
  jr_db_insert_entry (db, "2026-09-01", 1, "a", 1, 1);
  jr_db_insert_entry (db, "2026-09-01", 2, "b", 1, 1);
  jr_db_insert_entry (db, "2026-09-15", 1, "c", 1, 1);
  jr_db_insert_entry (db, "2026-10-02", 1, "d", 1, 1);
  Months m = { 0 };
  /* Months before October, newest first. */
  jr_db_foreach_month (db, "2026-10", collect_month, &m);
  g_assert_cmpint (m.count, ==, 2);
  g_assert_cmpstr (m.months[0], ==, "2026-09");
  g_assert_cmpint (m.days[0], ==, 2);
  g_assert_cmpstr (m.months[1], ==, "2026-08");
  g_assert_cmpint (m.days[1], ==, 1);
  jr_db_close (db);
}

static void
test_file_created_private_and_persists (void)
{
  g_autofree char *dir = g_dir_make_tmp ("journal-test-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "sub", "journal.db", NULL);
  GError *error = NULL;

  JrDb *db = jr_db_open (path, &error);
  g_assert_no_error (error);
  jr_db_set_setting (db, "k", "v");
  jr_db_close (db);

  /* Only the owner may read the journal file or its folder. */
  struct stat st;
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_autofree char *sub = g_path_get_dirname (path);
  g_assert_cmpint (g_stat (sub, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);

  db = jr_db_open (path, &error);
  g_assert_no_error (error);
  char *v = jr_db_get_setting (db, "k");
  g_assert_cmpstr (v, ==, "v");
  g_free (v);
  jr_db_close (db);

  g_remove (path);
  g_rmdir (sub);
  g_rmdir (dir);
}

static void
test_transaction_rollback (void)
{
  JrDb *db = open_memory ();
  g_assert_true (jr_db_begin (db));
  jr_db_set_setting (db, "k", "v");
  jr_db_rollback (db);
  g_assert_null (jr_db_get_setting (db, "k"));
  g_assert_true (jr_db_begin (db));
  jr_db_set_setting (db, "k", "v");
  g_assert_true (jr_db_commit (db));
  char *v = jr_db_get_setting (db, "k");
  g_assert_cmpstr (v, ==, "v");
  g_free (v);
  jr_db_close (db);
}

static void
test_open_bad_path_fails (void)
{
  GError *error = NULL;
  JrDb *db = jr_db_open ("/proc/no-such-dir/journal.db", &error);
  g_assert_null (db);
  g_assert_nonnull (error);
  g_error_free (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/db/settings", test_settings);
  g_test_add_func ("/db/entries", test_entries_crud_and_order);
  g_test_add_func ("/db/binary-body", test_binary_body);
  g_test_add_func ("/db/active-seconds", test_active_seconds_accumulate);
  g_test_add_func ("/db/day-summary", test_day_summary_range);
  g_test_add_func ("/db/months", test_months_written);
  g_test_add_func ("/db/file", test_file_created_private_and_persists);
  g_test_add_func ("/db/transaction", test_transaction_rollback);
  g_test_add_func ("/db/bad-path", test_open_bad_path_fails);
  return g_test_run ();
}
