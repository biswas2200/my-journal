/* netguard: see netguard.h. */
#include "netguard.h"

#include <string.h>

const char *
jn_url_check_message (JnUrlCheck check)
{
  switch (check)
    {
    case JN_URL_OK:          return "ok";
    case JN_URL_BAD_SCHEME:  return "only http and https links are fetched";
    case JN_URL_NO_HOST:     return "the link has no website in it";
    case JN_URL_CREDENTIALS: return "links with a user name or password are not fetched";
    case JN_URL_BAD_PORT:    return "only the usual web ports (80, 443) are used";
    case JN_URL_INVALID:     return "this is not a link";
    case JN_URL_TOO_LONG:    return "the link is too long";
    }
  return "not fetched";
}

JnUrlCheck
jn_url_check (const char *url, char **normalized)
{
  *normalized = NULL;
  if (strlen (url) > JN_URL_MAX)
    return JN_URL_TOO_LONG;
  g_autofree char *u = g_strstrip (g_strdup (url));
  if (*u == '\0')
    return JN_URL_INVALID;
  for (const char *p = u; *p != '\0'; p++)
    if ((guchar) *p < 0x21 || *p == 0x7f)
      return JN_URL_INVALID; /* spaces, newlines, control characters */
  g_autofree char *scheme_peek = g_strdup (g_uri_peek_scheme (u));
  if (scheme_peek == NULL)
    {
      if (strchr (u, '.') == NULL)
        return JN_URL_INVALID;
      char *with = g_strconcat ("https://", u, NULL);
      g_free (u);
      u = with;
    }
  g_autoptr (GUri) uri = g_uri_parse (u, G_URI_FLAGS_NONE, NULL);
  if (uri == NULL)
    return scheme_peek != NULL && g_strcmp0 (scheme_peek, "http") != 0 && g_strcmp0 (scheme_peek, "https") != 0
             ? JN_URL_BAD_SCHEME : JN_URL_INVALID;
  g_autofree char *scheme = g_ascii_strdown (g_uri_get_scheme (uri), -1);
  gboolean https = strcmp (scheme, "https") == 0;
  if (!https && strcmp (scheme, "http") != 0)
    return JN_URL_BAD_SCHEME;
  if (g_uri_get_userinfo (uri) != NULL)
    return JN_URL_CREDENTIALS;
  const char *host = g_uri_get_host (uri);
  if (host == NULL || *host == '\0')
    return JN_URL_NO_HOST;
  int port = g_uri_get_port (uri);
  if (port != -1 && port != 80 && port != 443)
    return JN_URL_BAD_PORT;
  if (port == (https ? 443 : 80))
    port = -1; /* the default: not written */
  const char *path = g_uri_get_path (uri);
  g_autofree char *lhost = g_ascii_strdown (host, -1);
  *normalized = g_uri_join (G_URI_FLAGS_NONE, scheme, NULL, lhost, port, *path != '\0' ? path : "/",
                            g_uri_get_query (uri), NULL);
  return JN_URL_OK;
}

/* IPv4 rules on four bytes. */
static gboolean
public_v4 (const guint8 *b)
{
  if (b[0] == 0 || b[0] == 10 || b[0] == 127)                  /* this network, private, loopback */
    return FALSE;
  if (b[0] == 100 && (b[1] & 0xC0) == 64)                      /* carrier NAT 100.64/10 */
    return FALSE;
  if (b[0] == 169 && b[1] == 254)                              /* link-local */
    return FALSE;
  if (b[0] == 172 && (b[1] & 0xF0) == 16)                      /* private 172.16/12 */
    return FALSE;
  if (b[0] == 192 && b[1] == 168)                              /* private */
    return FALSE;
  if (b[0] == 192 && b[1] == 0 && (b[2] == 0 || b[2] == 2))    /* IETF, documentation */
    return FALSE;
  if (b[0] == 198 && (b[1] & 0xFE) == 18)                      /* benchmarking 198.18/15 */
    return FALSE;
  if (b[0] == 198 && b[1] == 51 && b[2] == 100)                /* documentation */
    return FALSE;
  if (b[0] == 203 && b[1] == 0 && b[2] == 113)                 /* documentation */
    return FALSE;
  if (b[0] >= 224)                                             /* multicast, reserved, broadcast */
    return FALSE;
  return TRUE;
}

gboolean
jn_address_public (GInetAddress *address)
{
  const guint8 *b = g_inet_address_to_bytes (address);
  if (g_inet_address_get_family (address) == G_SOCKET_FAMILY_IPV4)
    return public_v4 (b);
  static const guint8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
  static const guint8 nat64[12] = { 0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0 };
  if (memcmp (b, mapped, 12) == 0 || memcmp (b, nat64, 12) == 0)
    return public_v4 (b + 12);                                 /* IPv4 inside IPv6 */
  static const guint8 zero[15] = { 0 };
  if (memcmp (b, zero, 15) == 0)                               /* :: and ::1 */
    return FALSE;
  if ((b[0] & 0xFE) == 0xFC)                                   /* unique local fc00::/7 */
    return FALSE;
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80)                   /* link-local fe80::/10 */
    return FALSE;
  if (b[0] == 0xFF)                                            /* multicast */
    return FALSE;
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) /* documentation 2001:db8::/32 */
    return FALSE;
  return (b[0] & 0xE0) == 0x20;                                /* global unicast is 2000::/3 */
}
