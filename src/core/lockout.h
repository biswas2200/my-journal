/* lockout: wait 30 seconds after 5 wrong PINs.
 *
 * Pure state machine with injected time. The caller persists it with
 * jr_lockout_to_string() so restarting the app does not reset the wait.
 */
#pragma once

#include <glib.h>

#define JR_LOCKOUT_MAX_TRIES 5
#define JR_LOCKOUT_SECS 30
#define JR_LOCKOUT_STR_LEN 32

typedef struct {
  int    failed; /* wrong tries since the last success or lockout */
  gint64 until;  /* unix seconds; locked out while now < until */
} JrLockout;

gboolean jr_lockout_active      (const JrLockout *l, gint64 now);
/* Whole seconds left to wait, 0 when not locked out. */
gint64   jr_lockout_remaining   (const JrLockout *l, gint64 now);
int      jr_lockout_tries_left  (const JrLockout *l);
/* Records a wrong try. Ignored while locked out. */
void     jr_lockout_fail        (JrLockout *l, gint64 now);
void     jr_lockout_success     (JrLockout *l);

void     jr_lockout_to_string   (const JrLockout *l, char *buf, gsize len);
gboolean jr_lockout_from_string (const char *s, JrLockout *out);
