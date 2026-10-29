/* Tests for fetching Sources (src/notes/ai/fetch.c) from a local web
 * server, LeetCode style (see cases.h).
 *
 * The server is on this machine, so it is a "private" address: the
 * cases that should read a page switch both guards off; the guard cases
 * switch on one guard at a time and must be refused before anything is
 * sent (the server counts every request it gets). */
#include <string.h>
#include <libsoup/soup.h>
#include "cases.h"
#include "fetch.h"

static int requests;

static void
serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user)
{
  (void) server; (void) query; (void) user;
  g_atomic_int_inc (&requests);
  soup_server_message_set_status (msg, 200, NULL);
  if (strcmp (path, "/page.html") == 0)
    soup_server_message_set_response (msg, "text/html; charset=utf-8", SOUP_MEMORY_STATIC,
                                      "<h1>Prefix sums</h1><p>pre[0] = 0</p><script>steal()</script>", 61);
  else if (strcmp (path, "/plain.txt") == 0)
    soup_server_message_set_response (msg, "text/plain", SOUP_MEMORY_STATIC, "line one\nline two", 17);
  else if (strcmp (path, "/doc.pdf") == 0)
    soup_server_message_set_response (msg, "application/pdf", SOUP_MEMORY_STATIC, "%PDF-1.7", 8);
  else if (strcmp (path, "/script-only") == 0)
    soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_STATIC, "<script>app()</script>", 22);
  else if (strcmp (path, "/redirect") == 0)
    soup_server_message_set_redirect (msg, 302, "/page.html");
  else if (strcmp (path, "/big") == 0)
    {
      GString *s = g_string_new (NULL);
      while (s->len < 5 * 1024 * 1024)
        g_string_append (s, "<p>prefix sums turn range sums into one subtraction</p>");
      gsize len = s->len;
      soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_TAKE, g_string_free (s, FALSE), len);
    }
  else
    soup_server_message_set_status (msg, 404, NULL);
}

static char *base;
static GMainContext *server_ctx;
static GMainLoop *server_loop;
static GThread *server;

/* The server runs on its own thread and main context, and is shut down
 * and freed at the end, so leak checkers see a clean exit. */
static gpointer
server_thread (gpointer data)
{
  (void) data;
  g_main_context_push_thread_default (server_ctx);
  SoupServer *srv = soup_server_new (NULL, NULL);
  soup_server_add_handler (srv, NULL, serve, NULL, NULL);
  g_assert_true (soup_server_listen_local (srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris (srv);
  char *b = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  g_atomic_pointer_set (&base, b);
  g_main_loop_run (server_loop);
  soup_server_disconnect (srv);
  g_object_unref (srv);
  while (g_main_context_iteration (server_ctx, FALSE))
    ;
  g_main_context_pop_thread_default (server_ctx);
  return NULL;
}

static void
start_server (void)
{
  server_ctx = g_main_context_new ();
  server_loop = g_main_loop_new (server_ctx, FALSE);
  server = g_thread_new ("server", server_thread, NULL);
  while (g_atomic_pointer_get (&base) == NULL)
    g_usleep (1000);
}

static gboolean
quit_server (gpointer data)
{
  (void) data;
  g_main_loop_quit (server_loop);
  return G_SOURCE_REMOVE;
}

static void
stop_server (void)
{
  g_main_context_invoke (server_ctx, quit_server, NULL);
  g_thread_join (server);
  g_main_loop_unref (server_loop);
  g_main_context_unref (server_ctx);
  g_free (base);
}

static char *
url (const char *path)
{
  /* base ends with "/" */
  return g_strconcat (base, path + 1, NULL);
}

typedef struct {
  const char *name, *path;
  gboolean    check_host, check_connection;
  const char *text, *problem; /* expected: one of them */
} FetchCase;

static const FetchCase cases[] = {
  { "a page",              "/page.html",   FALSE, FALSE, "Prefix sums\n\npre[0] = 0", NULL },
  { "plain text",          "/plain.txt",   FALSE, FALSE, "line one line two", NULL },
  { "a redirect",          "/redirect",    FALSE, FALSE, "Prefix sums\n\npre[0] = 0", NULL },
  { "not found",           "/missing",     FALSE, FALSE, NULL, "could not be fetched (404)" },
  { "a PDF",               "/doc.pdf",     FALSE, FALSE, NULL, "(application/pdf) is not read yet" },
  { "a page with no text", "/script-only", FALSE, FALSE, NULL, "has no text" },
  { "the host check",      "/page.html",   TRUE,  FALSE, NULL, "points inside this computer" },
  { "the connection check","/page.html",   FALSE, TRUE,  NULL, "led inside this computer" },
  { "both checks",         "/page.html",   TRUE,  TRUE,  NULL, "inside this computer" },
};

static void
test_fetch (void)
{
  SoupSession *session = soup_session_new_with_options ("timeout", JN_FETCH_TIMEOUT, NULL);
  JrCases c = { "fetch", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      const FetchCase *k = &cases[i];
      g_autofree char *u = url (k->path);
      int before = g_atomic_int_get (&requests);
      JnAiSource s = { 0 };
      /* A guard case gets a fresh session: a kept-alive connection made
       * earlier without the guard would be reused, not checked. */
      SoupSession *own = k->check_host || k->check_connection
                           ? soup_session_new_with_options ("timeout", JN_FETCH_TIMEOUT, NULL) : g_object_ref (session);
      jn_fetch_url (own, u, k->check_host, k->check_connection, &s);
      g_object_unref (own);
      gboolean ok = k->text != NULL ? g_strcmp0 (s.text, k->text) == 0 && s.problem == NULL
                                    : s.text == NULL && s.problem != NULL && strstr (s.problem, k->problem) != NULL;
      jr_case (&c, k->name, ok, "Expected %s  Got text \"%s\" problem \"%s\"", k->text ? k->text : k->problem, s.text,
               s.problem);
      if (k->check_host || k->check_connection)
        jr_case (&c, k->name, g_atomic_int_get (&requests) == before, "Expected no request to reach the server");
      g_free (s.url);
      g_free (s.text);
      g_free (s.problem);
    }
  /* A 5 MB page: read up to the cap, text cut to its limit. */
  g_autofree char *big = url ("/big");
  JnAiSource s = { 0 };
  jn_fetch_url (session, big, FALSE, FALSE, &s);
  jr_case (&c, "a 5 MB page", s.text != NULL && strlen (s.text) <= JN_FETCH_MAX_TEXT, "Got %zu bytes",
           s.text ? strlen (s.text) : 0);
  g_free (s.text);
  g_free (s.problem);
  jr_cases_done (&c);
  g_object_unref (session);
}

/* The list: bad links never reach the network; at most 8 are used; blank
 * lines are skipped; the helper's checks are always on. */
static void
test_sources (void)
{
  SoupSession *session = soup_session_new_with_options ("timeout", JN_FETCH_TIMEOUT, NULL);
  g_autofree char *local = url ("/page.html");
  char *links[] = { (char *) "", (char *) "  ", (char *) "file:///etc/passwd", (char *) "https://me:pw@example.com/",
                    (char *) "ftp://example.com/", local, NULL };
  int before = g_atomic_int_get (&requests);
  gsize n = 0;
  JnAiSource *s = jn_fetch_sources (session, links, FALSE, &n);
  g_assert_cmpuint (n, ==, 4);
  g_assert_cmpstr (s[0].problem, ==, "only http and https links are fetched");
  g_assert_cmpstr (s[1].problem, ==, "links with a user name or password are not fetched");
  g_assert_cmpstr (s[2].problem, ==, "only http and https links are fetched");
  g_assert_nonnull (s[3].problem); /* the local server: never fetched by the helper */
  for (gsize i = 0; i < n; i++)
    g_assert_null (s[i].text);
  g_assert_cmpint (g_atomic_int_get (&requests), ==, before);
  jn_fetch_free (s, n);

  char *many[12];
  for (int i = 0; i < 11; i++)
    many[i] = (char *) "ftp://x";
  many[11] = NULL;
  s = jn_fetch_sources (session, many, FALSE, &n);
  g_assert_cmpuint (n, ==, JN_FETCH_MAX_LINKS);
  jn_fetch_free (s, n);
  g_object_unref (session);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  start_server ();
  g_test_add_func ("/fetch/pages", test_fetch);
  g_test_add_func ("/fetch/sources", test_sources);
  int rc = g_test_run ();
  stop_server ();
  return rc;
}
