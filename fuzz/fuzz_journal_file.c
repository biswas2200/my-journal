/* Fuzz target: a damaged journal file. The input is written to disk and
 * opened the way the app does; then everything the app shows is read,
 * and (when the saved keys are cheap to try) it is unlocked, changed and
 * written to. Nothing may crash, leak or report impossible state.
 *
 * Seeds (fuzz/corpus/journal_file) are real journals from make_seeds:
 * no lock, a PIN, a passphrase, a blocked PIN. */
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include "db.h"
#include "fuzz.h"
#include "fuzz_store.h"

#define NOW G_GINT64_CONSTANT (1791316800)
#define MAX_DAYS 64

typedef struct {
  char days[MAX_DAYS][JR_DB_DAY_LEN];
  int n;
} Days;

static void
on_day (const char *day, int entries, gint64 seconds, gpointer user)
{
  Days *d = user;
  FUZZ_CHECK (day != NULL); /* the calendar copies it */
  FUZZ_CHECK (entries >= 0);
  (void) seconds;
  if (d->n < MAX_DAYS && strlen (day) < JR_DB_DAY_LEN)
    g_strlcpy (d->days[d->n++], day, JR_DB_DAY_LEN);
}

static void
on_month (const char *month, int days_written, gpointer user)
{
  (void) user;
  FUZZ_CHECK (month != NULL); /* the day list shows it */
  FUZZ_CHECK (days_written >= 1);
  (void) strlen (month);
}

static void
on_entry (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) id; (void) stamped_at; (void) user;
  FUZZ_CHECK (text != NULL);
  (void) strlen (text);
}

/* Unlocking runs Argon2id at the cost saved in the file; a damaged file
 * may say a gigabyte. Only try when every saved key is at the minimum. */
static gboolean
cheap_to_unlock (JrDb *db)
{
  static const char *const keys[] = { "key_pin", "key_master", "key_recovery" };
  for (gsize i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      g_autofree char *wrapped = jr_db_get_setting (db, keys[i]);
      JrKdfCost cost;
      if (wrapped != NULL && jr_key_wrap_cost (wrapped, &cost) &&
          (cost.ops > FUZZ_FAST.ops || cost.mem > FUZZ_FAST.mem))
        return FALSE;
    }
  return TRUE;
}

static void
check_state (JrJournal *j)
{
  int pin_left = jr_journal_pin_tries_left (j), left = jr_journal_tries_left (j);
  gint64 wait = jr_journal_lockout_remaining (j, NOW);
  FUZZ_CHECK (pin_left >= 0 && pin_left <= 5);
  FUZZ_CHECK (left >= 0 && left <= 5);
  FUZZ_CHECK (wait >= 0 && wait <= 30);
  FUZZ_CHECK (!jr_journal_pin_blocked (j) || pin_left == 0);
  (void) jr_journal_lock_kind (j);
  (void) jr_journal_has_master (j);
  (void) jr_journal_needs_new_pin (j);
  (void) jr_journal_needs_master (j);
  (void) jr_journal_lock_on_sleep (j);
}

static void
try_unlocking (JrJournal *j)
{
  gboolean pin = jr_journal_lock_kind (j) == JR_LOCK_PIN;
  const char *right = pin ? FUZZ_PIN : FUZZ_PASSPHRASE;
  jr_journal_unlock (j, pin ? "000000" : "not the passphrase", NOW);
  check_state (j);
  if (jr_journal_unlock (j, right, NOW) != JR_UNLOCK_OK)
    {
      jr_journal_unlock_with (j, JR_SECRET_MASTER, FUZZ_MASTER, NOW);
      jr_journal_unlock_recovery (j, "0000-0000-0000-0000-0000-0000", NOW);
    }
  check_state (j);
  if (jr_journal_is_unlocked (j) && !jr_journal_needs_new_pin (j) && !jr_journal_needs_master (j))
    {
      const char *next = pin ? FUZZ_NEW_PIN : "another long passphrase";
      jr_journal_change_lock (j, right, jr_journal_lock_kind (j), next, NOW);
      check_state (j);
    }
}

static void
exercise (JrJournal *j, gboolean cheap)
{
  check_state (j);
  if (jr_journal_lock_enabled (j) && cheap)
    try_unlocking (j);

  Days days = { .n = 0 };
  jr_journal_foreach_day (j, "0000-00-00", "9999-12-31", on_day, &days);
  jr_journal_foreach_month (j, "9999-12", on_month, NULL);
  for (int i = 0; i < days.n; i++)
    {
      jr_journal_foreach_entry (j, days.days[i], on_entry, NULL);
      (void) jr_journal_active_seconds (j, days.days[i]); /* any number: the file may say anything */
    }
  /* Writing to it works or fails cleanly. */
  gint64 id = jr_journal_add_entry (j, "2026-10-06", NOW, "fuzz", NOW);
  if (id > 0)
    jr_journal_update_entry (j, id, "fuzz again", NOW);
  jr_journal_add_active_seconds (j, "2026-10-06", 15);
  jr_journal_foreach_entry (j, "2026-10-06", on_entry, NULL);
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  static char *path;
  if (path == NULL)
    {
      char name[64];
      g_snprintf (name, sizeof name, "jr-fuzz-%d.db", (int) getpid ());
      path = g_build_filename (g_get_tmp_dir (), name, NULL);
    }
  if (!g_file_set_contents (path, (const char *) data, (gssize) size, NULL))
    return 0;

  /* What the file says about itself, read before the app opens it. */
  FuzzStore store = { NULL };
  gboolean cheap = FALSE;
  JrDb *db = jr_db_open (path, NULL);
  if (db != NULL)
    {
      cheap = cheap_to_unlock (db);
      g_autofree char *device = jr_db_get_setting (db, FUZZ_DEVICE_SETTING);
      store.secret = device ? jr_key_import (device) : NULL;
      jr_db_close (db);
    }

  GError *error = NULL;
  JrJournal *j = jr_journal_open (path, &error);
  if (j != NULL)
    {
      const JrDeviceStore keyring = { fuzz_store_load, fuzz_store_save, fuzz_store_clear, &store };
      jr_journal_set_kdf_cost (j, FUZZ_FAST);
      jr_journal_set_device_store (j, &keyring);
      exercise (j, cheap);
      jr_journal_close (j);
    }
  g_clear_error (&error);
  jr_key_free (store.secret);
  g_remove (path);
  g_autofree char *journal = g_strconcat (path, "-journal", NULL);
  g_remove (journal);
  return 0;
}
