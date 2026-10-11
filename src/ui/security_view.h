/* security_view: screen 05, Lock & security. */
#pragma once

#include <gtk/gtk.h>
#include "window.h"

GtkWidget *jr_security_view_new (JrWindow *win, GtkWidget **header_out);

/* Required steps after unlocking, before the journal shows: a new PIN
 * (blocked, or its keyring secret is gone), or a master passphrase for a
 * PIN set before they existed. Closing the dialog locks the journal. */
void       jr_security_require_new_pin (JrWindow *win);
void       jr_security_require_master  (JrWindow *win);
