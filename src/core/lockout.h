/* lockout: what wrong tries cost.
 *
 * Passphrases, master passphrases and recovery keys: after 5 wrong tries
 * the app waits 30 seconds; tries during the wait do not count.
 * PINs: after 5 wrong PINs the PIN is blocked. No wait brings it back;
 * only opening the journal another way and choosing a new PIN does.
 *
 * Pure state machine with injected time. The caller persists it with
 * jr_lockout_to_string() so restarting the app resets neither.
 */
#pragma once

#include <glib.h>

#define JR_LOCKOUT_MAX_TRIES 5
#define JR_LOCKOUT_SECS 30
#define JR_PIN_MAX_TRIES 5
#define JR_LOCKOUT_STR_LEN 48

typedef struct {
  int    failed;     /* wrong tries since the last success or wait */
  gint64 until;      /* unix seconds; waiting while now < until */
  int    pin_failed; /* wrong PINs since the last right or new one */
} JrLockout;

gboolean jr_lockout_active         (const JrLockout *l, gint64 now);
/* Whole seconds left to wait, 0 when not waiting. */
gint64   jr_lockout_remaining      (const JrLockout *l, gint64 now);
int      jr_lockout_tries_left     (const JrLockout *l);
/* Records a wrong passphrase or key. Ignored while waiting. */
void     jr_lockout_fail           (JrLockout *l, gint64 now);
/* A right passphrase or key: resets the count and the wait (not a
 * blocked PIN). */
void     jr_lockout_success        (JrLockout *l);

gboolean jr_lockout_pin_blocked    (const JrLockout *l);
int      jr_lockout_pin_tries_left (const JrLockout *l);
/* Records a wrong PIN. Ignored once blocked. */
void     jr_lockout_pin_fail       (JrLockout *l);
/* The right PIN, or a new PIN was set: clears everything. */
void     jr_lockout_pin_success    (JrLockout *l);

/* "failed:until:pin_failed". Older files saved "failed:until". */
void     jr_lockout_to_string      (const JrLockout *l, char *buf, gsize len);
gboolean jr_lockout_from_string    (const char *s, JrLockout *out);
