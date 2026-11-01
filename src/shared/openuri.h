/* openuri: opens a web address in your browser, also from a program
 * that runs locked down (Notes).
 *
 * A program Notes starts inherits Notes' Landlock lock. A browser started
 * that way could not save its own files, and would stay locked until you
 * closed it. So the address goes to the desktop's portal
 * (org.freedesktop.portal.OpenURI, over D-Bus), which starts the browser
 * outside the lock, the way a Flatpak app opens links. Only when there is
 * no portal, or it refuses, is the browser started directly. */
#pragma once

#include <gio/gio.h>

/* Starts a browser directly; FALSE (and why) if none could be. */
typedef gboolean (*JrLaunch) (const char *uri, GError **error);

/* TRUE once the portal or the browser has the address. Waits a few
 * seconds at most for the portal. */
gboolean jr_open_uri      (const char *uri, GError **error);
/* As jr_open_uri, with `launch` in place of starting the browser directly
 * (tests: never a real browser). */
gboolean jr_open_uri_with (const char *uri, JrLaunch launch, GError **error);
