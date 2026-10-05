/* journal: see journal.h. */
#include "journal.h"

#include <string.h>
#include "lockout.h"

/* Settings keys. */
#define K_KEY_PLAIN "key_plain"       /* data key, only while no PIN is set */
#define K_KEY_PIN "key_pin"           /* data key wrapped with the PIN */
#define K_KEY_RECOVERY "key_recovery" /* data key wrapped with the recovery key */
#define K_LOCKOUT "lockout"
#define K_LOCK_ON_SLEEP "lock_on_sleep"

struct JrJournal {
  JrDb     *db;
  JrKey    *key;   /* NULL while locked */
  gboolean  pin_enabled;
  JrKdfCost cost;
  JrLockout lockout;
};

static void
save_lockout (JrJournal *j)
{
  char buf[JR_LOCKOUT_STR_LEN];
  jr_lockout_to_string (&j->lockout, buf, sizeof buf);
  jr_db_set_setting (j->db, K_LOCKOUT, buf);
}

JrJournal *
jr_journal_open (const char *path, GError **error)
{
  JrDb *db = jr_db_open (path, error);
  if (db == NULL)
    return NULL;

  JrJournal *j = g_new0 (JrJournal, 1);
  j->db = db;
  j->cost = (JrKdfCost){ JR_KDF_OPS_DEFAULT, JR_KDF_MEM_DEFAULT };

  g_autofree char *lockout = jr_db_get_setting (db, K_LOCKOUT);
  if (!jr_lockout_from_string (lockout, &j->lockout))
    j->lockout = (JrLockout){ 0 };

  g_autofree char *pin = jr_db_get_setting (db, K_KEY_PIN);
  j->pin_enabled = pin != NULL;
  if (!j->pin_enabled)
    {
      char *plain = jr_db_get_setting (db, K_KEY_PLAIN);
      if (plain == NULL)
        {
          /* First run: make the data key. */
          j->key = jr_key_generate ();
          plain = jr_key_export (j->key);
          jr_db_set_setting (db, K_KEY_PLAIN, plain);
        }
      else
        j->key = jr_key_import (plain);
      jr_secret_free (plain);
      if (j->key == NULL)
        {
          g_set_error (error, g_quark_from_static_string ("jr-journal-error"), 1,
                       "The journal key in %s is damaged", path);
          jr_journal_close (j);
          return NULL;
        }
    }
  return j;
}

void
jr_journal_close (JrJournal *j)
{
  if (j == NULL)
    return;
  jr_key_free (j->key);
  jr_db_close (j->db);
  g_free (j);
}

void
jr_journal_set_kdf_cost (JrJournal *j, JrKdfCost cost)
{
  j->cost = cost;
}

gboolean
jr_journal_pin_enabled (JrJournal *j)
{
  return j->pin_enabled;
}

gboolean
jr_journal_is_unlocked (JrJournal *j)
{
  return j->key != NULL;
}

struct JrUnlockAttempt {
  char  *wrapped; /* copy of the stored wrapped key */
  char  *secret;  /* normalized PIN or recovery key; NULL if malformed */
  gint64 now;
  JrKey *key;     /* result of run(); NULL if the secret was wrong */
};

JrUnlockAttempt *
jr_journal_begin_unlock (JrJournal *j, JrSecretKind kind, const char *secret, gint64 now)
{
  if (jr_lockout_active (&j->lockout, now))
    return NULL;

  JrUnlockAttempt *a = g_new0 (JrUnlockAttempt, 1);
  a->now = now;
  if (kind == JR_SECRET_PIN)
    {
      a->wrapped = jr_db_get_setting (j->db, K_KEY_PIN);
      if (jr_pin_valid (secret))
        a->secret = g_strdup (secret);
    }
  else
    {
      char norm[JR_RECOVERY_NORM_LEN];
      a->wrapped = jr_db_get_setting (j->db, K_KEY_RECOVERY);
      if (jr_recovery_key_normalize (secret, norm))
        a->secret = g_strdup (norm);
      memset (norm, 0, sizeof norm);
    }
  return a;
}

void
jr_unlock_attempt_run (JrUnlockAttempt *a)
{
  if (a->secret != NULL && a->wrapped != NULL)
    a->key = jr_key_unwrap (a->wrapped, a->secret);
}

void
jr_unlock_attempt_free (JrUnlockAttempt *a)
{
  if (a == NULL)
    return;
  jr_secret_free (a->secret);
  jr_key_free (a->key);
  g_free (a->wrapped);
  g_free (a);
}

JrUnlockResult
jr_journal_finish_unlock (JrJournal *j, JrUnlockAttempt *a)
{
  JrUnlockResult result;
  if (a->key == NULL)
    {
      jr_lockout_fail (&j->lockout, a->now);
      save_lockout (j);
      result = JR_UNLOCK_WRONG;
    }
  else
    {
      jr_key_free (j->key);
      j->key = a->key; /* ownership moves to the journal */
      a->key = NULL;
      if (j->lockout.failed != 0 || j->lockout.until != 0)
        {
          jr_lockout_success (&j->lockout);
          save_lockout (j);
        }
      result = JR_UNLOCK_OK;
    }
  jr_unlock_attempt_free (a);
  return result;
}

static JrUnlockResult
unlock_now (JrJournal *j, JrSecretKind kind, const char *secret, gint64 now)
{
  JrUnlockAttempt *a = jr_journal_begin_unlock (j, kind, secret, now);
  if (a == NULL)
    return JR_UNLOCK_WAIT;
  jr_unlock_attempt_run (a);
  return jr_journal_finish_unlock (j, a);
}

JrUnlockResult
jr_journal_unlock_pin (JrJournal *j, const char *pin, gint64 now)
{
  return unlock_now (j, JR_SECRET_PIN, pin, now);
}

JrUnlockResult
jr_journal_unlock_recovery (JrJournal *j, const char *recovery, gint64 now)
{
  return unlock_now (j, JR_SECRET_RECOVERY, recovery, now);
}

gint64
jr_journal_lockout_remaining (JrJournal *j, gint64 now)
{
  return jr_lockout_remaining (&j->lockout, now);
}

int
jr_journal_tries_left (JrJournal *j)
{
  return jr_lockout_tries_left (&j->lockout);
}

void
jr_journal_lock (JrJournal *j)
{
  if (!j->pin_enabled)
    return;
  jr_key_free (j->key);
  j->key = NULL;
}

/* Wraps the data key with a fresh recovery key; returns it for display. */
static char *
wrap_recovery (JrJournal *j, char **wrapped_out)
{
  char *recovery = jr_recovery_key_new ();
  char norm[JR_RECOVERY_NORM_LEN];
  jr_recovery_key_normalize (recovery, norm);
  *wrapped_out = jr_key_wrap (j->key, norm, j->cost);
  memset (norm, 0, sizeof norm);
  if (*wrapped_out == NULL)
    {
      jr_secret_free (recovery);
      return NULL;
    }
  return recovery;
}

char *
jr_journal_enable_pin (JrJournal *j, const char *pin)
{
  if (j->key == NULL || j->pin_enabled || !jr_pin_valid (pin))
    return NULL;

  g_autofree char *wrapped_pin = jr_key_wrap (j->key, pin, j->cost);
  g_autofree char *wrapped_rec = NULL;
  char *recovery = wrap_recovery (j, &wrapped_rec);
  if (wrapped_pin == NULL || recovery == NULL)
    {
      jr_secret_free (recovery);
      return NULL;
    }

  /* All or nothing: never leave the file with no way to open it. */
  if (!jr_db_begin (j->db) ||
      !jr_db_set_setting (j->db, K_KEY_PIN, wrapped_pin) ||
      !jr_db_set_setting (j->db, K_KEY_RECOVERY, wrapped_rec) ||
      !jr_db_delete_setting (j->db, K_KEY_PLAIN) ||
      !jr_db_commit (j->db))
    {
      jr_db_rollback (j->db);
      jr_secret_free (recovery);
      return NULL;
    }
  j->pin_enabled = TRUE;
  return recovery;
}

gboolean
jr_journal_change_pin (JrJournal *j, const char *pin)
{
  if (j->key == NULL || !j->pin_enabled || !jr_pin_valid (pin))
    return FALSE;
  g_autofree char *wrapped = jr_key_wrap (j->key, pin, j->cost);
  return wrapped != NULL && jr_db_set_setting (j->db, K_KEY_PIN, wrapped);
}

char *
jr_journal_new_recovery_key (JrJournal *j)
{
  if (j->key == NULL || !j->pin_enabled)
    return NULL;
  g_autofree char *wrapped = NULL;
  char *recovery = wrap_recovery (j, &wrapped);
  if (recovery != NULL && !jr_db_set_setting (j->db, K_KEY_RECOVERY, wrapped))
    {
      jr_secret_free (recovery);
      return NULL;
    }
  return recovery;
}

gboolean
jr_journal_disable_pin (JrJournal *j)
{
  if (j->key == NULL || !j->pin_enabled)
    return FALSE;
  char *plain = jr_key_export (j->key);
  gboolean ok = jr_db_begin (j->db) &&
                jr_db_set_setting (j->db, K_KEY_PLAIN, plain) &&
                jr_db_delete_setting (j->db, K_KEY_PIN) &&
                jr_db_delete_setting (j->db, K_KEY_RECOVERY) &&
                jr_db_commit (j->db);
  jr_secret_free (plain);
  if (!ok)
    {
      jr_db_rollback (j->db);
      return FALSE;
    }
  j->pin_enabled = FALSE;
  return TRUE;
}

gboolean
jr_journal_lock_on_sleep (JrJournal *j)
{
  g_autofree char *v = jr_db_get_setting (j->db, K_LOCK_ON_SLEEP);
  return g_strcmp0 (v, "0") != 0; /* on by default */
}

void
jr_journal_set_lock_on_sleep (JrJournal *j, gboolean on)
{
  jr_db_set_setting (j->db, K_LOCK_ON_SLEEP, on ? "1" : "0");
}

gint64
jr_journal_add_entry (JrJournal *j, const char *day, gint64 stamped_at,
                      const char *text, gint64 now)
{
  if (j->key == NULL)
    return 0;
  gsize len = 0;
  g_autofree guint8 *blob = jr_seal (j->key, text, strlen (text), &len);
  return jr_db_insert_entry (j->db, day, stamped_at, blob, len, now);
}

gboolean
jr_journal_update_entry (JrJournal *j, gint64 id, const char *text, gint64 now)
{
  if (j->key == NULL)
    return FALSE;
  gsize len = 0;
  g_autofree guint8 *blob = jr_seal (j->key, text, strlen (text), &len);
  return jr_db_update_entry (j->db, id, blob, len, now);
}

gboolean
jr_journal_delete_entry (JrJournal *j, gint64 id)
{
  return j->key != NULL && jr_db_delete_entry (j->db, id);
}

typedef struct {
  JrJournal  *j;
  JrEntryFunc fn;
  gpointer    user;
} ForeachCtx;

static void
decrypt_row (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  ForeachCtx *c = user;
  char *text = jr_unseal (c->j->key, body, len);
  if (text == NULL)
    {
      g_warning ("Entry %" G_GINT64_FORMAT " could not be decrypted; skipped", id);
      return;
    }
  c->fn (id, stamped_at, text, c->user);
  jr_secret_free (text);
}

gboolean
jr_journal_foreach_entry (JrJournal *j, const char *day, JrEntryFunc fn, gpointer user)
{
  if (j->key == NULL)
    return FALSE;
  ForeachCtx c = { j, fn, user };
  return jr_db_foreach_entry (j->db, day, decrypt_row, &c);
}

void
jr_journal_add_active_seconds (JrJournal *j, const char *day, gint64 seconds)
{
  jr_db_add_active_seconds (j->db, day, seconds);
}

gint64
jr_journal_active_seconds (JrJournal *j, const char *day)
{
  return jr_db_active_seconds (j->db, day);
}

gboolean
jr_journal_foreach_day (JrJournal *j, const char *from, const char *to,
                        JrDbDayFunc fn, gpointer user)
{
  return jr_db_foreach_day (j->db, from, to, fn, user);
}

gboolean
jr_journal_foreach_month (JrJournal *j, const char *before_month,
                          JrDbMonthFunc fn, gpointer user)
{
  return jr_db_foreach_month (j->db, before_month, fn, user);
}
