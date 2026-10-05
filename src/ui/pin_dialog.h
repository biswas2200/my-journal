/* pin_dialog: small modal windows for PIN steps and the recovery key.
 *
 * A PIN dialog walks through one or more prompts ("Current PIN", "New
 * PIN", "Again"), six digits each, and hands all PINs to `done`. The PINs
 * are wiped as soon as `done` returns. `done` either destroys the dialog
 * or calls jr_pin_dialog_restart() with an error. */
#pragma once

#include <gtk/gtk.h>
#include "window.h"

typedef void (*JrPinDone) (GtkWindow *dialog, const char *const *pins, gpointer user);

GtkWindow *jr_pin_dialog_new      (JrWindow *parent, const char *title,
                                   const char *const *prompts, JrPinDone done, gpointer user);
void       jr_pin_dialog_restart  (GtkWindow *dialog, const char *error);

/* Shows `key` once with Copy and "I have saved it" buttons. */
void       jr_recovery_dialog_show (JrWindow *parent, const char *key);
