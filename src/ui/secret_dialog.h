/* secret_dialog: small modal windows for PIN or passphrase steps, and the
 * one-time recovery key.
 *
 * A secret dialog walks through one or more steps ("Current PIN", "New
 * passphrase", "Again"). A PIN step takes six digits from the keyboard; a
 * passphrase step is a hidden text field finished with Enter. When all
 * steps are done it hands every secret to `done` and wipes them as soon as
 * `done` returns. `done` then destroys the dialog, calls
 * jr_secret_dialog_restart() with an error, or jr_secret_dialog_set_busy()
 * while slow work runs elsewhere. */
#pragma once

#include <gtk/gtk.h>
#include "vault.h"
#include "window.h"

#define JR_SECRET_DIALOG_MAX_STEPS 3

typedef struct {
  const char *prompt;
  JrLockKind  kind;
  gboolean    is_new; /* a new secret: check the rules before moving on */
} JrSecretStep;

typedef void (*JrSecretDone) (GtkWindow *dialog, const char *const *secrets, gpointer user);

GtkWindow *jr_secret_dialog_new      (JrWindow *parent, const char *title,
                                      const JrSecretStep *steps, int n_steps,
                                      JrSecretDone done, gpointer user);
void       jr_secret_dialog_restart  (GtkWindow *dialog, const char *error);
/* Shows `message` and ignores input until restarted or destroyed. */
void       jr_secret_dialog_set_busy (GtkWindow *dialog, const char *message);

/* Asks whether to lock with a PIN or a passphrase; calls `chosen` (after
 * closing itself) unless cancelled. */
typedef void (*JrKindChosen) (JrWindow *win, JrLockKind kind);
void       jr_lock_kind_dialog_show  (JrWindow *parent, JrKindChosen chosen);

/* Shows `key` once, to be written down. It cannot be copied or selected,
 * so it never reaches the clipboard. */
void       jr_recovery_dialog_show   (JrWindow *parent, const char *key);
