/* keyring: where a PIN's device secret lives, the user's login keyring
 * (Secret Service, via libsecret). The keyring is locked by the login
 * password, so a copy of the journal file alone, or a laptop that is off,
 * gives nothing to try PINs against. Anyone using the logged-in session
 * can read it, though, like everything else in that session. */
#pragma once

#include "journal.h"

const JrDeviceStore *jr_keyring_store (void);
