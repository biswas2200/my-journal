/* journal: the app's single data object.
 *
 * Combines the database and the vault and enforces the rules: no entry
 * can be read or written until the journal is unlocked; locking zeroes
 * the key. Entry text exists in memory only for the duration of a
 * callback, then is wiped.
 *
 * Locks are a passphrase, or a PIN with a master passphrase:
 * - A PIN's key also needs a device secret kept in the login keyring
 *   (see vault.h), so a copy of the file cannot be opened with any PIN.
 * - 5 wrong PINs block the PIN (lockout.h). Only the master passphrase or
 *   the recovery key opens the journal then, and a new PIN, different
 *   from the old one, must be set before any entry can be read.
 * - 5 wrong passphrases, master passphrases or recovery keys start a
 *   30-second wait.
 */
#pragma once

#include <glib.h>
#include "db.h"
#include "vault.h"

typedef struct JrJournal JrJournal;

/* Where the device secret lives: the login keyring in the app, memory in
 * tests. load and save block (D-Bus), so the journal calls them only in
 * the run steps below, on worker threads. clear must not block (fire and
 * forget); it is called from the UI thread after a PIN is removed. */
typedef enum {
  JR_DEVICE_FOUND,
  JR_DEVICE_MISSING,     /* the keyring works but has no such secret */
  JR_DEVICE_UNAVAILABLE, /* no keyring, or it is locked */
} JrDeviceStatus;

typedef struct {
  JrDeviceStatus (*load)  (const char *id, JrKey **out, gpointer user);
  gboolean       (*save)  (const char *id, const JrKey *secret, gpointer user);
  void           (*clear) (const char *id, gpointer user);
  gpointer         user;
} JrDeviceStore;

typedef enum {
  JR_UNLOCK_OK,
  JR_UNLOCK_WRONG,
  JR_UNLOCK_WAIT,      /* a 30-second wait is on; nothing was checked */
  JR_UNLOCK_BLOCKED,   /* the PIN is blocked: use the master passphrase or recovery key */
  JR_UNLOCK_NO_DEVICE, /* the PIN's device secret is not available; nothing was counted */
} JrUnlockResult;

/* Opens the file. Without a lock it is unlocked at once (a key is created
 * on first run); with a lock it starts locked and no entry is read. */
JrJournal     *jr_journal_open               (const char *path, GError **error);
/* Zeroes the key and closes the file. NULL is fine. */
void           jr_journal_close              (JrJournal *j);
void           jr_journal_set_kdf_cost       (JrJournal *j, JrKdfCost cost);
/* `store` must outlive the journal. Without one, PINs cannot be used. */
void           jr_journal_set_device_store   (JrJournal *j, const JrDeviceStore *store);

/* TRUE when a PIN or passphrase is set. */
gboolean       jr_journal_lock_enabled       (JrJournal *j);
/* PIN or passphrase (not secret; readable while locked). */
JrLockKind     jr_journal_lock_kind          (JrJournal *j);
gboolean       jr_journal_is_unlocked        (JrJournal *j);
/* PIN locks only. */
gboolean       jr_journal_has_master         (JrJournal *j);
gboolean       jr_journal_pin_blocked        (JrJournal *j);
int            jr_journal_pin_tries_left     (JrJournal *j);
/* After unlocking: a new PIN must be set (JR_JOB_RESET) before entries
 * open, because the PIN was blocked or its device secret is gone. */
gboolean       jr_journal_needs_new_pin      (JrJournal *j);
/* A PIN set before master passphrases existed: one must be added
 * (JR_JOB_SET_MASTER). Until then the PIN alone guards the file. */
gboolean       jr_journal_needs_master       (JrJournal *j);

/* Unlock in one call. Slow (Argon2id, ~1 s). LOCK is the PIN or the
 * passphrase; MASTER exists only for PINs. */
typedef enum { JR_SECRET_LOCK, JR_SECRET_MASTER, JR_SECRET_RECOVERY } JrSecretKind;
JrUnlockResult jr_journal_unlock_with        (JrJournal *j, JrSecretKind kind, const char *secret,
                                              gint64 now);
JrUnlockResult jr_journal_unlock             (JrJournal *j, const char *secret, gint64 now);
JrUnlockResult jr_journal_unlock_recovery    (JrJournal *j, const char *recovery, gint64 now);
/* The same in three steps so the slow part can run on a worker thread:
 * begin and finish use the database (UI thread only); run touches only
 * the attempt and the device store. begin returns NULL with *refused set
 * when nothing can be checked (WAIT, BLOCKED, or WRONG for a master
 * passphrase that does not exist). A successful unlock also re-saves the
 * key with the current Argon2id cost if it was saved with a weaker one. */
typedef struct JrUnlockAttempt JrUnlockAttempt;
JrUnlockAttempt *jr_journal_begin_unlock     (JrJournal *j, JrSecretKind kind,
                                              const char *secret, gint64 now,
                                              JrUnlockResult *refused);
void           jr_unlock_attempt_run         (JrUnlockAttempt *a);
/* Applies the result and frees the attempt. */
JrUnlockResult jr_journal_finish_unlock      (JrJournal *j, JrUnlockAttempt *a);
/* Drops an attempt without applying it (wipes the secret). */
void           jr_unlock_attempt_free        (JrUnlockAttempt *a);

/* The 30-second wait (passphrases, master passphrases, recovery keys). */
gint64         jr_journal_lockout_remaining  (JrJournal *j, gint64 now);
int            jr_journal_tries_left         (JrJournal *j);
/* Zeroes the key. No-op without a lock (there is nothing to unlock with). */
void           jr_journal_lock               (JrJournal *j);

/* Changing the lock, in the same three steps (the slow Argon2id work and
 * the keyring run in jr_lock_job_run on private copies). All need the
 * journal unlocked; while a new PIN is needed only RESET is allowed. A
 * wrong current PIN counts toward the block, and the 5th locks the
 * journal (JR_JOB_BLOCKED). A job finished after the journal was locked
 * is not applied. Recovery keys are shown once; free with jr_secret_free. */
typedef enum {
  JR_JOB_ENABLE,        /* set a lock: new_kind, new_secret, and new_master for a PIN */
  JR_JOB_CHANGE,        /* replace it (current): new_kind, new_secret, and new_master
                         * when the result is a PIN without a master passphrase yet */
  JR_JOB_DISABLE,       /* remove it (current) */
  JR_JOB_NEW_RECOVERY,  /* replace the recovery key (current) */
  JR_JOB_CHANGE_MASTER, /* current = the master passphrase; new_master */
  JR_JOB_SET_MASTER,    /* a PIN without one: current = the PIN; new_master */
  JR_JOB_RESET,         /* a new PIN is needed: new_kind, new_secret, and new_master
                         * when there is none; no current (the journal was just opened
                         * with the master passphrase or recovery key) */
} JrLockJobKind;

typedef enum {
  JR_JOB_OK,
  JR_JOB_WRONG,     /* the current secret was wrong */
  JR_JOB_WAIT,      /* a 30-second wait is on; nothing was checked */
  JR_JOB_BLOCKED,   /* that was the 5th wrong PIN: blocked, and the journal locked */
  JR_JOB_INVALID,   /* not allowed now, or a new secret is not valid */
  JR_JOB_SAME_PIN,  /* the new PIN is the one it replaces */
  JR_JOB_NO_DEVICE, /* the login keyring is not available; nothing changed */
  JR_JOB_FAILED,    /* locked meanwhile, or could not save */
} JrJobResult;

typedef struct {
  JrLockJobKind what;
  const char   *current;
  JrLockKind    new_kind;
  const char   *new_secret;
  const char   *new_master;
} JrLockRequest;

typedef struct JrLockJob JrLockJob;
/* Returns NULL with *result set when the job cannot start. */
JrLockJob     *jr_journal_begin_lock_job     (JrJournal *j, const JrLockRequest *req, gint64 now,
                                              JrJobResult *result);
void           jr_lock_job_run               (JrLockJob *job);
/* Applies the job and frees it. `recovery_out` may be NULL. */
JrJobResult    jr_journal_finish_lock_job    (JrJournal *j, JrLockJob *job, char **recovery_out);
void           jr_lock_job_free              (JrLockJob *job);

/* One call: begin, run and finish. */
JrJobResult    jr_journal_run_lock_job       (JrJournal *j, const JrLockRequest *req, gint64 now,
                                              char **recovery_out);
/* Shortcuts for the common jobs. */
char          *jr_journal_enable_lock        (JrJournal *j, JrLockKind kind, const char *secret,
                                              const char *master);
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
