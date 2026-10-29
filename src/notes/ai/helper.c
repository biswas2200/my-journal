/* journal-ai: Notes' AI helper. One run per "Enhance".
 *
 * Reads one request (aiproto.h) on stdin, locks itself (notesjail.h:
 * no files at all, TCP only to the ports Notes allows, no listening),
 * fetches the note's Sources (fetch.h), asks Gemini, and writes one
 * answer on stdout. It holds a one-hour access token, never the
 * sign-in itself; it cannot read the journal, the notes or anything
 * else of yours, and whatever a page or the model says, it can only
 * end up as text in the answer. Without its lock it does nothing. */
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <libsoup/soup.h>
#include "aiproto.h"
#include "fetch.h"
#include "notesjail.h"

#define MAX_REQUEST (4 * 1024 * 1024)
#define AI_TIMEOUT 180

static int
answer (const char *markdown, const char *model, const JnAiSource *sources, gsize n, const char *error)
{
  g_autofree char *json = jn_ai_answer_to_json (markdown, model, sources, n, error);
  fputs (json, stdout);
  fflush (stdout);
  return error != NULL ? 1 : 0;
}

static char *
read_request (gsize *len)
{
  GString *s = g_string_new (NULL);
  char buf[65536];
  size_t n;
  while ((n = fread (buf, 1, sizeof buf, stdin)) > 0 && s->len < MAX_REQUEST)
    g_string_append_len (s, buf, (gssize) n);
  *len = s->len;
  return g_string_free (s, FALSE);
}

/* "…/models/gemini-3.8-flash:generateContent" -> "gemini-3.8-flash". */
static char *
model_of (const char *endpoint)
{
  const char *m = strstr (endpoint, "/models/");
  if (m == NULL)
    return g_strdup (JN_AI_MODEL);
  m += strlen ("/models/");
  const char *colon = strchr (m, ':');
  return colon != NULL ? g_strndup (m, (gsize) (colon - m)) : g_strdup (m);
}

int
main (void)
{
  /* A crash must not write the token to disk, and nothing may read it. */
  struct rlimit none = { 0, 0 };
  setrlimit (RLIMIT_CORE, &none);
  prctl (PR_SET_DUMPABLE, 0, 0, 0, 0);
  /* No desktop services: no settings store (GIO would ask it for proxy
   * settings and try to write to it), no D-Bus, no virtual file systems.
   * The helper talks to the sources and Gemini, directly. */
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  g_setenv ("GIO_USE_VOLUME_MONITOR", "unix", TRUE);
  g_unsetenv ("DBUS_SESSION_BUS_ADDRESS");

  gsize len = 0;
  g_autofree char *raw = read_request (&len);
  JnAiRequest req = { 0 };
  g_autoptr (GError) error = NULL;
  if (len >= MAX_REQUEST || !jn_ai_request_from_json (raw, (gssize) len, &req, &error))
    return answer (NULL, NULL, NULL, 0, "the request to the AI helper was not valid");
  memset (raw, 0, len); /* it held the token */

  JnJailPaths *paths = jn_jail_helper_paths (req.ports, req.n_ports);
  gboolean locked = jn_jail_enter (paths, &error);
  jn_jail_paths_free (paths);
  if (!locked)
    {
      g_autofree char *msg = g_strdup_printf ("the AI helper could not lock itself (%s), so it did nothing",
                                              error->message);
      jn_ai_request_clear (&req);
      return answer (NULL, NULL, NULL, 0, msg);
    }

  SoupSession *session = soup_session_new_with_options ("timeout", JN_FETCH_TIMEOUT, "user-agent",
                                                        "Journal-Notes/1.0 (study notes)", "proxy-resolver", NULL,
                                                        NULL);
  gsize n = 0;
  JnAiSource *sources = jn_fetch_sources (session, req.links, FALSE, &n);
  req.input.sources = sources;
  req.input.n_sources = n;

  g_autofree char *body = jn_ai_build_body (&req.input);
  g_autofree char *model = model_of (req.endpoint);
  SoupMessage *msg = soup_message_new ("POST", req.endpoint);
  int rc;
  if (msg == NULL)
    rc = answer (NULL, NULL, sources, n, "the AI address is not valid");
  else
    {
      SoupMessageHeaders *h = soup_message_get_request_headers (msg);
      g_autofree char *auth = g_strconcat ("Bearer ", req.token, NULL);
      soup_message_headers_replace (h, "Authorization", auth);
      if (req.project != NULL)
        soup_message_headers_replace (h, "x-goog-user-project", req.project);
      g_autoptr (GBytes) out = g_bytes_new (body, strlen (body));
      soup_message_set_request_body_from_bytes (msg, "application/json", out);
      soup_session_set_timeout (session, AI_TIMEOUT);
      g_autoptr (GError) net = NULL;
      g_autoptr (GBytes) reply = soup_session_send_and_read (session, msg, NULL, &net);
      memset (auth, 0, strlen (auth));
      if (reply == NULL)
        {
          g_autofree char *m = g_strdup_printf ("Gemini could not be reached (%s)", net->message);
          rc = answer (NULL, NULL, sources, n, m);
        }
      else
        {
          gsize rlen = 0;
          const char *rdata = g_bytes_get_data (reply, &rlen);
          g_autoptr (GError) perr = NULL;
          g_autofree char *md = jn_ai_parse_response (rdata, (gssize) rlen, (int) soup_message_get_status (msg), &perr);
          rc = md != NULL ? answer (md, model, sources, n, NULL) : answer (NULL, NULL, sources, n, perr->message);
        }
      g_object_unref (msg);
    }
  /* The helper owns the fetched pages: they go with it. */
  req.input.sources = NULL;
  req.input.n_sources = 0;
  jn_fetch_free (sources, n);
  jn_ai_request_clear (&req);
  g_object_unref (session);
  return rc;
}
