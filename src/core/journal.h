/* journal: the app's single data object.
 *
 * Combines the database and the vault and enforces the rules: no entry
 * can be read or written until the journal is unlocked; wrong PINs feed a
 * persisted lockout; locking zeroes the key. Entry text exists in memory
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

/* Opens the file. Without a PIN it is unlocked at once (a key is created
 * on first run); with a PIN it starts locked and no entry is read. */
JrJournal     *jr_journal_open               (const char *path, GError **error);
/* Zeroes the key and closes the file. NULL is fine. */
void           jr_journal_close              (JrJournal *j);
void           jr_journal_set_kdf_cost       (JrJournal *j, JrKdfCost cost);

gboolean       jr_journal_pin_enabled        (JrJournal *j);
gboolean       jr_journal_is_unlocked        (JrJournal *j);

/* Unlock in one call. Slow (Argon2id, ~0.5 s). */
JrUnlockResult jr_journal_unlock_pin         (JrJournal *j, const char *pin, gint64 now);
JrUnlockResult jr_journal_unlock_recovery    (JrJournal *j, const char *recovery, gint64 now);
/* The same in three steps so the slow part can run on a worker thread:
 * begin and finish use the database (UI thread only); run touches nothing
 * but the attempt. begin returns NULL during a lockout (JR_UNLOCK_WAIT). */
typedef enum { JR_SECRET_PIN, JR_SECRET_RECOVERY } JrSecretKind;
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
/* Zeroes the key. No-op without a PIN (there is nothing to unlock with). */
void           jr_journal_lock               (JrJournal *j);

/* All need the journal unlocked. Returned recovery keys are shown once;
 * free them with jr_secret_free. */
char          *jr_journal_enable_pin         (JrJournal *j, const char *pin);
gboolean       jr_journal_change_pin         (JrJournal *j, const char *pin);
char          *jr_journal_new_recovery_key   (JrJournal *j);
gboolean       jr_journal_disable_pin        (JrJournal *j);

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
