/* netguard: which links and addresses the AI helper may fetch.
 *
 * Links: http or https only, on their usual ports (80, 443), with a host
 * and no "user:password@" part; a bare "example.com/x" means https. The
 * #fragment is dropped (it never reaches a server).
 * Addresses: the public internet only. Never this laptop, the home
 * network, link-local (where cloud metadata lives), carrier NAT,
 * multicast, broadcast, documentation or reserved ranges, in IPv4 or
 * IPv6, also when an IPv4 address hides inside IPv6 (::ffff:, 64:ff9b::).
 * So a page or a pasted link cannot make the helper reach local services. */
#pragma once

#include <gio/gio.h>

typedef enum {
  JN_URL_OK,
  JN_URL_BAD_SCHEME,
  JN_URL_NO_HOST,
  JN_URL_CREDENTIALS,
  JN_URL_BAD_PORT,
  JN_URL_INVALID,
  JN_URL_TOO_LONG,
} JnUrlCheck;

#define JN_URL_MAX 4096

/* On OK, *normalized gets the link to fetch; otherwise NULL. */
JnUrlCheck  jn_url_check         (const char *url, char **normalized);
/* A short reason to show next to a source that was not fetched. */
const char *jn_url_check_message (JnUrlCheck check);
gboolean    jn_address_public    (GInetAddress *address);
