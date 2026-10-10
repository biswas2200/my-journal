/* journal: the app's single data object.
 *
 * Combines the database and the vault and enforces the rules: no entry
 * can be read or written until the journal is unlocked; wrong PINs or
 * passphrases feed a persisted lockout; locking zeroes the key. Entry text exists in memory
 * only for the duration of a callback, then is wiped.
 */
#pragma once

#include <glib.h>
#include "db.h"
#include "vault.h"

typedef struct JrJournal JrJournal;

typedef enum {
  JR_UNLOCK_OK,
  JR_UNLOCK_WRONG,
  JR_UNLOCK_WAIT, /* lockout active, nothing was checked */
} JrUnlockResult;

/* Opens the file. Without a lock it is unlocked at once (a key is created
 * on first run); with a PIN or passphrase it starts locked and no entry is
 * read. */
JrJournal     *jr_journal_open               (const char *path, GError **error);
/* Zeroes the key and closes the file. NULL is fine. */
void           jr_journal_close              (JrJournal *j);
void           jr_journal_set_kdf_cost       (JrJournal *j, JrKdfCost cost);

/* TRUE when a PIN or passphrase is set. */
gboolean       jr_journal_lock_enabled       (JrJournal *j);
/* PIN or passphrase (not secret; readable while locked). */
JrLockKind     jr_journal_lock_kind          (JrJournal *j);
gboolean       jr_journal_is_unlocked        (JrJournal *j);

/* Unlock in one call with the PIN or passphrase. Slow (Argon2id, ~1 s). */
JrUnlockResult jr_journal_unlock             (JrJournal *j, const char *secret, gint64 now);
JrUnlockResult jr_journal_unlock_recovery    (JrJournal *j, const char *recovery, gint64 now);
/* The same in three steps so the slow part can run on a worker thread:
 * begin and finish use the database (UI thread only); run touches nothing
 * but the attempt. begin returns NULL during a lockout (JR_UNLOCK_WAIT).
 * A successful unlock also re-saves the key with the current Argon2id
 * cost if it was saved with a weaker one. */
typedef enum { JR_SECRET_LOCK, JR_SECRET_RECOVERY } JrSecretKind;
typedef struct JrUnlockAttempt JrUnlockAttempt;
JrUnlockAttempt *jr_journal_begin_unlock     (JrJournal *j, JrSecretKind kind,
                                              const char *secret, gint64 now);
void           jr_unlock_attempt_run         (JrUnlockAttempt *a);
/* Applies the result and frees the attempt. */
JrUnlockResult jr_journal_finish_unlock      (JrJournal *j, JrUnlockAttempt *a);
/* Drops an attempt without applying it (wipes the secret). */
void           jr_unlock_attempt_free        (JrUnlockAttempt *a);

gint64         jr_journal_lockout_remaining  (JrJournal *j, gint64 now);
int            jr_journal_tries_left         (JrJournal *j);
/* Zeroes the key. No-op without a lock (there is nothing to unlock with). */
void           jr_journal_lock               (JrJournal *j);

/* Changing the lock, in the same three steps (the slow Argon2id work runs
 * in jr_lock_job_run on private copies). All need the journal unlocked.
 * Every job except ENABLE needs the current PIN or passphrase; a wrong one
 * counts toward the lockout. A job finished after the journal was locked
 * is not applied. Recovery keys are shown once; free with jr_secret_free. */
typedef enum {
  JR_JOB_ENABLE,       /* set a lock (new_kind, new_secret) */
  JR_JOB_CHANGE,       /* replace it (current, new_kind, new_secret) */
  JR_JOB_DISABLE,      /* remove it (current) */
  JR_JOB_NEW_RECOVERY, /* replace the recovery key (current) */
} JrLockJobKind;

typedef enum {
  JR_JOB_OK,
  JR_JOB_WRONG,   /* current PIN or passphrase was wrong */
  JR_JOB_WAIT,    /* lockout active, nothing was checked */
  JR_JOB_INVALID, /* not allowed now, or the new secret is not valid */
  JR_JOB_FAILED,  /* locked meanwhile, or could not save */
} JrJobResult;

typedef struct JrLockJob JrLockJob;
/* Returns NULL with *result set when the job cannot start. */
JrLockJob     *jr_journal_begin_lock_job     (JrJournal *j, JrLockJobKind what, const char *current,
                                              JrLockKind new_kind, const char *new_secret,
                                              gint64 now, JrJobResult *result);
void           jr_lock_job_run               (JrLockJob *job);
/* Applies the job and frees it. `recovery_out` may be NULL. */
JrJobResult    jr_journal_finish_lock_job    (JrJournal *j, JrLockJob *job, char **recovery_out);
void           jr_lock_job_free              (JrLockJob *job);

/* One-call versions of the jobs. */
char          *jr_journal_enable_lock        (JrJournal *j, JrLockKind kind, const char *secret);
JrJobResult    jr_journal_change_lock        (JrJournal *j, const char *current, JrLockKind kind,
                                              const char *secret, gint64 now);
JrJobResult    jr_journal_disable_lock       (JrJournal *j, const char *current, gint64 now);
char          *jr_journal_new_recovery_key   (JrJournal *j, const char *current, gint64 now,
                                              JrJobResult *result);

gboolean       jr_journal_lock_on_sleep      (JrJournal *j);
void           jr_journal_set_lock_on_sleep  (JrJournal *j, gboolean on);

/* Entries: return 0 / FALSE when locked or on error. */
gint64         jr_journal_add_entry          (JrJournal *j, const char *day, gint64 stamped_at,
                                              const char *text, gint64 now);
gboolean       jr_journal_update_entry       (JrJournal *j, gint64 id, const char *text, gint64 now);
gboolean       jr_journal_delete_entry       (JrJournal *j, gint64 id);

/* `text` is wiped right after each call; copy what you keep. */
typedef void (*JrEntryFunc) (gint64 id, gint64 stamped_at, const char *text, gpointer user);
gboolean       jr_journal_foreach_entry      (JrJournal *j, const char *day,
                                              JrEntryFunc fn, gpointer user);

/* Time and day statistics hold no secrets and need no key. */
void           jr_journal_add_active_seconds (JrJournal *j, const char *day, gint64 seconds);
gint64         jr_journal_active_seconds     (JrJournal *j, const char *day);
gboolean       jr_journal_foreach_day        (JrJournal *j, const char *from, const char *to,
                                              JrDbDayFunc fn, gpointer user);
gboolean       jr_journal_foreach_month      (JrJournal *j, const char *before_month,
                                              JrDbMonthFunc fn, gpointer user);
