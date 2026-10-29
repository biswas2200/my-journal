/* Notes' AI client (src/notes/ai/client.c) against fake Google services
 * on this machine (token endpoint and Gemini), with the real helper.
 *
 * Checked: a token is fetched once and reused; the refresh token and
 * client secret go only to the token endpoint, never to Gemini or the
 * helper's side; a revoked sign-in, a missing sign-in, a missing project
 * or a missing helper each give one plain sentence. */
#include <string.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include "cases.h"
#include "client.h"

#define REFRESH "1//0refresh-secret"
static char *base;
static GMutex lock;
static int token_hits, gemini_hits;
static gboolean revoked_signin;
static GString *gemini_seen; /* every header and body Gemini got */

static void
serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user)
{
  (void) server; (void) query; (void) user;
  g_mutex_lock (&lock);
  SoupMessageBody *b = soup_server_message_get_request_body (msg);
  soup_server_message_set_status (msg, 200, NULL);
  if (strcmp (path, "/token") == 0)
    {
      token_hits++;
      const char *ok = "{\"access_token\":\"ya29.fake\",\"expires_in\":3599}";
      const char *no = "{\"error\":\"invalid_grant\"}";
      if (revoked_signin)
        soup_server_message_set_status (msg, 400, NULL);
      gboolean has_refresh = b->data != NULL && strstr (b->data, "refresh_token=1%2F%2F0refresh-secret") != NULL;
      const char *r = revoked_signin ? no : has_refresh ? ok : "{\"error\":\"invalid_request\"}";
      soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, r, strlen (r));
    }
  else
    {
      gemini_hits++;
      SoupMessageHeadersIter it;
      const char *name, *value;
      soup_message_headers_iter_init (&it, soup_server_message_get_request_headers (msg));
      while (soup_message_headers_iter_next (&it, &name, &value))
        g_string_append_printf (gemini_seen, "%s: %s\n", name, value);
      g_string_append (gemini_seen, b->data != NULL ? b->data : "");
      const char *r = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"## Idea\\n- more [1]\"}]}}]}";
      soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, r, strlen (r));
    }
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

typedef struct {
  gboolean finished;
  char    *markdown, *error;
} Result;

static void
on_done (const JnAiAnswer *a, const char *error, gpointer user)
{
  Result *r = user;
  r->finished = TRUE;
  r->markdown = g_strdup (a != NULL ? a->markdown : NULL);
  r->error = g_strdup (error);
}

static Result
enhance (JnEnhancer *e)
{
  Result r = { 0 };
  JnAiInput in = { "DSA", "Prefix sum", "now", "- pre[0] = 0", NULL, 0, NULL };
  char *links[] = { NULL };
  e->start (e, &in, links, NULL, on_done, &r);
  while (!r.finished)
    g_main_context_iteration (NULL, TRUE);
  return r;
}

static char *adc_path;

static JnEnhancer *
client (const char *helper, const char *adc, const char *project)
{
  g_autofree char *token_url = g_strconcat (base, "token", NULL);
  g_autoptr (GUri) u = g_uri_parse (base, G_URI_FLAGS_NONE, NULL);
  JnGeminiConfig c = { helper, adc, token_url, base, project, { (guint16) g_uri_get_port (u) }, 1 };
  return jn_gemini_enhancer_new (&c);
}

static void
write_adc (const char *json)
{
  g_assert_true (g_file_set_contents (adc_path, json, -1, NULL));
}

static void
test_client (gconstpointer helper)
{
  JrCases c = { "client", 0, 0 };
  write_adc ("{\"type\":\"authorized_user\",\"client_id\":\"id\",\"client_secret\":\"shh\",\"refresh_token\":\""
             REFRESH "\",\"quota_project_id\":\"proj\"}");
  JnEnhancer *e = client (helper, adc_path, NULL);
  g_autofree char *about = e->describe (e);
  jr_case (&c, "describe", strstr (about, "gemini-3.8-flash") != NULL && strstr (about, "proj") != NULL, "Got %s", about);

  Result r = enhance (e);
  jr_case (&c, "enhanced", g_strcmp0 (r.markdown, "## Idea\n- more [1]") == 0 && r.error == NULL, "Got %s / %s",
           r.markdown, r.error);
  g_free (r.markdown);
  g_free (r.error);
  r = enhance (e);
  g_mutex_lock (&lock);
  jr_case (&c, "token reused", token_hits == 1 && gemini_hits == 2, "Token %d, Gemini %d", token_hits, gemini_hits);
  jr_case (&c, "only the token reached Gemini", strstr (gemini_seen->str, "Bearer ya29.fake") != NULL &&
           strstr (gemini_seen->str, "refresh") == NULL && strstr (gemini_seen->str, "shh") == NULL &&
           strstr (gemini_seen->str, "x-goog-user-project: proj") != NULL, "Gemini saw: %.400s", gemini_seen->str);
  g_mutex_unlock (&lock);
  g_free (r.markdown);
  g_free (r.error);
  e->free (e);

  /* Revoked sign-in: one sentence, no secret in it. */
  revoked_signin = TRUE;
  e = client (helper, adc_path, NULL);
  r = enhance (e);
  jr_case (&c, "revoked", r.error != NULL && strstr (r.error, "gcloud auth application-default login") != NULL &&
           strstr (r.error, REFRESH) == NULL, "Got %s", r.error);
  g_free (r.error);
  e->free (e);
  revoked_signin = FALSE;

  /* Not signed in at all. */
  g_autofree char *missing = g_strconcat (adc_path, ".missing", NULL);
  e = client (helper, missing, NULL);
  r = enhance (e);
  jr_case (&c, "not signed in", r.error != NULL && strstr (r.error, "not signed in") != NULL, "Got %s", r.error);
  g_free (r.error);
  e->free (e);

  /* No project anywhere. */
  write_adc ("{\"type\":\"authorized_user\",\"client_id\":\"id\",\"client_secret\":\"shh\",\"refresh_token\":\"r\"}");
  g_unsetenv ("GOOGLE_CLOUD_PROJECT");
  e = client (helper, adc_path, NULL);
  r = enhance (e);
  jr_case (&c, "no project", r.error != NULL && strstr (r.error, "set-quota-project") != NULL, "Got %s", r.error);
  g_free (r.error);
  e->free (e);

  /* The helper is not installed. */
  write_adc ("{\"type\":\"authorized_user\",\"client_id\":\"id\",\"client_secret\":\"shh\",\"refresh_token\":\""
             REFRESH "\",\"quota_project_id\":\"proj\"}");
  e = client ("/nonexistent/journal-ai", adc_path, NULL);
  r = enhance (e);
  jr_case (&c, "helper missing", r.error != NULL && strstr (r.error, "missing") != NULL, "Got %s", r.error);
  g_free (r.error);
  e->free (e);
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >, 1);
  gemini_seen = g_string_new (NULL);
  g_autofree char *dir = g_dir_make_tmp ("ai-client-XXXXXX", NULL);
  adc_path = g_build_filename (dir, "adc.json", NULL);
  g_thread_new ("fake-google", server_thread, g_main_context_new ());
  while (g_atomic_pointer_get (&base) == NULL)
    g_usleep (1000);
  g_test_add_data_func ("/ai-client/flow", argv[1], test_client);
  int rc = g_test_run ();
  g_remove (adc_path);
  g_rmdir (dir);
  return rc;
}
