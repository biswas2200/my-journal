/* The real AI helper (journal-ai) against a fake Gemini on this machine.
 *
 * The fake records what it is sent and answers like Gemini. Checked:
 * the token and project arrive as headers; the note, the rules and what
 * happened to each source are in the request; the answer comes back as
 * Markdown; a port Notes did not allow cannot be reached at all; a
 * broken request or a Gemini error gives a plain error, never a crash. */
#include <string.h>
#include <libsoup/soup.h>
#include "aiproto.h"
#include "cases.h"

static const char *helper;
static char *base;
static GMutex lock;
static GString *last_body;
static char *last_auth, *last_project;
static int hits;
static const char *reply = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"## Idea\\n- pre[0] = 0 [1]\"}]},"
                           "\"finishReason\":\"STOP\"}]}";
static guint reply_status = 200;

static void
serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user)
{
  (void) server; (void) query; (void) user; (void) path;
  g_mutex_lock (&lock);
  hits++;
  SoupMessageBody *b = soup_server_message_get_request_body (msg);
  g_string_assign (last_body, b->data != NULL ? b->data : "");
  SoupMessageHeaders *h = soup_server_message_get_request_headers (msg);
  g_free (last_auth);
  g_free (last_project);
  last_auth = g_strdup (soup_message_headers_get_one (h, "Authorization"));
  last_project = g_strdup (soup_message_headers_get_one (h, "x-goog-user-project"));
  soup_server_message_set_status (msg, reply_status, NULL);
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, reply, strlen (reply));
  g_mutex_unlock (&lock);
}

static gpointer
server_thread (gpointer data)
{
  GMainContext *ctx = data;
  g_main_context_push_thread_default (ctx);
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, serve, NULL, NULL);
  g_assert_true (soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris (server);
  char *b = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  g_atomic_pointer_set (&base, b);
  g_main_loop_run (g_main_loop_new (ctx, FALSE));
  return NULL;
}

static int
port (void)
{
  g_autoptr (GUri) u = g_uri_parse (base, G_URI_FLAGS_NONE, NULL);
  return g_uri_get_port (u);
}

/* Runs the helper with `request` on stdin; returns its stdout. */
static char *
run (const char *request, int *status)
{
  const char *argv[] = { helper, NULL };
  GSubprocess *p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL);
  g_assert_nonnull (p);
  char *out = NULL;
  g_assert_true (g_subprocess_communicate_utf8 (p, request, NULL, &out, NULL, NULL));
  *status = g_subprocess_get_exit_status (p);
  g_object_unref (p);
  return out;
}

static char *
request (const char *const *links, int allowed_port)
{
  JnAiRequest r = { 0 };
  r.endpoint = g_strdup_printf ("%sv1/projects/p/locations/global/publishers/google/models/gemini-3.8-flash:generateContent",
                                base);
  r.project = g_strdup ("p");
  r.token = g_strdup ("ya29.test-token");
  r.ports[0] = (guint16) allowed_port;
  r.n_ports = 1;
  r.links = (char **) links;
  r.input.subject = "DSA";
  r.input.topic = "Prefix sum";
  r.input.span = "28 Oct 2026, 14:05–15:40";
  r.input.note = "# Prefix sum\n- pre[0] = 0";
  static const char *style[] = { "- stack: last in, first out", NULL };
  r.input.style = style;
  char *json = jn_ai_request_to_json (&r);
  g_free (r.endpoint);
  g_free (r.project);
  g_free (r.token);
  return json;
}

static void
test_round_trip (void)
{
  const char *links[] = { "file:///home/me/.local/share/journal/journal.db", "http://127.0.0.1:1/x", NULL };
  g_autofree char *req = request (links, port ());
  int status = -1;
  g_autofree char *out = run (req, &status);
  JnAiAnswer a = { 0 };
  g_autoptr (GError) error = NULL;
  g_assert_true (jn_ai_answer_from_json (out, -1, &a, &error));
  JrCases c = { "helper", 0, 0 };
  jr_case (&c, "answer", g_strcmp0 (a.markdown, "## Idea\n- pre[0] = 0 [1]") == 0 && a.error == NULL && status == 0,
           "Got markdown \"%s\" error \"%s\" status %d", a.markdown, a.error, status);
  jr_case (&c, "model", g_strcmp0 (a.model, "gemini-3.8-flash") == 0, "Got %s", a.model);
  jr_case (&c, "file link refused", a.n_sources == 2 && a.sources[0].problem != NULL &&
           strstr (a.sources[0].problem, "only http and https") != NULL, "Got %s",
           a.n_sources > 0 ? a.sources[0].problem : "no sources");
  jr_case (&c, "odd port refused", a.n_sources == 2 && a.sources[1].problem != NULL, "Expected a problem");
  g_mutex_lock (&lock);
  jr_case (&c, "token as a header", g_strcmp0 (last_auth, "Bearer ya29.test-token") == 0, "Got %s", last_auth);
  jr_case (&c, "project as a header", g_strcmp0 (last_project, "p") == 0, "Got %s", last_project);
  jr_case (&c, "the note went", strstr (last_body->str, "<<<NOTE\\n# Prefix sum\\n- pre[0] = 0\\nNOTE>>>") != NULL,
           "Body: %.300s", last_body->str);
  jr_case (&c, "the rules went", strstr (last_body->str, "ONLY the source texts") != NULL, "Expected the rules");
  jr_case (&c, "sources not used are named", strstr (last_body->str, "SOURCE [1] file:") != NULL &&
           strstr (last_body->str, "not available") != NULL, "Body: %.300s", last_body->str);
  jr_case (&c, "style went", strstr (last_body->str, "last in, first out") != NULL, "Expected the style sample");
  jr_case (&c, "no journal text", strstr (last_body->str, "journal.db") == NULL ||
           strstr (last_body->str, "SOURCE [1] file:") != NULL, "Expected only the link's name");
  g_mutex_unlock (&lock);
  jr_cases_done (&c);
  jn_ai_answer_clear (&a);
}

/* Notes allowed port 443 only: the fake on its own port is unreachable,
 * so nothing reaches it. */
static void
test_port_not_allowed (void)
{
  g_mutex_lock (&lock);
  int before = hits;
  g_mutex_unlock (&lock);
  const char *links[] = { NULL };
  g_autofree char *req = request (links, 443);
  int status = -1;
  g_autofree char *out = run (req, &status);
  JnAiAnswer a = { 0 };
  g_assert_true (jn_ai_answer_from_json (out, -1, &a, NULL));
  g_assert_null (a.markdown);
  g_assert_nonnull (a.error);
  g_assert_nonnull (strstr (a.error, "could not be reached"));
  g_assert_cmpint (status, !=, 0);
  g_mutex_lock (&lock);
  g_assert_cmpint (hits, ==, before);
  g_mutex_unlock (&lock);
  jn_ai_answer_clear (&a);
}

static void
test_errors (void)
{
  int status = -1;
  g_autofree char *out = run ("not json at all", &status);
  JnAiAnswer a = { 0 };
  g_assert_true (jn_ai_answer_from_json (out, -1, &a, NULL));
  g_assert_nonnull (strstr (a.error, "not valid"));
  g_assert_cmpint (status, !=, 0);
  jn_ai_answer_clear (&a);

  /* Gemini says no: its message comes through, plainly. */
  reply = "{\"error\":{\"code\":403,\"message\":\"Permission denied on resource project p.\"}}";
  reply_status = 403;
  const char *links[] = { NULL };
  g_autofree char *req = request (links, port ());
  g_autofree char *out2 = run (req, &status);
  g_assert_true (jn_ai_answer_from_json (out2, -1, &a, NULL));
  g_assert_nonnull (strstr (a.error, "Permission denied on resource project p."));
  g_assert_null (strstr (out2, "ya29.")); /* the token never comes back */
  jn_ai_answer_clear (&a);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  helper = argc > 1 ? argv[1] : NULL;
  g_assert_nonnull (helper);
  last_body = g_string_new (NULL);
  g_thread_new ("fake-gemini", server_thread, g_main_context_new ());
  while (g_atomic_pointer_get (&base) == NULL)
    g_usleep (1000);
  g_test_add_func ("/ai-helper/round-trip", test_round_trip);
  g_test_add_func ("/ai-helper/port-not-allowed", test_port_not_allowed);
  g_test_add_func ("/ai-helper/errors", test_errors);
  return g_test_run ();
}
