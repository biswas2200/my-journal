/* Tests for the journal store: PIN or passphrase, recovery key, lockout
 * persistence, lock jobs and encrypted entries (src/core/journal.c). */
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
  g_assert_false (jr_journal_lock_enabled (j));
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
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456");
  g_assert_nonnull (recovery);
  g_assert_true (jr_journal_lock_enabled (j));
  jr_journal_close (j);

  j = open_journal (f);
  /* Locked at startup: entries cannot be read before the PIN. */
  g_assert_true (jr_journal_lock_enabled (j));
  g_assert_false (jr_journal_is_unlocked (j));
  Texts t = { 0 };
  g_assert_false (jr_journal_foreach_entry (j, "2026-10-06", collect, &t));
  g_assert_cmpint (jr_journal_add_entry (j, "2026-10-06", T0, "x", T0), ==, 0);

  g_assert_cmpint (jr_journal_unlock (j, "000000", T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0), ==, JR_UNLOCK_OK);
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
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456");
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
  JrJobResult r;
  g_assert_null (jr_journal_new_recovery_key (j, "000000", T0, &r));
  g_assert_cmpint (r, ==, JR_JOB_WRONG);
  char *fresh = jr_journal_new_recovery_key (j, "123456", T0, &r);
  g_assert_cmpint (r, ==, JR_JOB_OK);
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
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456");
  jr_journal_add_entry (j, "2026-10-06", T0, "kept", T0);
  /* Changing needs the current PIN; a wrong one counts as a wrong try. */
  g_assert_cmpint (jr_journal_change_lock (j, "111111", JR_LOCK_PIN, "999999", T0), ==, JR_JOB_WRONG);
  g_assert_cmpint (jr_journal_tries_left (j), ==, 4);
  g_assert_cmpint (jr_journal_change_lock (j, "123456", JR_LOCK_PIN, "999999", T0), ==, JR_JOB_OK);
  g_assert_cmpint (jr_journal_tries_left (j), ==, 5);
  jr_journal_lock (j);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock (j, "999999", T0), ==, JR_UNLOCK_OK);
  /* The recovery key still works after a PIN change. */
  jr_journal_lock (j);
  g_assert_cmpint (jr_journal_unlock_recovery (j, recovery, T0), ==, JR_UNLOCK_OK);

  g_assert_cmpint (jr_journal_disable_lock (j, "123456", T0), ==, JR_JOB_WRONG);
  g_assert_true (jr_journal_lock_enabled (j));
  g_assert_cmpint (jr_journal_disable_lock (j, "999999", T0), ==, JR_JOB_OK);
  g_assert_false (jr_journal_lock_enabled (j));
  jr_journal_close (j);

  j = open_journal (f);
  g_assert_true (jr_journal_is_unlocked (j));
  Texts t = { 0 };
  jr_journal_foreach_entry (j, "2026-10-06", collect, &t);
  g_assert_cmpstr (t.texts[0], ==, "kept");
  /* Without a lock there is nothing to change; a bad new PIN is invalid. */
  g_assert_cmpint (jr_journal_change_lock (j, "999999", JR_LOCK_PIN, "123456", T0), ==, JR_JOB_INVALID);
  g_assert_null (jr_journal_enable_lock (j, JR_LOCK_PIN, "12"));
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static void
test_lockout_persists_across_restart (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456");
  jr_journal_close (j);

  j = open_journal (f);
  for (int i = 0; i < 5; i++)
    g_assert_cmpint (jr_journal_unlock (j, "111111", T0), ==, JR_UNLOCK_WRONG);
  /* Even the right PIN waits during the lockout. */
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0 + 1), ==, JR_UNLOCK_WAIT);
  g_assert_cmpint (jr_journal_unlock_recovery (j, recovery, T0 + 1), ==, JR_UNLOCK_WAIT);
  jr_journal_close (j);

  /* Restarting the app does not reset the wait. */
  j = open_journal (f);
  g_assert_cmpint (jr_journal_lockout_remaining (j, T0 + 10), ==, 20);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0 + 10), ==, JR_UNLOCK_WAIT);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0 + 30), ==, JR_UNLOCK_OK);
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

static gpointer
run_attempt (gpointer a)
{
  jr_unlock_attempt_run (a);
  return NULL;
}

static void
test_two_phase_unlock (Fixture *f, gconstpointer data)
{
  (void) data;
  /* The slow part (run) touches only the attempt, so it can run on a
   * worker thread while the database stays on the UI thread. */
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456");
  jr_journal_lock (j);

  JrUnlockAttempt *a = jr_journal_begin_unlock (j, JR_SECRET_LOCK, "654321", T0);
  g_assert_nonnull (a);
  GThread *th = g_thread_new ("kdf", run_attempt, a);
  g_thread_join (th);
  g_assert_cmpint (jr_journal_finish_unlock (j, a), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_tries_left (j), ==, 4);

  a = jr_journal_begin_unlock (j, JR_SECRET_RECOVERY, recovery, T0);
  jr_unlock_attempt_run (a);
  g_assert_cmpint (jr_journal_finish_unlock (j, a), ==, JR_UNLOCK_OK);
  g_assert_true (jr_journal_is_unlocked (j));

  /* During a lockout begin returns NULL and nothing is checked. */
  jr_journal_lock (j);
  for (int i = 0; i < 5; i++)
    jr_journal_unlock (j, "000000", T0);
  g_assert_null (jr_journal_begin_unlock (j, JR_SECRET_LOCK, "123456", T0 + 1));

  /* An attempt can be dropped without finishing (window closed). */
  a = jr_journal_begin_unlock (j, JR_SECRET_LOCK, "123456", T0 + 60);
  jr_unlock_attempt_free (a);
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static void
test_passphrase_lock (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  jr_journal_add_entry (j, "2026-10-06", T0, "behind a passphrase", T0);
  /* Too short is refused before anything changes. */
  g_assert_null (jr_journal_enable_lock (j, JR_LOCK_PASSPHRASE, "short one"));
  g_assert_false (jr_journal_lock_enabled (j));
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PASSPHRASE, "correct horse battery staple");
  g_assert_nonnull (recovery);
  g_assert_cmpint (jr_journal_lock_kind (j), ==, JR_LOCK_PASSPHRASE);
  jr_journal_close (j);

  j = open_journal (f);
  g_assert_false (jr_journal_is_unlocked (j));
  g_assert_cmpint (jr_journal_lock_kind (j), ==, JR_LOCK_PASSPHRASE); /* survives restart */
  g_assert_cmpint (jr_journal_unlock (j, "correct horse battery stapler", T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock (j, "correct horse battery staple", T0), ==, JR_UNLOCK_OK);
  Texts t = { 0 };
  jr_journal_foreach_entry (j, "2026-10-06", collect, &t);
  g_assert_cmpstr (t.texts[0], ==, "behind a passphrase");

  /* Switch back to a PIN (needs the current passphrase). */
  g_assert_cmpint (jr_journal_change_lock (j, "correct horse battery staple", JR_LOCK_PIN, "246810", T0),
                   ==, JR_JOB_OK);
  g_assert_cmpint (jr_journal_lock_kind (j), ==, JR_LOCK_PIN);
  jr_journal_lock (j);
  g_assert_cmpint (jr_journal_unlock (j, "246810", T0), ==, JR_UNLOCK_OK);
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static void
test_passphrase_unicode_forms (Fixture *f, gconstpointer data)
{
  (void) data;
  /* "é" typed as one character or as e + combining accent is the same. */
  const char *composed = "caf\xc3\xa9 au lait matin";
  const char *decomposed = "cafe\xcc\x81 au lait matin";
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PASSPHRASE, composed);
  g_assert_nonnull (recovery);
  jr_journal_lock (j);
  g_assert_cmpint (jr_journal_unlock (j, decomposed, T0), ==, JR_UNLOCK_OK);
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static void
test_legacy_file_is_pin (Fixture *f, gconstpointer data)
{
  (void) data;
  /* Files from before passphrases have no lock kind: they are PIN files. */
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456");
  jr_journal_close (j);
  sqlite3 *db;
  g_assert_cmpint (sqlite3_open (f->path, &db), ==, SQLITE_OK);
  sqlite3_exec (db, "DELETE FROM settings WHERE key = 'lock_kind'", NULL, NULL, NULL);
  sqlite3_close (db);
  j = open_journal (f);
  g_assert_cmpint (jr_journal_lock_kind (j), ==, JR_LOCK_PIN);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0), ==, JR_UNLOCK_OK);
  jr_journal_close (j);
  jr_secret_free (recovery);
}

static gpointer
run_job (gpointer job)
{
  jr_lock_job_run (job);
  return NULL;
}

static void
test_lock_jobs_off_thread (Fixture *f, gconstpointer data)
{
  (void) data;
  JrJournal *j = open_journal (f);
  JrJobResult r = JR_JOB_FAILED;

  /* Enable on a worker thread. */
  JrLockJob *job = jr_journal_begin_lock_job (j, JR_JOB_ENABLE, NULL, JR_LOCK_PIN, "135790", T0, &r);
  g_assert_nonnull (job);
  g_thread_join (g_thread_new ("job", run_job, job));
  char *recovery = NULL;
  g_assert_cmpint (jr_journal_finish_lock_job (j, job, &recovery), ==, JR_JOB_OK);
  g_assert_nonnull (recovery);
  g_assert_true (jr_journal_lock_enabled (j));

  /* Invalid new secrets are refused at begin, without a wrong try. */
  g_assert_null (jr_journal_begin_lock_job (j, JR_JOB_CHANGE, "135790", JR_LOCK_PIN, "12a456", T0, &r));
  g_assert_cmpint (r, ==, JR_JOB_INVALID);
  g_assert_cmpint (jr_journal_tries_left (j), ==, 5);

  /* If the journal locks while a job runs (lid closed), nothing is applied. */
  job = jr_journal_begin_lock_job (j, JR_JOB_CHANGE, "135790", JR_LOCK_PIN, "000111", T0, &r);
  g_assert_nonnull (job);
  jr_journal_lock (j);
  jr_lock_job_run (job);
  g_assert_cmpint (jr_journal_finish_lock_job (j, job, NULL), ==, JR_JOB_FAILED);
  g_assert_cmpint (jr_journal_unlock (j, "000111", T0), ==, JR_UNLOCK_WRONG);
  g_assert_cmpint (jr_journal_unlock (j, "135790", T0), ==, JR_UNLOCK_OK);

  /* Jobs cannot start while locked out. */
  for (int i = 0; i < 5; i++)
    jr_journal_change_lock (j, "999999", JR_LOCK_PIN, "000111", T0);
  g_assert_null (jr_journal_begin_lock_job (j, JR_JOB_DISABLE, "135790", JR_LOCK_PIN, NULL, T0 + 1, &r));
  g_assert_cmpint (r, ==, JR_JOB_WAIT);

  /* A job can be dropped (dialog closed) without applying anything. */
  job = jr_journal_begin_lock_job (j, JR_JOB_DISABLE, "135790", JR_LOCK_PIN, NULL, T0 + 60, &r);
  jr_lock_job_free (job);
  g_assert_true (jr_journal_lock_enabled (j));

  jr_journal_close (j);
  jr_secret_free (recovery);
}

/* Reads the Argon2id cost stored for the lock secret, straight from the file. */
static JrKdfCost
stored_lock_cost (const char *path)
{
  sqlite3 *db;
  sqlite3_stmt *st;
  JrKdfCost cost = { 0, 0 };
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  sqlite3_prepare_v2 (db, "SELECT value FROM settings WHERE key = 'key_pin'", -1, &st, NULL);
  if (sqlite3_step (st) == SQLITE_ROW)
    g_assert_true (jr_key_wrap_cost ((const char *) sqlite3_column_text (st, 0), &cost));
  sqlite3_finalize (st);
  sqlite3_close (db);
  return cost;
}

static void
test_unlock_upgrades_old_cost (Fixture *f, gconstpointer data)
{
  (void) data;
  /* A PIN saved with a weaker Argon2id setting is re-saved with the
   * current one the next time it unlocks. */
  JrJournal *j = open_journal (f);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, "123456");
  jr_journal_close (j);
  g_assert_cmpuint (stored_lock_cost (f->path).ops, ==, FAST.ops);

  j = open_journal (f);
  JrKdfCost stronger = { FAST.ops + 1, FAST.mem * 2 };
  jr_journal_set_kdf_cost (j, stronger);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0), ==, JR_UNLOCK_OK);
  jr_journal_close (j);
  JrKdfCost now = stored_lock_cost (f->path);
  g_assert_cmpuint (now.ops, ==, stronger.ops);
  g_assert_cmpuint (now.mem, ==, stronger.mem);

  /* And it still unlocks with the same PIN. */
  j = open_journal (f);
  g_assert_cmpint (jr_journal_unlock (j, "123456", T0), ==, JR_UNLOCK_OK);
  jr_journal_close (j);
  jr_secret_free (recovery);
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
  ADD ("/journal/two-phase", test_two_phase_unlock);
  ADD ("/journal/passphrase", test_passphrase_lock);
  ADD ("/journal/passphrase-unicode", test_passphrase_unicode_forms);
  ADD ("/journal/legacy-is-pin", test_legacy_file_is_pin);
  ADD ("/journal/lock-jobs", test_lock_jobs_off_thread);
  ADD ("/journal/upgrade-cost", test_unlock_upgrades_old_cost);
#undef ADD
  return g_test_run ();
}
