/* Writes the seeds that need the app's own crypto:
 *
 *   make_seeds JOURNAL_DIR SECRETS_DIR
 *
 * Journals for the journal_file target, and a sealed entry and wrapped
 * keys for the secrets target.
 * Each is a real journal made through the app's own code, with Argon2id
 * at its minimum (so the target can unlock it quickly), the PIN's device
 * secret saved in the file (see fuzz_store.h), and SQLite pages of 512
 * bytes so the files stay small. No real journal is ever read. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "fuzz_store.h"

#define NOW G_GINT64_CONSTANT (1791316800)

static void G_GNUC_NORETURN G_GNUC_PRINTF (1, 2)
die (const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  g_autofree char *msg = g_strdup_vprintf (format, ap);
  va_end (ap);
  g_printerr ("make_seeds: %s\n", msg);
  exit (1);
}

static void
add_entries (JrJournal *j)
{
  jr_journal_add_entry (j, "2026-10-05", NOW - 86400, "Slow start today.", NOW - 86400);
  jr_journal_add_entry (j, "2026-10-06", NOW, "Two chapters done", NOW);
  jr_journal_add_entry (j, "2026-10-06", NOW + 60, "na\xc3\xafve caf\xc3\xa9 \xe2\x9d\xa4\xef\xb8\x8f", NOW + 60);
  jr_journal_add_entry (j, "2026-09-30", NOW - 6 * 86400, "", NOW - 6 * 86400);
  jr_journal_add_active_seconds (j, "2026-10-06", 720);
  jr_journal_add_active_seconds (j, "2026-10-03", 60);
  jr_journal_set_lock_on_sleep (j, TRUE);
}

typedef enum { PLAIN, PIN, PASSPHRASE, BLOCKED } Kind;

static void
make (const char *dir, const char *name, Kind kind)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_remove (path);
  FuzzStore store = { NULL };
  const JrDeviceStore keyring = { fuzz_store_load, fuzz_store_save, fuzz_store_clear, &store };
  GError *error = NULL;
  JrJournal *j = jr_journal_open (path, &error);
  if (j == NULL)
    die ("%s: %s", path, error->message);
  jr_journal_set_kdf_cost (j, FUZZ_FAST);
  jr_journal_set_device_store (j, &keyring);
  add_entries (j);
  char *recovery = NULL;
  if (kind == PIN || kind == BLOCKED)
    recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, FUZZ_PIN, FUZZ_MASTER);
  else if (kind == PASSPHRASE)
    recovery = jr_journal_enable_lock (j, JR_LOCK_PASSPHRASE, FUZZ_PASSPHRASE, NULL);
  if (kind != PLAIN && recovery == NULL)
    die ("%s: could not set the lock", path);
  jr_secret_free (recovery);
  if (kind == BLOCKED)
    {
      jr_journal_lock (j);
      for (int i = 0; i < 5; i++)
        jr_journal_unlock (j, "000000", NOW);
      jr_journal_unlock_with (j, JR_SECRET_MASTER, "wrong master passphrase", NOW);
    }
  jr_journal_close (j);

  /* The device secret goes in the file; then small pages. */
  sqlite3 *db;
  sqlite3_stmt *st;
  if (sqlite3_open (path, &db) != SQLITE_OK)
    die ("%s: %s", path, sqlite3_errmsg (db));
  if (store.secret != NULL)
    {
      char *device = jr_key_export (store.secret);
      sqlite3_prepare_v2 (db, "INSERT INTO settings(key, value) VALUES(?1, ?2)", -1, &st, NULL);
      sqlite3_bind_text (st, 1, FUZZ_DEVICE_SETTING, -1, SQLITE_STATIC);
      sqlite3_bind_text (st, 2, device, -1, SQLITE_STATIC);
      if (sqlite3_step (st) != SQLITE_DONE)
        die ("%s: %s", path, sqlite3_errmsg (db));
      sqlite3_finalize (st);
      jr_secret_free (device);
    }
  if (sqlite3_exec (db, "PRAGMA page_size = 512; VACUUM;", NULL, NULL, NULL) != SQLITE_OK)
    die ("%s: %s", path, sqlite3_errmsg (db));
  sqlite3_close (db);
  jr_key_free (store.secret);
  g_print ("wrote %s\n", path);
}

static void
write_seed (const char *dir, const char *name, const void *data, gsize len)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  if (!g_file_set_contents (path, data, (gssize) len, NULL))
    die ("cannot write %s", path);
  g_print ("wrote %s\n", path);
}

/* A sealed entry the secrets target can open, and wrapped keys of both
 * kinds as saved in the file. */
static void
make_secrets (const char *dir)
{
  JrKey *key = jr_key_import (FUZZ_FIXED_KEY), *device = jr_key_generate ();
  static const char text[] = "Two chapters done";
  gsize len = 0;
  guint8 *sealed = jr_seal (key, text, sizeof text - 1, &len);
  write_seed (dir, "sealed.bin", sealed, len);
  g_free (sealed);
  char *plain = jr_key_wrap_for (key, FUZZ_PASSPHRASE, NULL, FUZZ_FAST);
  char *bound = jr_key_wrap_for (key, FUZZ_PIN, device, FUZZ_FAST);
  write_seed (dir, "wrapped-v1.txt", plain, strlen (plain));
  write_seed (dir, "wrapped-d1.txt", bound, strlen (bound));
  write_seed (dir, "key.txt", FUZZ_FIXED_KEY, strlen (FUZZ_FIXED_KEY));
  g_free (plain);
  g_free (bound);
  jr_key_free (device);
  jr_key_free (key);
}

int
main (int argc, char **argv)
{
  if (argc != 3)
    {
      g_printerr ("usage: %s JOURNAL_DIR SECRETS_DIR\n", argv[0]);
      return 2;
    }
  if (!jr_crypto_init ())
    return 1;
  g_mkdir_with_parents (argv[1], 0700);
  g_mkdir_with_parents (argv[2], 0700);
  make_secrets (argv[2]);
  make (argv[1], "plain.db", PLAIN);
  make (argv[1], "pin.db", PIN);
  make (argv[1], "passphrase.db", PASSPHRASE);
  make (argv[1], "blocked-pin.db", BLOCKED);
  return 0;
}
