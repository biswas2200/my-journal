/* journal: see journal.h. */
#include "journal.h"

#include <string.h>
#include "lockout.h"

/* Settings keys. */
#define K_KEY_PLAIN "key_plain"       /* data key, only while no lock is set */
#define K_KEY_PIN "key_pin"           /* data key wrapped with the PIN (+ device secret) or passphrase */
#define K_KEY_MASTER "key_master"     /* data key wrapped with the master passphrase (PIN locks) */
#define K_KEY_RECOVERY "key_recovery" /* data key wrapped with the recovery key */
#define K_LOCK_KIND "lock_kind"       /* "pin" (or absent) or "passphrase" */
#define K_DEVICE_ID "device_id"       /* names the PIN's device secret in the keyring (not secret) */
#define K_LOCKOUT "lockout"
#define K_LOCK_ON_SLEEP "lock_on_sleep"

struct JrJournal {
  JrDb      *db;
  JrKey     *key;            /* NULL while locked */
  gboolean   lock_enabled;
  JrLockKind lock_kind;
  gboolean   has_master;
  gboolean   pin_bound;      /* the PIN key needs the device secret ("d1") */
  char      *device_id;      /* NULL until a PIN is bound */
  gboolean   device_missing; /* the keyring said it has no device secret */
  const JrDeviceStore *store;
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
load_lock_state (JrJournal *j)
{
  g_autofree char *pin = jr_db_get_setting (j->db, K_KEY_PIN);
  g_autofree char *kind = jr_db_get_setting (j->db, K_LOCK_KIND);
  g_autofree char *master = jr_db_get_setting (j->db, K_KEY_MASTER);
  j->lock_enabled = pin != NULL;
  /* Files from before passphrases existed have no kind: they use a PIN. */
  j->lock_kind = g_strcmp0 (kind, "passphrase") == 0 ? JR_LOCK_PASSPHRASE : JR_LOCK_PIN;
  j->has_master = master != NULL;
  j->pin_bound = jr_key_wrap_needs_device (pin);
  g_free (j->device_id);
  j->device_id = jr_db_get_setting (j->db, K_DEVICE_ID);
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

  load_lock_state (j);
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
  g_free (j->device_id);
  g_free (j);
}

void
jr_journal_set_kdf_cost (JrJournal *j, JrKdfCost cost)
{
  j->cost = cost;
}

void
jr_journal_set_device_store (JrJournal *j, const JrDeviceStore *store)
{
  j->store = store;
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
is_pin (JrJournal *j)
{
  return j->lock_enabled && j->lock_kind == JR_LOCK_PIN;
}

gboolean
jr_journal_has_master (JrJournal *j)
{
  return is_pin (j) && j->has_master;
}

gboolean
jr_journal_pin_blocked (JrJournal *j)
{
  return is_pin (j) && jr_lockout_pin_blocked (&j->lockout);
}

int
jr_journal_pin_tries_left (JrJournal *j)
{
  return jr_lockout_pin_tries_left (&j->lockout);
}

gboolean
jr_journal_needs_new_pin (JrJournal *j)
{
  return is_pin (j) && (jr_lockout_pin_blocked (&j->lockout) || j->device_missing);
}

gboolean
jr_journal_needs_master (JrJournal *j)
{
  return is_pin (j) && !j->has_master;
}

/* Entries open only when unlocked and no new PIN is pending. */
static gboolean
entries_open (JrJournal *j)
{
  return j->key != NULL && !jr_journal_needs_new_pin (j);
}

static gboolean
cost_weaker (JrKdfCost have, JrKdfCost want)
{
  return have.ops < want.ops || have.mem < want.mem;
}

/* Asks the store for the device secret; no store or no id is treated as
 * unavailable or missing. Worker threads only. */
static JrDeviceStatus
device_load (const JrDeviceStore *store, const char *id, JrKey **out)
{
  *out = NULL;
  if (store == NULL)
    return JR_DEVICE_UNAVAILABLE;
  if (id == NULL)
    return JR_DEVICE_MISSING;
  JrDeviceStatus status = store->load (id, out, store->user);
  if (status != JR_DEVICE_FOUND)
    {
      jr_key_free (*out);
      *out = NULL;
    }
  else if (*out == NULL)
    status = JR_DEVICE_UNAVAILABLE;
  return status;
}

/* ---- unlocking ------------------------------------------------------- */

struct JrUnlockAttempt {
  JrSecretKind kind;
  gboolean     pin;          /* a PIN try (counts toward the block) */
  char        *wrapped;      /* copy of the saved key for this secret */
  char        *secret;       /* normalized; NULL if malformed */
  gint64       now;
  gboolean     bound;        /* the saved key needs the device secret */
  gboolean     probe;        /* ask the keyring whether the PIN's secret is still there */
  const JrDeviceStore *store;
  char        *device_id;
  gboolean     may_upgrade;  /* re-save with a stronger cost */
  JrKdfCost    target;
  /* Results of run(). */
  gboolean       asked;
  JrDeviceStatus device;
  JrKey         *key;        /* NULL if the secret was wrong */
  char          *rewrapped;  /* set when upgraded */
};

JrUnlockAttempt *
jr_journal_begin_unlock (JrJournal *j, JrSecretKind kind, const char *secret, gint64 now,
                         JrUnlockResult *refused)
{
  JrUnlockResult dummy;
  if (refused == NULL)
    refused = &dummy;
  gboolean pin = is_pin (j);
  if (!j->lock_enabled || (kind == JR_SECRET_MASTER && !(pin && j->has_master)))
    {
      *refused = JR_UNLOCK_WRONG; /* nothing to unlock with that */
      return NULL;
    }
  if (kind == JR_SECRET_LOCK && pin)
    {
      if (jr_lockout_pin_blocked (&j->lockout))
        {
          *refused = JR_UNLOCK_BLOCKED;
          return NULL;
        }
    }
  else if (jr_lockout_active (&j->lockout, now))
    {
      *refused = JR_UNLOCK_WAIT;
      return NULL;
    }

  JrUnlockAttempt *a = g_new0 (JrUnlockAttempt, 1);
  a->kind = kind;
  a->now = now;
  a->store = j->store;
  a->device_id = g_strdup (j->device_id);
  a->target = j->cost;
  switch (kind)
    {
    case JR_SECRET_LOCK:
      a->pin = pin;
      a->wrapped = jr_db_get_setting (j->db, K_KEY_PIN);
      a->secret = jr_lock_secret_normalize (j->lock_kind, secret);
      a->bound = pin && j->pin_bound;
      a->may_upgrade = TRUE;
      break;
    case JR_SECRET_MASTER:
      a->wrapped = jr_db_get_setting (j->db, K_KEY_MASTER);
      a->secret = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, secret);
      a->probe = j->pin_bound;
      a->may_upgrade = TRUE;
      break;
    case JR_SECRET_RECOVERY:
    default:
      {
        char norm[JR_RECOVERY_NORM_LEN];
        a->wrapped = jr_db_get_setting (j->db, K_KEY_RECOVERY);
        if (jr_recovery_key_normalize (secret, norm))
          a->secret = g_strdup (norm);
        jr_wipe (norm, sizeof norm);
        a->probe = pin && j->pin_bound;
      }
      break;
    }
  return a;
}

void
jr_unlock_attempt_run (JrUnlockAttempt *a)
{
  JrKey *device = NULL;
  if (a->bound || a->probe)
    {
      a->asked = TRUE;
      a->device = device_load (a->store, a->device_id, &device);
    }
  if (a->secret != NULL && a->wrapped != NULL && (!a->bound || device != NULL))
    {
      const JrKey *with = a->bound ? device : NULL;
      a->key = jr_key_unwrap_for (a->wrapped, a->secret, with);
      JrKdfCost have;
      if (a->key != NULL && a->may_upgrade && jr_key_wrap_cost (a->wrapped, &have) &&
          cost_weaker (have, a->target))
        a->rewrapped = jr_key_wrap_for (a->key, a->secret, with, a->target);
    }
  jr_key_free (device);
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
  g_free (a->device_id);
  g_free (a);
}

JrUnlockResult
jr_journal_finish_unlock (JrJournal *j, JrUnlockAttempt *a)
{
  JrUnlockResult result;
  if (a->asked && a->device == JR_DEVICE_MISSING)
    j->device_missing = TRUE;
  else if (a->asked && a->device == JR_DEVICE_FOUND)
    j->device_missing = FALSE;

  if (a->bound && a->device != JR_DEVICE_FOUND)
    result = JR_UNLOCK_NO_DEVICE; /* nothing was checked, nothing counts */
  else if (a->key == NULL)
    {
      if (a->pin)
        jr_lockout_pin_fail (&j->lockout);
      else
        jr_lockout_fail (&j->lockout, a->now);
      save_lockout (j);
      result = a->pin && jr_lockout_pin_blocked (&j->lockout) ? JR_UNLOCK_BLOCKED : JR_UNLOCK_WRONG;
    }
  else
    {
      jr_key_free (j->key);
      j->key = a->key; /* ownership moves to the journal */
      a->key = NULL;
      if (a->rewrapped != NULL)
        jr_db_set_setting (j->db, a->kind == JR_SECRET_MASTER ? K_KEY_MASTER : K_KEY_PIN, a->rewrapped);
      JrLockout before = j->lockout;
      if (a->pin)
        jr_lockout_pin_success (&j->lockout); /* the right PIN clears everything */
      else
        jr_lockout_success (&j->lockout);     /* a blocked PIN stays blocked */
      if (memcmp (&before, &j->lockout, sizeof before) != 0)
        save_lockout (j);
      result = JR_UNLOCK_OK;
    }
  jr_unlock_attempt_free (a);
  return result;
}

JrUnlockResult
jr_journal_unlock_with (JrJournal *j, JrSecretKind kind, const char *secret, gint64 now)
{
  JrUnlockResult refused = JR_UNLOCK_WRONG;
  JrUnlockAttempt *a = jr_journal_begin_unlock (j, kind, secret, now, &refused);
  if (a == NULL)
    return refused;
  jr_unlock_attempt_run (a);
  return jr_journal_finish_unlock (j, a);
}

JrUnlockResult
jr_journal_unlock (JrJournal *j, const char *secret, gint64 now)
{
  return jr_journal_unlock_with (j, JR_SECRET_LOCK, secret, now);
}

JrUnlockResult
jr_journal_unlock_recovery (JrJournal *j, const char *recovery, gint64 now)
{
  return jr_journal_unlock_with (j, JR_SECRET_RECOVERY, recovery, now);
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

typedef enum { VERIFY_NONE, VERIFY_PIN, VERIFY_PASSPHRASE, VERIFY_MASTER } Verify;

struct JrLockJob {
  JrLockJobKind what;
  gint64        now;
  JrKdfCost     cost;
  JrKey        *key;            /* private copy of the data key */
  const JrDeviceStore *store;
  char         *device_id;      /* the PIN's device secret: existing or new name */
  /* The journal as it was at begin; the job is only applied to that. */
  gboolean      was_enabled, was_master;
  JrLockKind    was_kind;
  /* Checking `current`. */
  Verify        verify;
  gboolean      verify_bound;   /* the saved PIN needs the device secret */
  char         *stored;         /* the saved key `current` must open */
  char         *current;        /* normalized; NULL if malformed */
  /* The new lock. */
  JrLockKind    new_kind;
  char         *new_secret;     /* normalized */
  char         *new_master;     /* normalized, or NULL */
  gboolean      bind_pin;       /* writes a device-bound PIN key */
  char         *old_pin;        /* RESET: the PIN key being replaced, to refuse the same PIN */
  gboolean      old_pin_bound;
  /* Results of run(). */
  JrDeviceStatus device;
  gboolean      device_failed;  /* the keyring was needed and did not help */
  gboolean      verified;
  gboolean      same_pin;
  gboolean      failed;
  char         *wrapped_lock;
  char         *wrapped_master;
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
  jr_secret_free (job->new_master);
  jr_secret_free (job->recovery);
  g_free (job->device_id);
  g_free (job->stored);
  g_free (job->old_pin);
  g_free (job->wrapped_lock);
  g_free (job->wrapped_master);
  g_free (job->wrapped_recovery);
  g_free (job);
}

/* Is this job allowed on the journal as it is now? */
static gboolean
job_allowed (JrJournal *j, JrLockJobKind what)
{
  gboolean pin = is_pin (j);
  if (jr_journal_needs_new_pin (j) != (what == JR_JOB_RESET))
    return FALSE; /* a pending new PIN comes first, and only then */
  switch (what)
    {
    case JR_JOB_ENABLE:
      return !j->lock_enabled;
    case JR_JOB_CHANGE:
    case JR_JOB_DISABLE:
    case JR_JOB_NEW_RECOVERY:
      return j->lock_enabled;
    case JR_JOB_CHANGE_MASTER:
      return pin && j->has_master;
    case JR_JOB_SET_MASTER:
      return pin && !j->has_master;
    case JR_JOB_RESET:
    default:
      return TRUE;
    }
}

JrLockJob *
jr_journal_begin_lock_job (JrJournal *j, const JrLockRequest *req, gint64 now, JrJobResult *result)
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
  JrLockJobKind what = req->what;
  gboolean pin = is_pin (j);
  if (!job_allowed (j, what))
    {
      *result = JR_JOB_INVALID;
      return NULL;
    }

  /* The new secrets must be valid, and a master passphrase comes exactly
   * with a PIN that does not have one yet. */
  gboolean new_lock = what == JR_JOB_ENABLE || what == JR_JOB_CHANGE || what == JR_JOB_RESET;
  gboolean wants_master = new_lock ? req->new_kind == JR_LOCK_PIN && !(pin && j->has_master)
                                   : what == JR_JOB_CHANGE_MASTER || what == JR_JOB_SET_MASTER;
  char *norm_new = new_lock ? jr_lock_secret_normalize (req->new_kind, req->new_secret) : NULL;
  char *norm_master = wants_master ? jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, req->new_master) : NULL;
  if ((new_lock && norm_new == NULL) || wants_master != (req->new_master != NULL) ||
      (wants_master && norm_master == NULL))
    {
      jr_secret_free (norm_new);
      jr_secret_free (norm_master);
      *result = JR_JOB_INVALID;
      return NULL;
    }

  Verify verify = VERIFY_NONE;
  if (what == JR_JOB_CHANGE || what == JR_JOB_DISABLE || what == JR_JOB_NEW_RECOVERY)
    verify = pin ? VERIFY_PIN : VERIFY_PASSPHRASE;
  else if (what == JR_JOB_SET_MASTER)
    verify = VERIFY_PIN;
  else if (what == JR_JOB_CHANGE_MASTER)
    verify = VERIFY_MASTER;
  char *norm_current = verify == VERIFY_NONE ? NULL
                       : jr_lock_secret_normalize (verify == VERIFY_PIN ? JR_LOCK_PIN : JR_LOCK_PASSPHRASE,
                                                   req->current);
  JrJobResult refuse = JR_JOB_OK;
  if ((verify == VERIFY_PASSPHRASE || verify == VERIFY_MASTER) && jr_lockout_active (&j->lockout, now))
    refuse = JR_JOB_WAIT;
  else if (what == JR_JOB_CHANGE && verify == VERIFY_PIN && req->new_kind == JR_LOCK_PIN &&
           norm_current != NULL && strcmp (norm_current, norm_new) == 0)
    refuse = JR_JOB_SAME_PIN; /* nothing is checked, so nothing counts */
  if (refuse != JR_JOB_OK)
    {
      jr_secret_free (norm_new);
      jr_secret_free (norm_master);
      jr_secret_free (norm_current);
      *result = refuse;
      return NULL;
    }

  JrLockJob *job = g_new0 (JrLockJob, 1);
  job->what = what;
  job->now = now;
  job->cost = j->cost;
  job->key = jr_key_dup (j->key);
  job->store = j->store;
  job->was_enabled = j->lock_enabled;
  job->was_kind = j->lock_kind;
  job->was_master = j->has_master;
  job->verify = verify;
  job->current = norm_current;
  job->new_kind = req->new_kind;
  job->new_secret = norm_new;
  job->new_master = norm_master;
  if (verify != VERIFY_NONE)
    job->stored = jr_db_get_setting (j->db, verify == VERIFY_MASTER ? K_KEY_MASTER : K_KEY_PIN);
  job->verify_bound = verify == VERIFY_PIN && j->pin_bound;
  job->bind_pin = (new_lock && req->new_kind == JR_LOCK_PIN) || what == JR_JOB_SET_MASTER;
  if (what == JR_JOB_RESET && req->new_kind == JR_LOCK_PIN)
    {
      job->old_pin = jr_db_get_setting (j->db, K_KEY_PIN);
      job->old_pin_bound = j->pin_bound;
    }
  if (job->bind_pin || job->verify_bound || job->old_pin_bound)
    job->device_id = j->device_id != NULL ? g_strdup (j->device_id)
                     : job->bind_pin ? g_uuid_string_random () : NULL;
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
  JrKey *device = NULL;
  if (job->key == NULL)
    {
      job->failed = TRUE;
      return;
    }
  if (job->bind_pin || job->verify_bound || job->old_pin_bound)
    {
      job->device = device_load (job->store, job->device_id, &device);
      /* Checking a PIN needs its secret; a new PIN needs a working keyring. */
      if ((job->verify_bound && job->device != JR_DEVICE_FOUND) ||
          (job->bind_pin && job->device == JR_DEVICE_UNAVAILABLE))
        {
          job->device_failed = TRUE;
          goto out;
        }
    }

  if (job->verify != VERIFY_NONE)
    {
      JrKey *k = job->current != NULL
                     ? jr_key_unwrap_for (job->stored, job->current, job->verify_bound ? device : NULL)
                     : NULL;
      job->verified = k != NULL && jr_key_equal (k, job->key);
      jr_key_free (k);
      if (!job->verified)
        goto out;
    }
  else
    job->verified = TRUE;

  /* After a block, the PIN being replaced is not allowed again. (If its
   * device secret is gone it cannot be compared, and it was not guessed
   * at anyway.) */
  if (job->old_pin != NULL && (!job->old_pin_bound || device != NULL))
    {
      JrKey *k = jr_key_unwrap_for (job->old_pin, job->new_secret, job->old_pin_bound ? device : NULL);
      job->same_pin = k != NULL;
      jr_key_free (k);
      if (job->same_pin)
        goto out;
    }

  if (job->bind_pin && device == NULL)
    {
      /* First PIN on this laptop, or the keyring lost it: a new secret. */
      device = jr_key_generate ();
      if (device == NULL || !job->store->save (job->device_id, device, job->store->user))
        {
          job->device_failed = TRUE;
          goto out;
        }
    }

  switch (job->what)
    {
    case JR_JOB_ENABLE:
    case JR_JOB_CHANGE:
    case JR_JOB_RESET:
      job->wrapped_lock = jr_key_wrap_for (job->key, job->new_secret,
                                           job->new_kind == JR_LOCK_PIN ? device : NULL, job->cost);
      job->failed = job->wrapped_lock == NULL;
      break;
    case JR_JOB_SET_MASTER:
      /* The same PIN, now bound to the device secret. */
      job->wrapped_lock = jr_key_wrap_for (job->key, job->current, device, job->cost);
      job->failed = job->wrapped_lock == NULL;
      break;
    case JR_JOB_NEW_RECOVERY:
      job->failed = !make_recovery (job);
      break;
    case JR_JOB_CHANGE_MASTER:
    case JR_JOB_DISABLE:
    default:
      break;
    }
  if (!job->failed && job->new_master != NULL)
    {
      job->wrapped_master = jr_key_wrap (job->key, job->new_master, job->cost);
      job->failed = job->wrapped_master == NULL;
    }
  if (!job->failed && job->what == JR_JOB_ENABLE)
    job->failed = !make_recovery (job);

out:
  jr_key_free (device);
}

/* Writes the job in one transaction, together with `lockout`. */
static gboolean
apply_job (JrJournal *j, JrLockJob *job, const JrLockout *lockout)
{
  JrDb *db = j->db;
  gboolean pin_now = job->what == JR_JOB_SET_MASTER || job->what == JR_JOB_CHANGE_MASTER ||
                     job->what == JR_JOB_NEW_RECOVERY ? job->was_enabled && job->was_kind == JR_LOCK_PIN
                     : job->what == JR_JOB_DISABLE ? FALSE
                     : job->new_kind == JR_LOCK_PIN;
  gboolean ok = jr_db_begin (db);
  if (job->wrapped_lock != NULL)
    ok = ok && jr_db_set_setting (db, K_KEY_PIN, job->wrapped_lock);
  if (job->wrapped_master != NULL)
    ok = ok && jr_db_set_setting (db, K_KEY_MASTER, job->wrapped_master);
  if (job->wrapped_recovery != NULL)
    ok = ok && jr_db_set_setting (db, K_KEY_RECOVERY, job->wrapped_recovery);
  switch (job->what)
    {
    case JR_JOB_ENABLE:
      ok = ok && jr_db_delete_setting (db, K_KEY_PLAIN);
      G_GNUC_FALLTHROUGH;
    case JR_JOB_CHANGE:
    case JR_JOB_RESET:
      ok = ok && jr_db_set_setting (db, K_LOCK_KIND, job->new_kind == JR_LOCK_PASSPHRASE ? "passphrase" : "pin");
      break;
    case JR_JOB_DISABLE:
      {
        char *plain = jr_key_export (j->key);
        ok = ok && jr_db_set_setting (db, K_KEY_PLAIN, plain) && jr_db_delete_setting (db, K_KEY_PIN) &&
             jr_db_delete_setting (db, K_KEY_RECOVERY) && jr_db_delete_setting (db, K_LOCK_KIND);
        jr_secret_free (plain);
      }
      break;
    default:
      break;
    }
  if (pin_now && job->bind_pin)
    ok = ok && jr_db_set_setting (db, K_DEVICE_ID, job->device_id);
  if (!pin_now)
    ok = ok && jr_db_delete_setting (db, K_KEY_MASTER) && jr_db_delete_setting (db, K_DEVICE_ID);
  char buf[JR_LOCKOUT_STR_LEN];
  jr_lockout_to_string (lockout, buf, sizeof buf);
  ok = ok && jr_db_set_setting (db, K_LOCKOUT, buf);
  /* All or nothing: never leave the file with no way to open it. */
  if (!(ok && jr_db_commit (db)))
    {
      jr_db_rollback (db);
      return FALSE;
    }

  char *old_device = g_strdup (j->device_id);
  j->lockout = *lockout;
  load_lock_state (j);
  if (j->pin_bound)
    j->device_missing = FALSE;
  /* A device secret no PIN uses any more goes from the keyring. */
  if (old_device != NULL && g_strcmp0 (old_device, j->device_id) != 0 && j->store != NULL)
    j->store->clear (old_device, j->store->user);
  g_free (old_device);
  return TRUE;
}

JrJobResult
jr_journal_finish_lock_job (JrJournal *j, JrLockJob *job, char **recovery_out)
{
  JrJobResult result;
  if (job->verify_bound && job->device == JR_DEVICE_MISSING)
    j->device_missing = TRUE;

  if (job->device_failed)
    result = JR_JOB_NO_DEVICE;
  else if (!job->verified)
    {
      result = JR_JOB_WRONG;
      if (job->verify == VERIFY_PIN)
        {
          jr_lockout_pin_fail (&j->lockout);
          if (jr_lockout_pin_blocked (&j->lockout))
            {
              jr_journal_lock (j); /* the 5th wrong PIN: only the master passphrase now */
              result = JR_JOB_BLOCKED;
            }
        }
      else
        jr_lockout_fail (&j->lockout, job->now);
      save_lockout (j);
    }
  else if (job->same_pin)
    result = JR_JOB_SAME_PIN;
  else if (job->failed || j->key == NULL || !jr_key_equal (j->key, job->key) ||
           j->lock_enabled != job->was_enabled || j->lock_kind != job->was_kind ||
           j->has_master != job->was_master)
    result = JR_JOB_FAILED; /* locked or changed meanwhile, or out of memory */
  else
    {
      JrLockout lockout = j->lockout;
      if (job->verify == VERIFY_PIN || job->what == JR_JOB_RESET || job->what == JR_JOB_DISABLE)
        jr_lockout_pin_success (&lockout);
      else if (job->verify != VERIFY_NONE)
        jr_lockout_success (&lockout);
      if (!apply_job (j, job, &lockout))
        result = JR_JOB_FAILED;
      else
        {
          if (recovery_out != NULL)
            {
              *recovery_out = job->recovery; /* ownership moves to the caller */
              job->recovery = NULL;
            }
          result = JR_JOB_OK;
        }
    }
  jr_lock_job_free (job);
  return result;
}

JrJobResult
jr_journal_run_lock_job (JrJournal *j, const JrLockRequest *req, gint64 now, char **recovery_out)
{
  JrJobResult result;
  JrLockJob *job = jr_journal_begin_lock_job (j, req, now, &result);
  if (job == NULL)
    return result;
  jr_lock_job_run (job);
  return jr_journal_finish_lock_job (j, job, recovery_out);
}

char *
jr_journal_enable_lock (JrJournal *j, JrLockKind kind, const char *secret, const char *master)
{
  char *recovery = NULL;
  JrLockRequest req = { JR_JOB_ENABLE, NULL, kind, secret, master };
  jr_journal_run_lock_job (j, &req, 0, &recovery);
  return recovery;
}

JrJobResult
jr_journal_change_lock (JrJournal *j, const char *current, JrLockKind kind, const char *secret,
                        gint64 now)
{
  JrLockRequest req = { JR_JOB_CHANGE, current, kind, secret, NULL };
  return jr_journal_run_lock_job (j, &req, now, NULL);
}

JrJobResult
jr_journal_disable_lock (JrJournal *j, const char *current, gint64 now)
{
  JrLockRequest req = { JR_JOB_DISABLE, current, JR_LOCK_PIN, NULL, NULL };
  return jr_journal_run_lock_job (j, &req, now, NULL);
}

char *
jr_journal_new_recovery_key (JrJournal *j, const char *current, gint64 now, JrJobResult *result)
{
  char *recovery = NULL;
  JrLockRequest req = { JR_JOB_NEW_RECOVERY, current, JR_LOCK_PIN, NULL, NULL };
  JrJobResult r = jr_journal_run_lock_job (j, &req, now, &recovery);
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
  if (!entries_open (j))
    return 0;
  gsize len = 0;
  g_autofree guint8 *blob = jr_seal (j->key, text, strlen (text), &len);
  return jr_db_insert_entry (j->db, day, stamped_at, blob, len, now);
}

gboolean
jr_journal_update_entry (JrJournal *j, gint64 id, const char *text, gint64 now)
{
  if (!entries_open (j))
    return FALSE;
  gsize len = 0;
  g_autofree guint8 *blob = jr_seal (j->key, text, strlen (text), &len);
  return jr_db_update_entry (j->db, id, blob, len, now);
}

gboolean
jr_journal_delete_entry (JrJournal *j, gint64 id)
{
  return entries_open (j) && jr_db_delete_entry (j->db, id);
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
  if (!entries_open (j))
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
