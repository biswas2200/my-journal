/* Tests for which links the AI helper may fetch (src/notes/ai/netguard.c),
 * LeetCode style (see cases.h).
 *
 * Constraints: a link is whatever was pasted into Sources (any text, up
 * to 4 KB). Only http and https on their usual ports, with a host and no
 * user:password part, are fetched; a bare "example.com/x" means https.
 * An address is fetched only if it is on the public internet: never this
 * laptop, the home network, link-local, carrier NAT, multicast or
 * reserved ranges, in IPv4 or IPv6 (including IPv4 inside IPv6). */
#include <string.h>
#include <gio/gio.h>
#include "cases.h"
#include "netguard.h"

static const char *
check_name (JnUrlCheck c)
{
  switch (c)
    {
    case JN_URL_OK:          return "ok";
    case JN_URL_BAD_SCHEME:  return "bad scheme";
    case JN_URL_NO_HOST:     return "no host";
    case JN_URL_CREDENTIALS: return "credentials";
    case JN_URL_BAD_PORT:    return "bad port";
    case JN_URL_INVALID:     return "invalid";
    case JN_URL_TOO_LONG:    return "too long";
    }
  return "?";
}

typedef struct {
  const char *name, *url;
  JnUrlCheck  want;
  const char *normalized; /* when ok */
} UrlCase;

static const UrlCase url_cases[] = {
  { "https",                 "https://cp-algorithms.com/data_structures/prefix_sum.html", JN_URL_OK,
                             "https://cp-algorithms.com/data_structures/prefix_sum.html" },
  { "http",                  "http://example.com/a?b=c#frag", JN_URL_OK, "http://example.com/a?b=c" },
  { "bare host means https", "youtube.com/watch?v=abc", JN_URL_OK, "https://youtube.com/watch?v=abc" },
  { "spaces around",         "  https://example.com  ", JN_URL_OK, "https://example.com/" },
  { "explicit 443",          "https://example.com:443/x", JN_URL_OK, "https://example.com/x" },
  { "uppercase scheme",      "HTTPS://Example.com/X", JN_URL_OK, "https://example.com/X" },
  { "file scheme",           "file:///home/me/.local/share/journal/journal.db", JN_URL_BAD_SCHEME, NULL },
  { "ftp",                   "ftp://example.com/x", JN_URL_BAD_SCHEME, NULL },
  { "javascript",            "javascript:alert(1)", JN_URL_BAD_SCHEME, NULL },
  { "data",                  "data:text/html,hi", JN_URL_BAD_SCHEME, NULL },
  { "user and password",     "https://me:secret@example.com/", JN_URL_CREDENTIALS, NULL },
  { "user only",             "https://me@example.com/", JN_URL_CREDENTIALS, NULL },
  { "other port",            "https://example.com:8443/", JN_URL_BAD_PORT, NULL },
  { "http on 443",           "http://example.com:443/", JN_URL_OK, "http://example.com:443/" },
  { "port 22",               "http://example.com:22/", JN_URL_BAD_PORT, NULL },
  { "no host",               "https:///path", JN_URL_NO_HOST, NULL },
  { "empty",                 "", JN_URL_INVALID, NULL },
  { "spaces only",           "   ", JN_URL_INVALID, NULL },
  { "not a link",            "my notes about sums", JN_URL_INVALID, NULL },
  { "control characters",    "https://exa\nmple.com/", JN_URL_INVALID, NULL },
};

static void
test_urls (void)
{
  JrCases c = { "urls", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (url_cases); i++)
    {
      const UrlCase *k = &url_cases[i];
      g_autofree char *norm = NULL;
      JnUrlCheck got = jn_url_check (k->url, &norm);
      gboolean ok = got == k->want && (k->want != JN_URL_OK || g_strcmp0 (norm, k->normalized) == 0) &&
                    (k->want == JN_URL_OK || norm == NULL);
      jr_case (&c, k->name, ok, "Input \"%s\"  Expected %s %s  Got %s %s", k->url, check_name (k->want),
               k->normalized ? k->normalized : "", check_name (got), norm ? norm : "");
      jr_case (&c, k->name, jn_url_check_message (got) != NULL && *jn_url_check_message (got) != '\0',
               "Expected a message");
    }
  g_autofree char *tail = g_strnfill (5000, 'a');
  g_autofree char *big = g_strconcat ("https://example.com/", tail, NULL);
  g_autofree char *norm = NULL;
  jr_case (&c, "too long", jn_url_check (big, &norm) == JN_URL_TOO_LONG && norm == NULL, "Expected too long");
  jr_cases_done (&c);
}

typedef struct {
  const char *name, *address;
  gboolean    public;
} AddrCase;

static const AddrCase addr_cases[] = {
  { "a public IPv4",          "142.250.183.42", TRUE },
  { "another public IPv4",    "1.1.1.1",        TRUE },
  { "loopback",               "127.0.0.1",      FALSE },
  { "loopback range",         "127.255.255.254", FALSE },
  { "this network",           "0.0.0.0",        FALSE },
  { "0/8",                    "0.1.2.3",        FALSE },
  { "home 192.168",           "192.168.1.1",    FALSE },
  { "home 10",                "10.0.0.5",       FALSE },
  { "home 172.16",            "172.16.0.1",     FALSE },
  { "edge 172.31",            "172.31.255.255", FALSE },
  { "just outside 172.32",    "172.32.0.1",     TRUE },
  { "just outside 172.15",    "172.15.255.255", TRUE },
  { "link-local (cloud metadata)", "169.254.169.254", FALSE },
  { "carrier NAT",            "100.64.0.1",     FALSE },
  { "just outside carrier NAT","100.128.0.1",   TRUE },
  { "multicast",              "224.0.0.251",    FALSE },
  { "broadcast",              "255.255.255.255", FALSE },
  { "reserved 240/4",         "240.0.0.1",      FALSE },
  { "benchmark 198.18",       "198.18.0.1",     FALSE },
  { "documentation",          "192.0.2.1",      FALSE },
  { "IETF 192.0.0",           "192.0.0.8",      FALSE },
  { "a public IPv6",          "2607:f8b0:4005:80b::200e", TRUE },
  { "IPv6 loopback",          "::1",            FALSE },
  { "IPv6 unspecified",       "::",             FALSE },
  { "IPv6 link-local",        "fe80::1",        FALSE },
  { "IPv6 unique local",      "fd12:3456::1",   FALSE },
  { "IPv6 multicast",         "ff02::1",        FALSE },
  { "IPv6 documentation",     "2001:db8::1",    FALSE },
  { "IPv4 inside IPv6, private","::ffff:192.168.0.1", FALSE },
  { "IPv4 inside IPv6, loopback","::ffff:127.0.0.1",  FALSE },
  { "IPv4 inside IPv6, public","::ffff:1.1.1.1", TRUE },
  { "NAT64 of a private one", "64:ff9b::c0a8:1", FALSE },
};

static void
test_addresses (void)
{
  JrCases c = { "addresses", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (addr_cases); i++)
    {
      g_autoptr (GInetAddress) a = g_inet_address_new_from_string (addr_cases[i].address);
      g_assert_nonnull (a);
      gboolean got = jn_address_public (a);
      jr_case (&c, addr_cases[i].name, got == addr_cases[i].public, "Input %s  Expected %s  Got %s",
               addr_cases[i].address, addr_cases[i].public ? "public" : "refused", got ? "public" : "refused");
    }
  /* Hidden: every 10/8, 192.168/16 and 127/8 address is refused. */
  for (int i = 0; i < 20000; i++)
    {
      static const char *nets[] = { "10.%d.%d.%d", "192.168.%d.%d%s", "127.%d.%d.%d" };
      int which = g_test_rand_int_range (0, 3);
      g_autofree char *s = which == 1 ? g_strdup_printf (nets[1], g_test_rand_int_range (0, 256),
                                                         g_test_rand_int_range (0, 256), "")
                                      : g_strdup_printf (nets[which], g_test_rand_int_range (0, 256),
                                                         g_test_rand_int_range (0, 256), g_test_rand_int_range (0, 256));
      g_autoptr (GInetAddress) a = g_inet_address_new_from_string (s);
      jr_case (&c, "private (hidden)", a != NULL && !jn_address_public (a), "Input %s", s);
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/netguard/urls", test_urls);
  g_test_add_func ("/netguard/addresses", test_addresses);
  return g_test_run ();
}
