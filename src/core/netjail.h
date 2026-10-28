/* netjail: no network for this process, its threads, and every program
 * it starts. The Journal enters it first thing in main().
 *
 * After jr_netjail_enter() only local AF_UNIX sockets can be created
 * (needed for the display, D-Bus and the keyring). Any other socket, and
 * io_uring (which could open sockets without socket()), fails with
 * EACCES before a single byte leaves: nothing goes out, so nothing can
 * come back. Each attempt is counted and logged on stderr. Programs the
 * Journal starts inherit the filter but not the handler, so the kernel
 * ends them on their first network call.
 *
 * Built on seccomp; it cannot be undone. A second, independent layer is
 * the AppArmor profile in data/apparmor (README, Security model). */
#pragma once

#include <glib.h>

/* FALSE if the kernel refused the filter; the caller must not go on. */
gboolean jr_netjail_enter   (void);
/* Network calls stopped in this process so far. */
guint    jr_netjail_blocked (void);
