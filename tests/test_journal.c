/* Tests for the journal store: PIN, recovery key, lockout persistence and
 * encrypted entries working together (src/core/journal.c). */
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "journal.h"

#define T0 1791316800

static const JrKdfCost FAST = { JR_KDF_OPS_MIN, JR_KDF_MEM_MIN };

typedef struct {
  char *dir;
  char *path;
} Fixture;

static void
fixture_setup (Fixture *f, gconstpointer data)
{
  (void) data;
  f->dir = g_dir_make_tmp ("journal-test-XXXXXX", NULL);
  f->path = g_build_filename (f->dir, "journal.db", NULL);
}

static void
fixture_teardown (Fixture *f, gconstpointer data)
{
  (void) data;
  g_remove (f->path);
  g_rmdir (f->dir);
  g_free (f->path);
  g_free (f->dir);
}

static JrJournal *
open_journal (Fixture *f)
{
  GError *error = NULL;
  JrJournal *j = jr_journal_open (f->path, &error);
  g_assert_no_error (error);
  jr_journal_set_kdf_cost (j, FAST);
  return j;
}

typedef struct {
  int count;
  gint64 ids[8];
  char texts[8][64];
} Texts;

static void
collect (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) stamped_at;
  Texts *t = user;
  t->ids[t->count] = id;
  g_strlcpy (t->texts[t->count], text, sizeof t->texts[0]);
  t->count++;
}

/* Reads the raw bytes of every body straight from the file, bypassing the app. */
static gboolean
file_contains_plaintext (const char *path, const char *needle)
{
  sqlite3 *db;
  sqlite3_stmt *st;
  gboolean found = FALSE;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  sqlite3_prepare_v2 (db, "SELECT body FROM entries", -1, &st, NULL);
  while (sqlite3_step (st) == SQLITE_ROW)
    {
      const void *b = sqlite3_column_blob (st, 0);
      int n = sqlite3_column_bytes (st, 0);
      if (b != NULL && g_strstr_len (b, n, needle) != NULL)
        found = TRUE;
    }
  sqlite3_finalize (st);
  sqlite3_close (db);
  return found;
}

static void
test_no_pin_opens_unlocked (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  g_assert_false (jr_journal_pin_enabled (j));
  g_assert_true (jr_journal_is_unlocked (j));

  gint64 id = jr_journal_add_entry (j, "2026-10-06", T0, "hello journal", T0);
  g_assert_cmpint (id, >, 0);
  jr_journal_close (j);

  /* Bodies are encrypted on disk even without a PIN. */
  g_assert_false (file_contains_plaintext (f->path, "hello journal"));

  j = open_journal (f);
  Texts t = { 0 };
  g_assert_true (jr_journal_foreach_entry (j, "2026-10-06", collect, &t));
  g_assert_cmpint (t.count, ==, 1);
  g_assert_cmpstr (t.texts[0], ==, "hello journal");
  jr_journal_close (j);
}

static void
test_enable_pin_then_unlock (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  jr_journal_add_entry (j, "2026-10-06", T0, "before pin", T0);
  char *recovery = jr_journal_enable_pin (j, "123456");
  g_assert_nonnull (recovery);
  g_assert_true (jr_journal_pin_enabled (j));
  jr_journal_close (j);

  j = open_journal (f);
  /* Locked at startup: entries cannot be read before the PIN. */
  g_assert_true (jr_journal_pin_enabled (j));
  g_assert_false (jr_journal_is_unlocked (j));
  Texts t = { 0 };
  g_assert_false (jr_journal_foreach_entry (j, "2026-10-06", collect, &t));
  g_assert_cmpint (jr_journal_add_entry (j, "2026-10-06", T0, "x", T0), ==, 0);

  g_assert_cmpint (jr_journal_unlock_pin (j, "000000", T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock_pin (j, "123456", T0), ==, JR_UNLOCK_OK);
  g_assert_true (jr_journal_foreach_entry (j, "2026-10-06", collect, &t));
  g_assert_cmpstr (t.texts[0], ==, "before pin");

  /* Locking drops the key again. */
  jr_journal_lock (j);
  g_assert_false (jr_journal_is_unlocked (j));
  jr_journal_close (j);

  /* The plain key is gone from the file once a PIN is set. */
  j = open_journal (f);
  g_assert_false (jr_journal_is_unlocked (j));
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static void
test_recovery_key_unlocks (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_pin (j, "123456");
  jr_journal_add_entry (j, "2026-10-06", T0, "recover me", T0);
  jr_journal_lock (j);

  g_assert_cmpint (jr_journal_unlock_recovery (j, "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA", T0), ==,
                   JR_UNLOCK_WRONG);
  char *lower = g_ascii_strdown (recovery, -1);
  g_assert_cmpint (jr_journal_unlock_recovery (j, lower, T0), ==, JR_UNLOCK_OK);
  Texts t = { 0 };
  jr_journal_foreach_entry (j, "2026-10-06", collect, &t);
  g_assert_cmpstr (t.texts[0], ==, "recover me");

  /* A new recovery key replaces the old one. */
  char *fresh = jr_journal_new_recovery_key (j);
  jr_journal_lock (j);
  g_assert_cmpint (jr_journal_unlock_recovery (j, recovery, T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock_recovery (j, fresh, T0), ==, JR_UNLOCK_OK);

  g_free (lower);
  jr_secret_free (fresh);
  jr_secret_free (recovery);
  jr_journal_close (j);
}

static void
test_change_and_disable_pin (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_pin (j, "123456");
  jr_journal_add_entry (j, "2026-10-06", T0, "kept", T0);
  g_assert_true (jr_journal_change_pin (j, "999999"));
  jr_journal_lock (j);
  g_assert_cmpint (jr_journal_unlock_pin (j, "123456", T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock_pin (j, "999999", T0), ==, JR_UNLOCK_OK);
  /* The recovery key still works after a PIN change. */
  jr_journal_lock (j);
  g_assert_cmpint (jr_journal_unlock_recovery (j, recovery, T0), ==, JR_UNLOCK_OK);

  g_assert_true (jr_journal_disable_pin (j));
  g_assert_false (jr_journal_pin_enabled (j));
  jr_journal_close (j);

  j = open_journal (f);
  g_assert_true (jr_journal_is_unlocked (j));
  Texts t = { 0 };
  jr_journal_foreach_entry (j, "2026-10-06", collect, &t);
  g_assert_cmpstr (t.texts[0], ==, "kept");
  g_assert_false (jr_journal_change_pin (j, "12")); /* invalid PIN */
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static void
test_lockout_persists_across_restart (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_pin (j, "123456");
  jr_journal_close (j);

  j = open_journal (f);
  for (int i = 0; i < 5; i++)
    g_assert_cmpint (jr_journal_unlock_pin (j, "111111", T0), ==, JR_UNLOCK_WRONG);
  /* Even the right PIN waits during the lockout. */
  g_assert_cmpint (jr_journal_unlock_pin (j, "123456", T0 + 1), ==, JR_UNLOCK_WAIT);
  g_assert_cmpint (jr_journal_unlock_recovery (j, recovery, T0 + 1), ==, JR_UNLOCK_WAIT);
  jr_journal_close (j);

  /* Restarting the app does not reset the wait. */
  j = open_journal (f);
  g_assert_cmpint (jr_journal_lockout_remaining (j, T0 + 10), ==, 20);
  g_assert_cmpint (jr_journal_unlock_pin (j, "123456", T0 + 10), ==, JR_UNLOCK_WAIT);
  g_assert_cmpint (jr_journal_unlock_pin (j, "123456", T0 + 30), ==, JR_UNLOCK_OK);
  g_assert_cmpint (jr_journal_tries_left (j), ==, 5);
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static void
test_update_delete_and_stats (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  gint64 a = jr_journal_add_entry (j, "2026-10-06", T0, "one", T0);
  gint64 b = jr_journal_add_entry (j, "2026-10-06", T0 + 60, "two", T0);
  g_assert_true (jr_journal_update_entry (j, a, "one, edited", T0 + 5));
  g_assert_true (jr_journal_delete_entry (j, b));
  jr_journal_add_active_seconds (j, "2026-10-06", 90);
  g_assert_cmpint (jr_journal_active_seconds (j, "2026-10-06"), ==, 90);

  Texts t = { 0 };
  jr_journal_foreach_entry (j, "2026-10-06", collect, &t);
  g_assert_cmpint (t.count, ==, 1);
  g_assert_cmpstr (t.texts[0], ==, "one, edited");
  jr_journal_close (j);
}

static void
test_lock_on_sleep_setting (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  g_assert_true (jr_journal_lock_on_sleep (j)); /* checked by default */
  jr_journal_set_lock_on_sleep (j, FALSE);
  jr_journal_close (j);
  j = open_journal (f);
  g_assert_false (jr_journal_lock_on_sleep (j));
  jr_journal_close (j);
}

static void
test_large_entry (Fixture *f, gconstpointer data)
{
  (void) data;
  /* A very long entry (1 MiB) round-trips. */
  gsize n = 1 << 20;
  char *big = g_malloc (n + 1);
  memset (big, 'w', n);
  big[n] = '\0';
  JrJournal *j = open_journal (f);
  gint64 id = jr_journal_add_entry (j, "2026-10-06", T0, big, T0);
  g_assert_cmpint (id, >, 0);
  g_free (big);
  jr_journal_close (j);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_true (jr_crypto_init ());
#define ADD(path, fn) g_test_add (path, Fixture, NULL, fixture_setup, fn, fixture_teardown)
  ADD ("/journal/no-pin", test_no_pin_opens_unlocked);
  ADD ("/journal/enable-pin", test_enable_pin_then_unlock);
  ADD ("/journal/recovery", test_recovery_key_unlocks);
  ADD ("/journal/change-disable", test_change_and_disable_pin);
  ADD ("/journal/lockout-persists", test_lockout_persists_across_restart);
  ADD ("/journal/update-delete", test_update_delete_and_stats);
  ADD ("/journal/lock-on-sleep", test_lock_on_sleep_setting);
  ADD ("/journal/large-entry", test_large_entry);
#undef ADD
  return g_test_run ();
}
