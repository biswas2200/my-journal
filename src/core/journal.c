/* journal: see journal.h. */
#include "journal.h"

#include <string.h>
#include "lockout.h"

/* Settings keys. */
#define K_KEY_PLAIN "key_plain"       /* data key, only while no lock is set */
#define K_KEY_PIN "key_pin"           /* data key wrapped with the PIN or passphrase */
#define K_KEY_RECOVERY "key_recovery" /* data key wrapped with the recovery key */
#define K_LOCK_KIND "lock_kind"       /* "pin" (or absent) or "passphrase" */
#define K_LOCKOUT "lockout"
#define K_LOCK_ON_SLEEP "lock_on_sleep"

struct JrJournal {
  JrDb      *db;
  JrKey     *key;   /* NULL while locked */
  gboolean   lock_enabled;
  JrLockKind lock_kind;
  JrKdfCost  cost;
  JrLockout  lockout;
};

static void
save_lockout (JrJournal *j)
{
  char buf[JR_LOCKOUT_STR_LEN];
  jr_lockout_to_string (&j->lockout, buf, sizeof buf);
  jr_db_set_setting (j->db, K_LOCKOUT, buf);
}

static void
lockout_succeeded (JrJournal *j)
{
  if (j->lockout.failed != 0 || j->lockout.until != 0)
    {
      jr_lockout_success (&j->lockout);
      save_lockout (j);
    }
}

static void
lockout_failed (JrJournal *j, gint64 now)
{
  jr_lockout_fail (&j->lockout, now);
  save_lockout (j);
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
  g_autofree char *kind = jr_db_get_setting (db, K_LOCK_KIND);
  j->lock_enabled = pin != NULL;
  /* Files from before passphrases existed have no kind: they use a PIN. */
  j->lock_kind = g_strcmp0 (kind, "passphrase") == 0 ? JR_LOCK_PASSPHRASE : JR_LOCK_PIN;
  if (!j->lock_enabled)
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
jr_journal_lock_enabled (JrJournal *j)
{
  return j->lock_enabled;
}

JrLockKind
jr_journal_lock_kind (JrJournal *j)
{
  return j->lock_kind;
}

gboolean
jr_journal_is_unlocked (JrJournal *j)
{
  return j->key != NULL;
}

static gboolean
cost_weaker (JrKdfCost have, JrKdfCost want)
{
  return have.ops < want.ops || have.mem < want.mem;
}

/* ---- unlocking ------------------------------------------------------- */

struct JrUnlockAttempt {
  char     *wrapped;     /* copy of the stored wrapped key */
  char     *secret;      /* normalized secret; NULL if malformed */
  gint64    now;
  gboolean  may_upgrade; /* lock secret: re-save with a stronger cost */
  JrKdfCost target;
  JrKey    *key;         /* result of run(); NULL if the secret was wrong */
  char     *rewrapped;   /* result of run(); set when upgraded */
};

JrUnlockAttempt *
jr_journal_begin_unlock (JrJournal *j, JrSecretKind kind, const char *secret, gint64 now)
{
  if (jr_lockout_active (&j->lockout, now))
    return NULL;

  JrUnlockAttempt *a = g_new0 (JrUnlockAttempt, 1);
  a->now = now;
  if (kind == JR_SECRET_LOCK)
    {
      a->wrapped = jr_db_get_setting (j->db, K_KEY_PIN);
      a->secret = jr_lock_secret_normalize (j->lock_kind, secret);
      a->may_upgrade = TRUE;
      a->target = j->cost;
    }
  else
    {
      char norm[JR_RECOVERY_NORM_LEN];
      a->wrapped = jr_db_get_setting (j->db, K_KEY_RECOVERY);
      if (jr_recovery_key_normalize (secret, norm))
        a->secret = g_strdup (norm);
      jr_wipe (norm, sizeof norm);
    }
  return a;
}

void
jr_unlock_attempt_run (JrUnlockAttempt *a)
{
  if (a->secret == NULL || a->wrapped == NULL)
    return;
  a->key = jr_key_unwrap (a->wrapped, a->secret);
  JrKdfCost have;
  if (a->key != NULL && a->may_upgrade && jr_key_wrap_cost (a->wrapped, &have) &&
      cost_weaker (have, a->target))
    a->rewrapped = jr_key_wrap (a->key, a->secret, a->target);
}

void
jr_unlock_attempt_free (JrUnlockAttempt *a)
{
  if (a == NULL)
    return;
  jr_secret_free (a->secret);
  jr_key_free (a->key);
  g_free (a->wrapped);
  g_free (a->rewrapped);
  g_free (a);
}

JrUnlockResult
jr_journal_finish_unlock (JrJournal *j, JrUnlockAttempt *a)
{
  JrUnlockResult result;
  if (a->key == NULL)
    {
      lockout_failed (j, a->now);
      result = JR_UNLOCK_WRONG;
    }
  else
    {
      jr_key_free (j->key);
      j->key = a->key; /* ownership moves to the journal */
      a->key = NULL;
      if (a->rewrapped != NULL)
        jr_db_set_setting (j->db, K_KEY_PIN, a->rewrapped);
      lockout_succeeded (j);
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
jr_journal_unlock (JrJournal *j, const char *secret, gint64 now)
{
  return unlock_now (j, JR_SECRET_LOCK, secret, now);
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
  if (!j->lock_enabled)
    return;
  jr_key_free (j->key);
  j->key = NULL;
}

/* ---- changing the lock ----------------------------------------------- */

struct JrLockJob {
  JrLockJobKind what;
  gint64        now;
  JrKdfCost     cost;
  JrKey        *key;            /* private copy of the data key */
  gboolean      need_verify;
  char         *stored_wrapped; /* the lock key as saved, to check `current` */
  char         *current;        /* normalized; NULL if malformed */
  JrLockKind    new_kind;
  char         *new_secret;     /* normalized */
  /* Results of run(). */
  gboolean      verified;
  gboolean      failed;
  char         *wrapped_lock;
  char         *recovery;
  char         *wrapped_recovery;
};

void
jr_lock_job_free (JrLockJob *job)
{
  if (job == NULL)
    return;
  jr_key_free (job->key);
  jr_secret_free (job->current);
  jr_secret_free (job->new_secret);
  jr_secret_free (job->recovery);
  g_free (job->stored_wrapped);
  g_free (job->wrapped_lock);
  g_free (job->wrapped_recovery);
  g_free (job);
}

JrLockJob *
jr_journal_begin_lock_job (JrJournal *j, JrLockJobKind what, const char *current,
                           JrLockKind new_kind, const char *new_secret, gint64 now,
                           JrJobResult *result)
{
  JrJobResult dummy;
  if (result == NULL)
    result = &dummy;
  *result = JR_JOB_OK;

  if (j->key == NULL)
    {
      *result = JR_JOB_FAILED;
      return NULL;
    }
  gboolean wants_lock = what != JR_JOB_ENABLE;
  if (j->lock_enabled != wants_lock)
    {
      *result = JR_JOB_INVALID; /* nothing to change, or already set */
      return NULL;
    }

  char *norm_new = NULL;
  if (what == JR_JOB_ENABLE || what == JR_JOB_CHANGE)
    {
      norm_new = jr_lock_secret_normalize (new_kind, new_secret);
      if (norm_new == NULL)
        {
          *result = JR_JOB_INVALID;
          return NULL;
        }
    }
  if (wants_lock && jr_lockout_active (&j->lockout, now))
    {
      jr_secret_free (norm_new);
      *result = JR_JOB_WAIT;
      return NULL;
    }

  JrLockJob *job = g_new0 (JrLockJob, 1);
  job->what = what;
  job->now = now;
  job->cost = j->cost;
  job->key = jr_key_dup (j->key);
  job->new_kind = new_kind;
  job->new_secret = norm_new;
  job->need_verify = wants_lock;
  if (wants_lock)
    {
      job->stored_wrapped = jr_db_get_setting (j->db, K_KEY_PIN);
      job->current = jr_lock_secret_normalize (j->lock_kind, current);
    }
  return job;
}

/* A fresh recovery key and the data key wrapped with it. */
static gboolean
make_recovery (JrLockJob *job)
{
  char norm[JR_RECOVERY_NORM_LEN];
  job->recovery = jr_recovery_key_new ();
  jr_recovery_key_normalize (job->recovery, norm);
  job->wrapped_recovery = jr_key_wrap (job->key, norm, job->cost);
  jr_wipe (norm, sizeof norm);
  return job->wrapped_recovery != NULL;
}

void
jr_lock_job_run (JrLockJob *job)
{
  if (job->key == NULL)
    {
      job->failed = TRUE;
      return;
    }
  if (job->need_verify)
    {
      JrKey *k = job->current != NULL ? jr_key_unwrap (job->stored_wrapped, job->current) : NULL;
      job->verified = k != NULL && jr_key_equal (k, job->key);
      jr_key_free (k);
      if (!job->verified)
        return;
    }
  else
    job->verified = TRUE;

  switch (job->what)
    {
    case JR_JOB_ENABLE:
      job->wrapped_lock = jr_key_wrap (job->key, job->new_secret, job->cost);
      job->failed = job->wrapped_lock == NULL || !make_recovery (job);
      break;
    case JR_JOB_CHANGE:
      job->wrapped_lock = jr_key_wrap (job->key, job->new_secret, job->cost);
      job->failed = job->wrapped_lock == NULL;
      break;
    case JR_JOB_NEW_RECOVERY:
      job->failed = !make_recovery (job);
      break;
    case JR_JOB_DISABLE:
    default:
      break; /* nothing slow to do */
    }
}

static gboolean
apply_job (JrJournal *j, JrLockJob *job)
{
  JrDb *db = j->db;
  const char *kind = job->new_kind == JR_LOCK_PASSPHRASE ? "passphrase" : "pin";
  gboolean ok = jr_db_begin (db);
  switch (job->what)
    {
    case JR_JOB_ENABLE:
      ok = ok && jr_db_set_setting (db, K_KEY_PIN, job->wrapped_lock) &&
           jr_db_set_setting (db, K_KEY_RECOVERY, job->wrapped_recovery) &&
           jr_db_set_setting (db, K_LOCK_KIND, kind) &&
           jr_db_delete_setting (db, K_KEY_PLAIN);
      break;
    case JR_JOB_CHANGE:
      ok = ok && jr_db_set_setting (db, K_KEY_PIN, job->wrapped_lock) &&
           jr_db_set_setting (db, K_LOCK_KIND, kind);
      break;
    case JR_JOB_NEW_RECOVERY:
      ok = ok && jr_db_set_setting (db, K_KEY_RECOVERY, job->wrapped_recovery);
      break;
    case JR_JOB_DISABLE:
    default:
      {
        char *plain = jr_key_export (j->key);
        ok = ok && jr_db_set_setting (db, K_KEY_PLAIN, plain) &&
             jr_db_delete_setting (db, K_KEY_PIN) &&
             jr_db_delete_setting (db, K_KEY_RECOVERY) &&
             jr_db_delete_setting (db, K_LOCK_KIND);
        jr_secret_free (plain);
      }
      break;
    }
  /* All or nothing: never leave the file with no way to open it. */
  if (!(ok && jr_db_commit (db)))
    {
      jr_db_rollback (db);
      return FALSE;
    }
  if (job->what == JR_JOB_ENABLE || job->what == JR_JOB_CHANGE)
    {
      j->lock_enabled = TRUE;
      j->lock_kind = job->new_kind;
    }
  else if (job->what == JR_JOB_DISABLE)
    j->lock_enabled = FALSE;
  return TRUE;
}

JrJobResult
jr_journal_finish_lock_job (JrJournal *j, JrLockJob *job, char **recovery_out)
{
  JrJobResult result;
  if (!job->verified)
    {
      if (job->need_verify)
        lockout_failed (j, job->now);
      result = job->need_verify ? JR_JOB_WRONG : JR_JOB_FAILED;
    }
  else if (job->failed || j->key == NULL || !jr_key_equal (j->key, job->key) ||
           j->lock_enabled != (job->what != JR_JOB_ENABLE))
    result = JR_JOB_FAILED; /* locked or changed meanwhile, or out of memory */
  else if (!apply_job (j, job))
    result = JR_JOB_FAILED;
  else
    {
      if (job->need_verify)
        lockout_succeeded (j);
      if (recovery_out != NULL)
        {
          *recovery_out = job->recovery; /* ownership moves to the caller */
          job->recovery = NULL;
        }
      result = JR_JOB_OK;
    }
  jr_lock_job_free (job);
  return result;
}

static JrJobResult
run_job_now (JrJournal *j, JrLockJobKind what, const char *current, JrLockKind kind,
             const char *secret, gint64 now, char **recovery_out)
{
  JrJobResult result;
  JrLockJob *job = jr_journal_begin_lock_job (j, what, current, kind, secret, now, &result);
  if (job == NULL)
    return result;
  jr_lock_job_run (job);
  return jr_journal_finish_lock_job (j, job, recovery_out);
}

char *
jr_journal_enable_lock (JrJournal *j, JrLockKind kind, const char *secret)
{
  char *recovery = NULL;
  run_job_now (j, JR_JOB_ENABLE, NULL, kind, secret, 0, &recovery);
  return recovery;
}

JrJobResult
jr_journal_change_lock (JrJournal *j, const char *current, JrLockKind kind, const char *secret,
                        gint64 now)
{
  return run_job_now (j, JR_JOB_CHANGE, current, kind, secret, now, NULL);
}

JrJobResult
jr_journal_disable_lock (JrJournal *j, const char *current, gint64 now)
{
  return run_job_now (j, JR_JOB_DISABLE, current, JR_LOCK_PIN, NULL, now, NULL);
}

char *
jr_journal_new_recovery_key (JrJournal *j, const char *current, gint64 now, JrJobResult *result)
{
  char *recovery = NULL;
  JrJobResult r = run_job_now (j, JR_JOB_NEW_RECOVERY, current, JR_LOCK_PIN, NULL, now, &recovery);
  if (result != NULL)
    *result = r;
  return recovery;
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
