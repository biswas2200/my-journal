/* Tests for the journal (src/core/journal.c): locks, the master
 * passphrase, the PIN block, the keyring secret, recovery keys and
 * encrypted entries.
 *
 * LeetCode style (see cases.h), in the form of its "design a class"
 * problems: each case is a list of operations and the output expected
 * from each, e.g.
 *
 *   enable pin 123456 (master ...) -> OK
 *   lock
 *   unlock 000000 x5               -> WRONG WRONG WRONG WRONG BLOCKED
 *   master ...                     -> OK
 *   reset pin 123456               -> SAME_PIN
 *
 * The keyring is an in-memory stand-in that can be switched off or made
 * to forget, so the tests never touch a real keyring. A hidden test runs
 * random operation sequences and checks the journal can never end up in
 * a state no secret opens. */
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "cases.h"
#include "journal.h"

#define T0 G_GINT64_CONSTANT (1791316800)
#define DAY "2026-10-06"
#define ENTRY "secret entry"

static const JrKdfCost FAST = { JR_KDF_OPS_MIN, JR_KDF_MEM_MIN };

/* ---- the keyring stand-in -------------------------------------------- */

typedef struct {
  GHashTable *items; /* id -> JrKey */
  gboolean    down;  /* unavailable (no keyring, or locked) */
} MemStore;

static JrDeviceStatus
mem_load (const char *id, JrKey **out, gpointer user)
{
  MemStore *m = user;
  if (m->down)
    return JR_DEVICE_UNAVAILABLE;
  JrKey *k = g_hash_table_lookup (m->items, id);
  if (k == NULL)
    return JR_DEVICE_MISSING;
  *out = jr_key_dup (k);
  return JR_DEVICE_FOUND;
}

static gboolean
mem_save (const char *id, const JrKey *secret, gpointer user)
{
  MemStore *m = user;
  if (m->down)
    return FALSE;
  g_hash_table_insert (m->items, g_strdup (id), jr_key_dup (secret));
  return TRUE;
}

static void
mem_clear (const char *id, gpointer user)
{
  MemStore *m = user;
  if (!m->down)
    g_hash_table_remove (m->items, id);
}

static MemStore mem;
static const JrDeviceStore STORE = { mem_load, mem_save, mem_clear, &mem };

static void
mem_reset (void)
{
  if (mem.items != NULL)
    g_hash_table_unref (mem.items);
  mem.items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) jr_key_free);
  mem.down = FALSE;
}

/* ---- running a list of operations ------------------------------------ */

typedef struct {
  char      *dir, *path;
  JrJournal *j;
  gint64     now;
  char      *recovery, *old_recovery;
} Run;

static JrJournal *
open_journal (const char *path)
{
  GError *error = NULL;
  JrJournal *j = jr_journal_open (path, &error);
  g_assert_no_error (error);
  jr_journal_set_kdf_cost (j, FAST);
  jr_journal_set_device_store (j, &STORE);
  return j;
}

static void
run_start (Run *r)
{
  mem_reset ();
  r->dir = g_dir_make_tmp ("journal-test-XXXXXX", NULL);
  r->path = g_build_filename (r->dir, "journal.db", NULL);
  r->j = open_journal (r->path);
  r->now = T0;
  r->recovery = r->old_recovery = NULL;
  g_assert_cmpint (jr_journal_add_entry (r->j, DAY, T0, ENTRY, T0), >, 0);
}

static void
run_end (Run *r)
{
  jr_journal_close (r->j);
  g_remove (r->path);
  g_rmdir (r->dir);
  g_free (r->path);
  g_free (r->dir);
  jr_secret_free (r->recovery);
  jr_secret_free (r->old_recovery);
}

static void
restart (Run *r)
{
  jr_journal_close (r->j);
  r->j = open_journal (r->path);
}

static void
keep_recovery (Run *r, char *fresh)
{
  if (fresh == NULL)
    return;
  jr_secret_free (r->old_recovery);
  r->old_recovery = r->recovery;
  r->recovery = fresh;
}

static const char *const UNLOCK_NAMES[] = { "OK", "WRONG", "WAIT", "BLOCKED", "NO_DEVICE" };
static const char *const JOB_NAMES[] = { "OK", "WRONG", "WAIT", "BLOCKED", "INVALID", "SAME_PIN", "NO_DEVICE", "FAILED" };

static JrLockKind
kind_of (const char *word)
{
  return g_strcmp0 (word, "passphrase") == 0 ? JR_LOCK_PASSPHRASE : JR_LOCK_PIN;
}

static void
note_entry (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) id; (void) stamped_at;
  if (strcmp (text, ENTRY) == 0)
    *(gboolean *) user = TRUE;
}

static char *
state_of (JrJournal *j)
{
  gboolean lock = jr_journal_lock_enabled (j);
  gboolean pin = lock && jr_journal_lock_kind (j) == JR_LOCK_PIN;
  GString *s = g_string_new (!lock ? "none" : pin ? "pin" : "passphrase");
  g_string_append (s, jr_journal_is_unlocked (j) ? ", unlocked" : ", locked");
  if (pin && jr_journal_pin_blocked (j))
    g_string_append (s, ", blocked");
  if (pin && jr_journal_needs_new_pin (j))
    g_string_append (s, ", needs-new-pin");
  if (pin && jr_journal_needs_master (j))
    g_string_append (s, ", needs-master");
  return g_string_free (s, FALSE);
}

/* The saved PIN key, opened with only the file: what someone with a copy
 * of the file (and not the keyring) could try. */
static gboolean
file_alone_opens (const char *path, const char *pin)
{
  sqlite3 *db;
  sqlite3_stmt *st;
  gboolean opens = FALSE;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  sqlite3_prepare_v2 (db, "SELECT value FROM settings WHERE key = 'key_pin'", -1, &st, NULL);
  if (sqlite3_step (st) == SQLITE_ROW)
    {
      JrKey *k = jr_key_unwrap_for ((const char *) sqlite3_column_text (st, 0), pin, NULL);
      opens = k != NULL;
      jr_key_free (k);
    }
  sqlite3_finalize (st);
  sqlite3_close (db);
  return opens;
}

static void
set_raw_setting (const char *path, const char *key, const char *value)
{
  sqlite3 *db;
  sqlite3_stmt *st;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  if (value == NULL)
    sqlite3_prepare_v2 (db, "DELETE FROM settings WHERE key = ?1", -1, &st, NULL);
  else
    sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO settings (key, value) VALUES (?1, ?2)", -1, &st, NULL);
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

/* Turns the (unlocked, no lock) journal into a PIN file the way the app
 * saved them before master passphrases: a "v1" PIN key, a recovery key,
 * no master, no keyring secret. Then restarts. */
static void
make_legacy_pin (Run *r, const char *pin)
{
  jr_journal_close (r->j);
  char *plain = raw_setting (r->path, "key_plain");
  JrKey *key = jr_key_import (plain);
  char *rk = jr_recovery_key_new (), norm[JR_RECOVERY_NORM_LEN];
  jr_recovery_key_normalize (rk, norm);
  g_autofree char *wrapped_pin = jr_key_wrap (key, pin, FAST);
  g_autofree char *wrapped_rk = jr_key_wrap (key, norm, FAST);
  set_raw_setting (r->path, "key_pin", wrapped_pin);
  set_raw_setting (r->path, "key_recovery", wrapped_rk);
  set_raw_setting (r->path, "key_plain", NULL);
  keep_recovery (r, rk);
  jr_key_free (key);
  jr_secret_free (plain);
  r->j = open_journal (r->path);
}

typedef struct {
  const char *op;
  const char *a, *b, *c, *d;
  const char *want; /* NULL: no output */
} Step;

static const char *
arg (Run *r, const char *a)
{
  if (g_strcmp0 (a, "$recovery") == 0)
    return r->recovery;
  if (g_strcmp0 (a, "$old-recovery") == 0)
    return r->old_recovery;
  return a;
}

/* Runs one operation; returns its output (g_free) or NULL. */
static char *
step (Run *r, const Step *s)
{
  const char *op = s->op;
  char *fresh = NULL;
  if (strcmp (op, "enable") == 0 || strcmp (op, "change") == 0 || strcmp (op, "disable") == 0 ||
      strcmp (op, "new-recovery") == 0 || strcmp (op, "change-master") == 0 ||
      strcmp (op, "set-master") == 0 || strcmp (op, "reset") == 0)
    {
      JrLockRequest req = { 0 };
      if (strcmp (op, "enable") == 0)
        req = (JrLockRequest){ JR_JOB_ENABLE, NULL, kind_of (s->a), s->b, s->c };
      else if (strcmp (op, "change") == 0)
        req = (JrLockRequest){ JR_JOB_CHANGE, s->a, kind_of (s->b), s->c, s->d };
      else if (strcmp (op, "disable") == 0)
        req = (JrLockRequest){ JR_JOB_DISABLE, s->a, JR_LOCK_PIN, NULL, NULL };
      else if (strcmp (op, "new-recovery") == 0)
        req = (JrLockRequest){ JR_JOB_NEW_RECOVERY, s->a, JR_LOCK_PIN, NULL, NULL };
      else if (strcmp (op, "change-master") == 0)
        req = (JrLockRequest){ JR_JOB_CHANGE_MASTER, s->a, JR_LOCK_PIN, NULL, s->b };
      else if (strcmp (op, "set-master") == 0)
        req = (JrLockRequest){ JR_JOB_SET_MASTER, s->a, JR_LOCK_PIN, NULL, s->b };
      else
        req = (JrLockRequest){ JR_JOB_RESET, NULL, kind_of (s->a), s->b, s->c };
      JrJobResult res = jr_journal_run_lock_job (r->j, &req, r->now, &fresh);
      keep_recovery (r, fresh);
      return g_strdup (JOB_NAMES[res]);
    }
  if (strcmp (op, "unlock") == 0 || strcmp (op, "master") == 0 || strcmp (op, "recovery") == 0)
    {
      JrSecretKind kind = op[0] == 'u' ? JR_SECRET_LOCK : op[0] == 'm' ? JR_SECRET_MASTER : JR_SECRET_RECOVERY;
      return g_strdup (UNLOCK_NAMES[jr_journal_unlock_with (r->j, kind, arg (r, s->a), r->now)]);
    }
  if (strcmp (op, "lock") == 0)
    jr_journal_lock (r->j);
  else if (strcmp (op, "restart") == 0)
    restart (r);
  else if (strcmp (op, "wait") == 0)
    r->now += g_ascii_strtoll (s->a, NULL, 10);
  else if (strcmp (op, "keyring") == 0)
    {
      if (strcmp (s->a, "forget") == 0)
        g_hash_table_remove_all (mem.items);
      else
        mem.down = strcmp (s->a, "down") == 0;
    }
  else if (strcmp (op, "legacy-pin") == 0)
    make_legacy_pin (r, s->a);
  else if (strcmp (op, "keyring-items") == 0)
    return g_strdup_printf ("%u", g_hash_table_size (mem.items));
  else if (strcmp (op, "tries") == 0)
    return g_strdup_printf ("%d", jr_journal_lock_kind (r->j) == JR_LOCK_PIN && jr_journal_lock_enabled (r->j)
                                      ? jr_journal_pin_tries_left (r->j)
                                      : jr_journal_tries_left (r->j));
  else if (strcmp (op, "state") == 0)
    return state_of (r->j);
  else if (strcmp (op, "read") == 0)
    {
      gboolean seen = FALSE;
      gboolean ok = jr_journal_foreach_entry (r->j, DAY, note_entry, &seen);
      return g_strdup (ok && seen ? "yes" : "no");
    }
  else if (strcmp (op, "write") == 0)
    return g_strdup (jr_journal_add_entry (r->j, DAY, r->now, "more", r->now) > 0 ? "yes" : "no");
  else if (strcmp (op, "file-alone") == 0)
    return g_strdup (file_alone_opens (r->path, s->a) ? "opens" : "refused");
  else
    g_error ("unknown operation %s", op);
  return NULL;
}

#define PIN "123456"
#define M "river lamp quiet oak"
#define M2 "silver fern morning tide"
#define P "correct horse battery staple"

#define DO(op, a, b, c, d, want) { op, a, b, c, d, want }
#define ENABLE_PIN DO ("enable", "pin", PIN, M, NULL, "OK")
#define ENABLE_PASS DO ("enable", "passphrase", P, NULL, NULL, "OK")
#define LOCK DO ("lock", NULL, NULL, NULL, NULL, NULL)
#define RESTART DO ("restart", NULL, NULL, NULL, NULL, NULL)
#define WAIT(s) DO ("wait", s, NULL, NULL, NULL, NULL)
#define KEYRING(how) DO ("keyring", how, NULL, NULL, NULL, NULL)
#define UNLOCK(s, w) DO ("unlock", s, NULL, NULL, NULL, w)
#define MASTER(s, w) DO ("master", s, NULL, NULL, NULL, w)
#define RECOVERY(s, w) DO ("recovery", s, NULL, NULL, NULL, w)
#define CHANGE(cur, kind, s, m, w) DO ("change", cur, kind, s, m, w)
#define RESET(kind, s, m, w) DO ("reset", kind, s, m, NULL, w)
#define STATE(w) DO ("state", NULL, NULL, NULL, NULL, w)
#define TRIES(w) DO ("tries", NULL, NULL, NULL, NULL, w)
#define READ(w) DO ("read", NULL, NULL, NULL, NULL, w)
#define WRITE(w) DO ("write", NULL, NULL, NULL, NULL, w)
#define ITEMS(w) DO ("keyring-items", NULL, NULL, NULL, NULL, w)
#define FILE_ALONE(pin, w) DO ("file-alone", pin, NULL, NULL, NULL, w)
#define END DO (NULL, NULL, NULL, NULL, NULL, NULL)
#define WRONG5 UNLOCK ("000000", "WRONG"), UNLOCK ("000001", "WRONG"), UNLOCK ("000002", "WRONG"), \
               UNLOCK ("000003", "WRONG"), UNLOCK ("000004", "BLOCKED")
#define BAD_MASTER(w) MASTER ("not the master passphrase", w)

typedef struct {
  const char *name;
  Step steps[40];
} Scenario;

static const Scenario SCENARIOS[] = {
  { "no lock: opens unlocked", { STATE ("none, unlocked"), READ ("yes"), WRITE ("yes"), END } },
  { "no lock: nothing to change",
    { CHANGE (PIN, "pin", "222222", NULL, "INVALID"), DO ("disable", PIN, NULL, NULL, NULL, "INVALID"),
      DO ("new-recovery", PIN, NULL, NULL, NULL, "INVALID"), RESET ("pin", PIN, M, "INVALID"), END } },
  { "a PIN needs a master passphrase",
    { DO ("enable", "pin", PIN, NULL, NULL, "INVALID"), DO ("enable", "pin", PIN, "too short", NULL, "INVALID"),
      DO ("enable", "pin", "12345", M, NULL, "INVALID"), STATE ("none, unlocked"), ENABLE_PIN,
      STATE ("pin, unlocked"), ITEMS ("1"), TRIES ("5"), END } },
  { "the PIN opens it",
    { ENABLE_PIN, LOCK, STATE ("pin, locked"), READ ("no"), WRITE ("no"), UNLOCK ("000000", "WRONG"),
      TRIES ("4"), UNLOCK (PIN, "OK"), READ ("yes"), TRIES ("5"), END } },
  { "the master passphrase opens it too",
    { ENABLE_PIN, LOCK, BAD_MASTER ("WRONG"), MASTER (M, "OK"), STATE ("pin, unlocked"), READ ("yes"), END } },
  { "a master passphrase does not reset the PIN tries",
    { ENABLE_PIN, LOCK, UNLOCK ("000000", "WRONG"), UNLOCK ("000001", "WRONG"), UNLOCK ("000002", "WRONG"),
      MASTER (M, "OK"), LOCK, TRIES ("2"), UNLOCK (PIN, "OK"), TRIES ("5"), END } },
  { "five wrong PINs block it",
    { ENABLE_PIN, LOCK, WRONG5, STATE ("pin, locked, blocked, needs-new-pin"), TRIES ("0"), UNLOCK (PIN, "BLOCKED"),
      READ ("no"), END } },
  { "blocked: waiting or restarting does not help",
    { ENABLE_PIN, LOCK, WRONG5, WAIT ("86400"), UNLOCK (PIN, "BLOCKED"), RESTART, UNLOCK (PIN, "BLOCKED"),
      STATE ("pin, locked, blocked, needs-new-pin"), END } },
  { "blocked: master passphrase, then a new PIN is a must",
    { ENABLE_PIN, LOCK, WRONG5, MASTER (M, "OK"), STATE ("pin, unlocked, blocked, needs-new-pin"),
      READ ("no"), WRITE ("no"), CHANGE (PIN, "pin", "222222", NULL, "INVALID"),
      DO ("new-recovery", PIN, NULL, NULL, NULL, "INVALID"), DO ("disable", PIN, NULL, NULL, NULL, "INVALID"),
      RESET ("pin", PIN, NULL, "SAME_PIN"), RESET ("pin", "12", NULL, "INVALID"),
      RESET ("pin", "222222", NULL, "OK"), STATE ("pin, unlocked"), READ ("yes"), TRIES ("5"), LOCK,
      UNLOCK (PIN, "WRONG"), UNLOCK ("222222", "OK"), END } },
  { "blocked: wrong master passphrases wait 30 s",
    { ENABLE_PIN, LOCK, WRONG5, BAD_MASTER ("WRONG"), BAD_MASTER ("WRONG"), BAD_MASTER ("WRONG"),
      BAD_MASTER ("WRONG"), BAD_MASTER ("WRONG"), MASTER (M, "WAIT"), RESTART, MASTER (M, "WAIT"),
      WAIT ("30"), MASTER (M, "OK"), STATE ("pin, unlocked, blocked, needs-new-pin"), END } },
  { "blocked: the recovery key opens it too",
    { ENABLE_PIN, LOCK, WRONG5, RECOVERY ("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA", "WRONG"),
      RECOVERY ("$recovery", "OK"), STATE ("pin, unlocked, blocked, needs-new-pin"),
      RESET ("pin", "333333", NULL, "OK"), STATE ("pin, unlocked"), END } },
  { "blocked: closing before the new PIN changes nothing",
    { ENABLE_PIN, LOCK, WRONG5, MASTER (M, "OK"), RESTART, STATE ("pin, locked, blocked, needs-new-pin"),
      UNLOCK (PIN, "BLOCKED"), MASTER (M, "OK"), RESET ("pin", "444444", NULL, "OK"), END } },
  { "blocked: a passphrase instead of a new PIN",
    { ENABLE_PIN, LOCK, WRONG5, MASTER (M, "OK"), RESET ("passphrase", P, NULL, "OK"),
      STATE ("passphrase, unlocked"), ITEMS ("0"), LOCK, MASTER (M, "WRONG"), UNLOCK (P, "OK"), END } },
  { "a copy of the file cannot be opened with the PIN",
    { ENABLE_PIN, FILE_ALONE (PIN, "refused"), LOCK, UNLOCK (PIN, "OK"), END } },
  { "keyring unavailable: the PIN is not checked, nothing counted",
    { ENABLE_PIN, LOCK, KEYRING ("down"), UNLOCK (PIN, "NO_DEVICE"), UNLOCK ("000000", "NO_DEVICE"),
      TRIES ("5"), STATE ("pin, locked"), MASTER (M, "OK"), STATE ("pin, unlocked"), LOCK, KEYRING ("up"),
      UNLOCK (PIN, "OK"), END } },
  { "keyring lost the secret: master passphrase, then a new PIN",
    { ENABLE_PIN, LOCK, KEYRING ("forget"), UNLOCK (PIN, "NO_DEVICE"), MASTER (M, "OK"),
      STATE ("pin, unlocked, needs-new-pin"), READ ("no"), RESET ("pin", PIN, NULL, "OK"),
      STATE ("pin, unlocked"), ITEMS ("1"), LOCK, UNLOCK (PIN, "OK"), END } },
  { "keyring unavailable when setting a PIN",
    { KEYRING ("down"), DO ("enable", "pin", PIN, M, NULL, "NO_DEVICE"), STATE ("none, unlocked"),
      KEYRING ("up"), ENABLE_PIN, END } },
  { "change the PIN: the current one is needed and it must differ",
    { ENABLE_PIN, CHANGE ("000000", "pin", "222222", NULL, "WRONG"), TRIES ("4"),
      CHANGE (PIN, "pin", PIN, NULL, "SAME_PIN"), TRIES ("4"), CHANGE (PIN, "pin", "222222", NULL, "OK"),
      TRIES ("5"), LOCK, UNLOCK (PIN, "WRONG"), UNLOCK ("222222", "OK"), LOCK, MASTER (M, "OK"), END } },
  { "five wrong PINs while changing it block it and lock the journal",
    { ENABLE_PIN, CHANGE ("000000", "pin", "222222", NULL, "WRONG"),
      CHANGE ("000001", "pin", "222222", NULL, "WRONG"), CHANGE ("000002", "pin", "222222", NULL, "WRONG"),
      CHANGE ("000003", "pin", "222222", NULL, "WRONG"), CHANGE ("000004", "pin", "222222", NULL, "BLOCKED"),
      STATE ("pin, locked, blocked, needs-new-pin"), READ ("no"), END } },
  { "change the master passphrase",
    { ENABLE_PIN, DO ("change-master", "not the master passphrase", M2, NULL, NULL, "WRONG"),
      DO ("change-master", M, "short", NULL, NULL, "INVALID"), DO ("change-master", M, M2, NULL, NULL, "OK"),
      LOCK, MASTER (M, "WRONG"), MASTER (M2, "OK"), END } },
  { "switch a PIN to a passphrase: the master and keyring secret go",
    { ENABLE_PIN, CHANGE (PIN, "passphrase", P, NULL, "OK"), STATE ("passphrase, unlocked"), ITEMS ("0"),
      LOCK, MASTER (M, "WRONG"), UNLOCK (P, "OK"), END } },
  { "switch a passphrase to a PIN: it needs a master passphrase",
    { ENABLE_PASS, ITEMS ("0"), CHANGE (P, "pin", PIN, NULL, "INVALID"), CHANGE (P, "pin", PIN, M, "OK"),
      ITEMS ("1"), STATE ("pin, unlocked"), LOCK, UNLOCK (PIN, "OK"), LOCK, MASTER (M, "OK"), END } },
  { "turn the lock off: everything goes",
    { ENABLE_PIN, DO ("disable", "000000", NULL, NULL, NULL, "WRONG"), DO ("disable", PIN, NULL, NULL, NULL, "OK"),
      STATE ("none, unlocked"), ITEMS ("0"), RESTART, STATE ("none, unlocked"), READ ("yes"), END } },
  { "a new recovery key replaces the old one",
    { ENABLE_PIN, DO ("new-recovery", "000000", NULL, NULL, NULL, "WRONG"),
      DO ("new-recovery", PIN, NULL, NULL, NULL, "OK"), LOCK, RECOVERY ("$old-recovery", "WRONG"),
      RECOVERY ("$recovery", "OK"), END } },
  { "a PIN from before master passphrases",
    { DO ("legacy-pin", PIN, NULL, NULL, NULL, NULL), STATE ("pin, locked, needs-master"),
      FILE_ALONE (PIN, "opens"), UNLOCK (PIN, "OK"), READ ("yes"),
      DO ("set-master", "000000", M, NULL, NULL, "WRONG"), DO ("set-master", PIN, "short", NULL, NULL, "INVALID"),
      DO ("set-master", PIN, M, NULL, NULL, "OK"), STATE ("pin, unlocked"), FILE_ALONE (PIN, "refused"),
      ITEMS ("1"), LOCK, UNLOCK (PIN, "OK"), LOCK, MASTER (M, "OK"), READ ("yes"), END } },
  { "an old PIN blocked: recovery key, then a new PIN and a master",
    { DO ("legacy-pin", PIN, NULL, NULL, NULL, NULL), WRONG5, MASTER (M, "WRONG"), RECOVERY ("$recovery", "OK"),
      STATE ("pin, unlocked, blocked, needs-new-pin, needs-master"), RESET ("pin", "555555", NULL, "INVALID"),
      RESET ("pin", PIN, M, "SAME_PIN"), RESET ("pin", "555555", M, "OK"), STATE ("pin, unlocked"), LOCK,
      UNLOCK ("555555", "OK"), END } },
  { "set-master only for a PIN without one",
    { ENABLE_PIN, DO ("set-master", PIN, M2, NULL, NULL, "INVALID"), END } },
  { "passphrase: 5 wrong start a 30 s wait that survives a restart",
    { ENABLE_PASS, LOCK, UNLOCK ("wrong passphrase 1", "WRONG"), UNLOCK ("wrong passphrase 2", "WRONG"),
      UNLOCK ("wrong passphrase 3", "WRONG"), UNLOCK ("wrong passphrase 4", "WRONG"),
      UNLOCK ("wrong passphrase 5", "WRONG"), UNLOCK (P, "WAIT"), RESTART, UNLOCK (P, "WAIT"), WAIT ("30"),
      UNLOCK (P, "OK"), TRIES ("5"), END } },
  { "passphrase: typed in another Unicode form",
    { DO ("enable", "passphrase", "caf\xc3\xa9 au lait matin", NULL, NULL, "OK"), LOCK,
      UNLOCK ("cafe\xcc\x81 au lait matin", "OK"), END } },
  { "passphrase: no master passphrase, no PIN-only jobs",
    { DO ("enable", "passphrase", P, M, NULL, "INVALID"), ENABLE_PASS, ITEMS ("0"),
      DO ("change-master", P, M2, NULL, NULL, "INVALID"), DO ("set-master", P, M, NULL, NULL, "INVALID"),
      RESET ("pin", PIN, M, "INVALID"), LOCK, MASTER (P, "WRONG"), TRIES ("5"), UNLOCK (P, "OK"), END } },
};

static void
test_scenarios (void)
{
  JrCases c = { .table = "scenarios" };
  for (gsize i = 0; i < G_N_ELEMENTS (SCENARIOS); i++)
    {
      Run r;
      run_start (&r);
      GString *done = g_string_new (NULL);
      for (int k = 0; SCENARIOS[i].steps[k].op != NULL; k++)
        {
          const Step *s = &SCENARIOS[i].steps[k];
          char *got = step (&r, s);
          g_string_append_printf (done, "%s%s%s%s", k ? ", " : "", s->op, s->a ? " " : "", s->a ? s->a : "");
          if (s->want != NULL)
            jr_case (&c, SCENARIOS[i].name, g_strcmp0 (got, s->want) == 0,
                     "Input [%s]  Expected %s  Got %s", done->str, s->want, got ? got : "nothing");
          g_free (got);
        }
      g_string_free (done, TRUE);
      run_end (&r);
    }
  jr_cases_done (&c);
}

/* ---- hidden test: no sequence of actions locks the owner out ---------- */

/* What the owner knows, updated as their actions succeed. */
typedef struct {
  char pin[8];
  char master[64];
  char passphrase[64];
} Known;

static const char *const PINS[] = { "123456", "222222", "333333", "444444" };
static const char *const PASSES[] = { M, M2, P, "another long passphrase" };

static const char *
pick (const char *const *list)
{
  return list[g_test_rand_int_range (0, 4)];
}

/* Hidden test: random sequences of everything a user (or someone at the
 * laptop guessing) can do, with right and wrong secrets, restarts and
 * keyring trouble. Invariants after every action: entries open exactly
 * when unlocked and no new PIN is pending; a copy of the file never opens
 * with the PIN when a keyring secret is in use; and at the end, with the
 * keyring back, the owner can always get in with what they know or the
 * recovery key, and the entry is still there. */
static void
test_hidden_never_locked_out (void)
{
  JrCases c = { .table = "hidden: never locked out" };
  for (int n = 0; n < 150; n++)
    {
      Run r;
      run_start (&r);
      Known k = { "", "", "" };
      GString *log = g_string_new (NULL);
      int len = g_test_rand_int_range (5, 30);
      for (int t = 0; t < len; t++)
        {
          JrJournal *j = r.j;
          gboolean pin_lock = jr_journal_lock_enabled (j) && jr_journal_lock_kind (j) == JR_LOCK_PIN;
          const char *right = !jr_journal_lock_enabled (j) ? NULL : pin_lock ? k.pin : k.passphrase;
          const char *guess = g_test_rand_bit () && right != NULL ? right : pin_lock ? pick (PINS) : pick (PASSES);
          int what = g_test_rand_int_range (0, 16);
          char *fresh = NULL;
          JrJobResult res = JR_JOB_FAILED;
          JrLockRequest req = { 0 };
          const char *new_pin = pick (PINS), *new_pass = pick (PASSES), *new_master = pick (PASSES);
          /* A master passphrase goes with a PIN that does not have one yet. */
          const char *master_if_needed = pin_lock && jr_journal_has_master (j) ? NULL : new_master;
          switch (what)
            {
            case 0: /* set a PIN */
              req = (JrLockRequest){ JR_JOB_ENABLE, NULL, JR_LOCK_PIN, new_pin, new_master };
              break;
            case 1: /* set a passphrase */
              req = (JrLockRequest){ JR_JOB_ENABLE, NULL, JR_LOCK_PASSPHRASE, new_pass, NULL };
              break;
            case 2: /* change to a PIN */
              req = (JrLockRequest){ JR_JOB_CHANGE, guess, JR_LOCK_PIN, new_pin, master_if_needed };
              break;
            case 3: /* change to a passphrase */
              req = (JrLockRequest){ JR_JOB_CHANGE, guess, JR_LOCK_PASSPHRASE, new_pass, NULL };
              break;
            case 4:
              req = (JrLockRequest){ JR_JOB_DISABLE, guess, JR_LOCK_PIN, NULL, NULL };
              break;
            case 5:
              req = (JrLockRequest){ JR_JOB_NEW_RECOVERY, guess, JR_LOCK_PIN, NULL, NULL };
              break;
            case 6:
              req = (JrLockRequest){ JR_JOB_CHANGE_MASTER, g_test_rand_bit () ? k.master : pick (PASSES),
                                     JR_LOCK_PIN, NULL, new_master };
              break;
            case 7: /* after a block: a new PIN, or a passphrase */
              if (g_test_rand_bit ())
                req = (JrLockRequest){ JR_JOB_RESET, NULL, JR_LOCK_PIN, new_pin, master_if_needed };
              else
                req = (JrLockRequest){ JR_JOB_RESET, NULL, JR_LOCK_PASSPHRASE, new_pass, NULL };
              break;
            default:
              break;
            }
          if (what <= 7)
            {
              res = jr_journal_run_lock_job (j, &req, r.now, &fresh);
              g_string_append_printf (log, "job %d -> %s; ", what, JOB_NAMES[res]);
              if (res == JR_JOB_OK)
                {
                  keep_recovery (&r, fresh);
                  if (req.what == JR_JOB_DISABLE)
                    k = (Known){ "", "", "" };
                  else if (req.what == JR_JOB_CHANGE_MASTER)
                    g_strlcpy (k.master, req.new_master, sizeof k.master);
                  else if (req.what != JR_JOB_NEW_RECOVERY && req.new_kind == JR_LOCK_PIN)
                    {
                      g_strlcpy (k.pin, req.new_secret, sizeof k.pin);
                      k.passphrase[0] = '\0';
                      if (req.new_master != NULL)
                        g_strlcpy (k.master, req.new_master, sizeof k.master);
                    }
                  else if (req.what != JR_JOB_NEW_RECOVERY)
                    {
                      g_strlcpy (k.passphrase, req.new_secret, sizeof k.passphrase);
                      k.pin[0] = k.master[0] = '\0';
                    }
                }
              continue;
            }
          switch (what)
            {
            case 8:
            case 9:
              g_string_append_printf (log, "unlock -> %s; ",
                                      UNLOCK_NAMES[jr_journal_unlock_with (j, JR_SECRET_LOCK, guess, r.now)]);
              break;
            case 10:
              g_string_append_printf (log, "master -> %s; ",
                                      UNLOCK_NAMES[jr_journal_unlock_with (j, JR_SECRET_MASTER,
                                                                           g_test_rand_bit () ? k.master : pick (PASSES), r.now)]);
              break;
            case 11:
              g_string_append_printf (log, "recovery -> %s; ",
                                      UNLOCK_NAMES[jr_journal_unlock_with (j, JR_SECRET_RECOVERY,
                                                                           g_test_rand_bit () ? r.recovery : "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA", r.now)]);
              break;
            case 12:
              jr_journal_lock (j);
              g_string_append (log, "lock; ");
              break;
            case 13:
              restart (&r);
              g_string_append (log, "restart; ");
              break;
            case 14:
              r.now += g_test_rand_int_range (0, 40);
              g_string_append (log, "wait; ");
              break;
            default:
              mem.down = g_test_rand_int_range (0, 4) == 0;
              g_string_append_printf (log, "keyring %s; ", mem.down ? "down" : "up");
              break;
            }

          /* Invariants after every action. */
          j = r.j;
          gboolean seen = FALSE;
          gboolean readable = jr_journal_foreach_entry (j, DAY, note_entry, &seen) && seen;
          gboolean should = jr_journal_is_unlocked (j) && !jr_journal_needs_new_pin (j);
          jr_case (&c, "entries open exactly when allowed", readable == should,
                   "Input [%s]  Expected entries %s  Got %s", log->str, should ? "open" : "closed",
                   readable ? "open" : "closed");
          if (k.pin[0] != '\0' && jr_journal_lock_kind (j) == JR_LOCK_PIN && !jr_journal_needs_master (j))
            jr_case (&c, "a copy of the file never opens with the PIN", !file_alone_opens (r.path, k.pin),
                     "Input [%s]  Expected refused  Got opens", log->str);
        }

      /* The owner, keyring back, after a restart, always gets in. */
      mem.down = FALSE;
      restart (&r);
      JrJournal *j = r.j;
      gboolean in = jr_journal_is_unlocked (j);
      gint64 later = r.now + 3600;
      if (!in && jr_journal_lock_kind (j) == JR_LOCK_PIN && k.pin[0] != '\0' && !jr_journal_pin_blocked (j))
        in = jr_journal_unlock_with (j, JR_SECRET_LOCK, k.pin, later) == JR_UNLOCK_OK;
      if (!in && jr_journal_lock_kind (j) == JR_LOCK_PASSPHRASE && k.passphrase[0] != '\0')
        in = jr_journal_unlock_with (j, JR_SECRET_LOCK, k.passphrase, later) == JR_UNLOCK_OK;
      if (!in && k.master[0] != '\0')
        in = jr_journal_unlock_with (j, JR_SECRET_MASTER, k.master, later) == JR_UNLOCK_OK;
      if (!in && r.recovery != NULL)
        in = jr_journal_unlock_with (j, JR_SECRET_RECOVERY, r.recovery, later) == JR_UNLOCK_OK;
      if (in && jr_journal_needs_new_pin (j))
        {
          JrLockRequest req = { JR_JOB_RESET, NULL, JR_LOCK_PIN, "987650", jr_journal_has_master (j) ? NULL : M };
          in = jr_journal_run_lock_job (j, &req, later, NULL) == JR_JOB_OK;
        }
      gboolean seen = FALSE;
      jr_journal_foreach_entry (j, DAY, note_entry, &seen);
      jr_case (&c, "the owner always gets in", in && seen,
               "Input [%s]  Expected in, with the entry  Got %s", log->str, !in ? "locked out" : "in, entry missing");
      g_string_free (log, TRUE);
      run_end (&r);
    }
  jr_cases_done (&c);
}

/* ---- the three-step API on worker threads ----------------------------- */

static gpointer
run_attempt (gpointer a)
{
  jr_unlock_attempt_run (a);
  return NULL;
}

static gpointer
run_job (gpointer job)
{
  jr_lock_job_run (job);
  return NULL;
}

static void
test_three_steps (void)
{
  Run r;
  run_start (&r);
  JrCases c = { .table = "three steps" };
  JrJournal *j = r.j;
  JrJobResult res = JR_JOB_FAILED;
  JrUnlockResult refused = JR_UNLOCK_OK;

  /* A job and an unlock run on worker threads. */
  JrLockRequest enable = { JR_JOB_ENABLE, NULL, JR_LOCK_PIN, PIN, M };
  JrLockJob *job = jr_journal_begin_lock_job (j, &enable, T0, &res);
  jr_case (&c, "begin a job", job != NULL, "Expected a job  Got %s", JOB_NAMES[res]);
  g_thread_join (g_thread_new ("job", run_job, job));
  char *recovery = NULL;
  res = jr_journal_finish_lock_job (j, job, &recovery);
  jr_case (&c, "finish a job", res == JR_JOB_OK && recovery != NULL, "Expected OK  Got %s", JOB_NAMES[res]);
  keep_recovery (&r, recovery);
  jr_journal_lock (j);
  JrUnlockAttempt *a = jr_journal_begin_unlock (j, JR_SECRET_LOCK, PIN, T0, &refused);
  g_thread_join (g_thread_new ("kdf", run_attempt, a));
  JrUnlockResult u = jr_journal_finish_unlock (j, a);
  jr_case (&c, "unlock on a thread", u == JR_UNLOCK_OK, "Expected OK  Got %s", UNLOCK_NAMES[u]);

  /* Invalid new secrets are refused at begin, without a wrong try. */
  JrLockRequest bad = { JR_JOB_CHANGE, PIN, JR_LOCK_PIN, "12a456", NULL };
  job = jr_journal_begin_lock_job (j, &bad, T0, &res);
  jr_case (&c, "invalid at begin", job == NULL && res == JR_JOB_INVALID && jr_journal_pin_tries_left (j) == 5,
           "Expected INVALID, 5 tries  Got %s, %d tries", JOB_NAMES[res], jr_journal_pin_tries_left (j));

  /* If the journal locks while a job runs (lid closed), nothing is applied. */
  JrLockRequest change = { JR_JOB_CHANGE, PIN, JR_LOCK_PIN, "000111", NULL };
  job = jr_journal_begin_lock_job (j, &change, T0, &res);
  jr_journal_lock (j);
  jr_lock_job_run (job);
  res = jr_journal_finish_lock_job (j, job, NULL);
  jr_case (&c, "locked meanwhile", res == JR_JOB_FAILED, "Expected FAILED  Got %s", JOB_NAMES[res]);
  u = jr_journal_unlock (j, "000111", T0);
  jr_case (&c, "locked meanwhile: old PIN kept", u == JR_UNLOCK_WRONG, "Expected WRONG  Got %s", UNLOCK_NAMES[u]);
  u = jr_journal_unlock (j, PIN, T0);
  jr_case (&c, "locked meanwhile: old PIN kept", u == JR_UNLOCK_OK, "Expected OK  Got %s", UNLOCK_NAMES[u]);

  /* A job or an attempt can be dropped (dialog or window closed). */
  JrLockRequest off = { JR_JOB_DISABLE, PIN, JR_LOCK_PIN, NULL, NULL };
  job = jr_journal_begin_lock_job (j, &off, T0, &res);
  jr_lock_job_free (job);
  jr_case (&c, "dropped job", jr_journal_lock_enabled (j), "Expected still locked with a PIN  Got no lock");
  jr_journal_lock (j);
  a = jr_journal_begin_unlock (j, JR_SECRET_LOCK, PIN, T0, &refused);
  jr_unlock_attempt_free (a);
  jr_case (&c, "dropped attempt", !jr_journal_is_unlocked (j), "Expected locked  Got unlocked");

  /* Nothing to check: blocked PIN, or a wait on. */
  for (int i = 0; i < 5; i++)
    jr_journal_unlock (j, "000000", T0);
  a = jr_journal_begin_unlock (j, JR_SECRET_LOCK, PIN, T0, &refused);
  jr_case (&c, "blocked at begin", a == NULL && refused == JR_UNLOCK_BLOCKED, "Expected BLOCKED  Got %s",
           a ? "an attempt" : UNLOCK_NAMES[refused]);
  for (int i = 0; i < 5; i++)
    jr_journal_unlock_with (j, JR_SECRET_MASTER, "not the master passphrase", T0);
  a = jr_journal_begin_unlock (j, JR_SECRET_MASTER, M, T0 + 1, &refused);
  jr_case (&c, "wait at begin", a == NULL && refused == JR_UNLOCK_WAIT, "Expected WAIT  Got %s",
           a ? "an attempt" : UNLOCK_NAMES[refused]);
  jr_unlock_attempt_free (a);
  jr_cases_done (&c);
  run_end (&r);
}

/* ---- re-saving with a stronger Argon2id cost --------------------------- */

static JrKdfCost
saved_cost (const char *path, const char *key)
{
  g_autofree char *wrapped = raw_setting (path, key);
  JrKdfCost cost = { 0, 0 };
  if (wrapped != NULL)
    jr_key_wrap_cost (wrapped, &cost);
  return cost;
}

static void
test_stronger_cost (void)
{
  /* A PIN, master passphrase or passphrase saved with a weaker Argon2id
   * setting is re-saved with the current one the next time it opens. */
  static const struct {
    const char *name;
    JrLockKind kind;
    const char *secret;
    JrSecretKind with;
    const char *setting;
  } cases[] = {
    { "PIN", JR_LOCK_PIN, PIN, JR_SECRET_LOCK, "key_pin" },
    { "master passphrase", JR_LOCK_PIN, M, JR_SECRET_MASTER, "key_master" },
    { "passphrase", JR_LOCK_PASSPHRASE, P, JR_SECRET_LOCK, "key_pin" },
  };
  JrCases c = { .table = "stronger cost" };
  const JrKdfCost stronger = { FAST.ops + 1, FAST.mem * 2 };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      Run r;
      run_start (&r);
      char *rk = jr_journal_enable_lock (r.j, cases[i].kind, cases[i].kind == JR_LOCK_PIN ? PIN : P,
                                         cases[i].kind == JR_LOCK_PIN ? M : NULL);
      keep_recovery (&r, rk);
      restart (&r);
      jr_journal_set_kdf_cost (r.j, stronger);
      JrUnlockResult u = jr_journal_unlock_with (r.j, cases[i].with, cases[i].secret, T0);
      restart (&r);
      JrKdfCost now = saved_cost (r.path, cases[i].setting);
      jr_case (&c, cases[i].name, u == JR_UNLOCK_OK && now.ops == stronger.ops && now.mem == stronger.mem,
               "Expected OK, re-saved at %" G_GUINT64_FORMAT "/%" G_GSIZE_FORMAT "  Got %s, %" G_GUINT64_FORMAT "/%" G_GSIZE_FORMAT,
               stronger.ops, stronger.mem, UNLOCK_NAMES[u], now.ops, now.mem);
      u = jr_journal_unlock_with (r.j, cases[i].with, cases[i].secret, T0);
      jr_case (&c, cases[i].name, u == JR_UNLOCK_OK, "Expected it still opens  Got %s", UNLOCK_NAMES[u]);
      run_end (&r);
    }
  jr_cases_done (&c);
}

/* ---- entries ----------------------------------------------------------- */

typedef struct {
  int count;
  char texts[8][64];
} Texts;

static void
collect (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) id; (void) stamped_at;
  Texts *t = user;
  if (t->count < 8)
    g_strlcpy (t->texts[t->count++], text, sizeof t->texts[0]);
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
test_entries (void)
{
  Run r;
  run_start (&r);
  JrCases c = { .table = "entries" };
  JrJournal *j = r.j;
  gint64 a = jr_journal_add_entry (j, "2026-10-07", T0, "one", T0);
  gint64 b = jr_journal_add_entry (j, "2026-10-07", T0 + 60, "two", T0);
  jr_case (&c, "edit", jr_journal_update_entry (j, a, "one, edited", T0 + 5), "Expected saved");
  jr_case (&c, "delete", jr_journal_delete_entry (j, b), "Expected deleted");
  jr_journal_add_active_seconds (j, "2026-10-07", 90);
  jr_case (&c, "minutes add up", jr_journal_active_seconds (j, "2026-10-07") == 90, "Expected 90 s");
  Texts t = { 0 };
  jr_journal_foreach_entry (j, "2026-10-07", collect, &t);
  jr_case (&c, "what is left", t.count == 1 && strcmp (t.texts[0], "one, edited") == 0,
           "Expected [one, edited]  Got %d entries", t.count);
  /* Bodies are encrypted on disk, with or without a lock. */
  jr_case (&c, "encrypted on disk", !file_contains_plaintext (r.path, "edited") &&
                                        !file_contains_plaintext (r.path, ENTRY),
           "Expected no plain text in the file  Got some");
  jr_case (&c, "lock on sleep is on by default", jr_journal_lock_on_sleep (j), "Expected on");
  jr_journal_set_lock_on_sleep (j, FALSE);
  restart (&r);
  jr_case (&c, "lock on sleep is saved", !jr_journal_lock_on_sleep (r.j), "Expected off after a restart");
  jr_cases_done (&c);
  run_end (&r);
}

typedef struct {
  const char *want;
  gboolean    same;
} Same;

static void
compare_entry (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) id; (void) stamped_at;
  Same *s = user;
  s->same = strcmp (text, s->want) == 0;
}

/* Max size: a 10 MiB entry is saved, survives a restart and reads back. */
static void
test_huge_entry (void)
{
  Run r;
  run_start (&r);
  JrCases c = { .table = "huge entry" };
  gsize n = 10 << 20;
  char *big = g_malloc (n + 1);
  memset (big, 'w', n);
  big[n] = '\0';
  gint64 id = jr_journal_add_entry (r.j, "2026-10-08", T0, big, T0);
  restart (&r);
  Same same = { big, FALSE };
  jr_journal_foreach_entry (r.j, "2026-10-08", compare_entry, &same);
  jr_case (&c, "10 MiB entry", id > 0 && same.same, "Expected saved and read back  Got %s",
           id > 0 ? "different text" : "not saved");
  g_free (big);
  jr_cases_done (&c);
  run_end (&r);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_true (jr_crypto_init ());
  g_test_add_func ("/journal/scenarios", test_scenarios);
  g_test_add_func ("/journal/three-steps", test_three_steps);
  g_test_add_func ("/journal/stronger-cost", test_stronger_cost);
  g_test_add_func ("/journal/entries", test_entries);
  g_test_add_func ("/journal/huge-entry", test_huge_entry);
  g_test_add_func ("/journal/hidden/never-locked-out", test_hidden_never_locked_out);
  int rc = g_test_run ();
  if (mem.items != NULL)
    g_hash_table_unref (mem.items);
  return rc;
}
