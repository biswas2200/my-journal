/* client: see client.h. */
#include "client.h"

#include <string.h>
#include <libsoup/soup.h>

typedef struct {
  JnEnhancer   base;
  char        *helper, *adc_file, *token_url, *api_base, *project;
  guint16      ports[JN_AI_MAX_PORTS];
  gsize        n_ports;
  SoupSession *session;
  char        *token;        /* cached access token */
  gint64       token_until;  /* monotonic seconds */
} Gemini;

typedef struct {
  Gemini        *g;
  JnAiRequest    req;          /* strings owned here */
  GCancellable  *cancel;
  JnEnhanceDone  done;
  gpointer       user;
  GSubprocess   *proc;
} Job;

static void
job_finish (Job *job, const JnAiAnswer *answer, const char *error)
{
  job->done (answer, error, job->user);
  g_free (job->req.endpoint);
  g_free (job->req.project);
  if (job->req.token != NULL)
    memset (job->req.token, 0, strlen (job->req.token));
  g_free (job->req.token);
  g_strfreev (job->req.links);
  g_free ((char *) job->req.input.subject);
  g_free ((char *) job->req.input.topic);
  g_free ((char *) job->req.input.span);
  g_free ((char *) job->req.input.note);
  g_strfreev ((char **) job->req.input.style);
  g_clear_object (&job->cancel);
  g_clear_object (&job->proc);
  g_free (job);
}

static void
on_helper_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *job = data;
  g_autofree char *out = NULL;
  g_autoptr (GError) error = NULL;
  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, &error))
    {
      job_finish (job, NULL, g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? "Stopped."
                                                                                       : "The AI helper did not answer.");
      return;
    }
  JnAiAnswer a = { 0 };
  if (!jn_ai_answer_from_json (out, -1, &a, NULL))
    job_finish (job, NULL, "The AI helper stopped without an answer.");
  else if (a.markdown == NULL)
    job_finish (job, &a, a.error != NULL ? a.error : "The AI gave no answer.");
  else
    job_finish (job, &a, NULL);
  jn_ai_answer_clear (&a);
}

static void
run_helper (Job *job)
{
  GSubprocessLauncher *l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                      G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  /* Nothing of Notes' environment: only language and time zone. */
  const char *keep[] = { "LANG", "LC_ALL", "LC_MESSAGES", "TZ" };
  char *env[G_N_ELEMENTS (keep) + 1] = { NULL };
  gsize n = 0;
  for (gsize i = 0; i < G_N_ELEMENTS (keep); i++)
    if (g_getenv (keep[i]) != NULL)
      env[n++] = g_strdup_printf ("%s=%s", keep[i], g_getenv (keep[i]));
  g_subprocess_launcher_set_environ (l, env);
  for (gsize i = 0; i < n; i++)
    g_free (env[i]);
  g_autoptr (GError) error = NULL;
  job->proc = g_subprocess_launcher_spawn (l, &error, job->g->helper, NULL);
  g_object_unref (l);
  if (job->proc == NULL)
    {
      job_finish (job, NULL, "The AI helper is missing (reinstall Journal).");
      return;
    }
  g_autofree char *json = jn_ai_request_to_json (&job->req);
  g_subprocess_communicate_utf8_async (job->proc, json, job->cancel, on_helper_done, job);
  memset (json, 0, strlen (json)); /* it held the token */
}

static void
on_token (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *job = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  if (body == NULL)
    {
      g_autofree char *m = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                             ? g_strdup ("Stopped.")
                             : g_strdup_printf ("Google's sign-in service could not be reached (%s).", error->message);
      job_finish (job, NULL, m);
      return;
    }
  gsize len = 0;
  const char *data_ = g_bytes_get_data (body, &len);
  gint64 expires = 0;
  char *token = jn_token_parse (data_, (gssize) len, (int) soup_message_get_status (msg), &expires, &error);
  if (token == NULL)
    {
      job_finish (job, NULL, error->message);
      return;
    }
  Gemini *g = job->g;
  if (g->token != NULL)
    memset (g->token, 0, strlen (g->token));
  g_free (g->token);
  g->token = token;
  g->token_until = g_get_monotonic_time () / G_USEC_PER_SEC + MAX (expires - 120, 0); /* renew 2 min early */
  job->req.token = g_strdup (token);
  run_helper (job);
}

static void
gemini_start (JnEnhancer *self, const JnAiInput *in, char **links, GCancellable *cancel, JnEnhanceDone done,
              gpointer user)
{
  Gemini *g = (Gemini *) self;
  Job *job = g_new0 (Job, 1);
  job->g = g;
  job->done = done;
  job->user = user;
  job->cancel = cancel != NULL ? g_object_ref (cancel) : g_cancellable_new ();
  job->req.input.subject = g_strdup (in->subject);
  job->req.input.topic = g_strdup (in->topic);
  job->req.input.span = g_strdup (in->span);
  job->req.input.note = g_strdup (in->note);
  job->req.input.style = (const char *const *) g_strdupv ((char **) in->style);
  job->req.links = g_strdupv (links);
  memcpy (job->req.ports, g->ports, sizeof g->ports);
  job->req.n_ports = g->n_ports;

  /* The sign-in: read fresh each time (you may have signed in again). */
  g_autofree char *text = NULL;
  gsize len = 0;
  if (!g_file_get_contents (g->adc_file, &text, &len, NULL))
    {
      job_finish (job, NULL, "You are not signed in to Google on this laptop. Sign in with: "
                             "gcloud auth application-default login");
      return;
    }
  JnAdc adc = { 0 };
  g_autoptr (GError) error = NULL;
  gboolean read = jn_adc_parse (text, (gssize) len, &adc, &error);
  memset (text, 0, len);
  if (!read)
    {
      job_finish (job, NULL, error->message);
      return;
    }
  const char *project = g->project != NULL ? g->project : adc.project;
  if (project == NULL)
    {
      jn_adc_clear (&adc);
      job_finish (job, NULL, "No Google Cloud project is set. Set one with: gcloud auth application-default "
                             "set-quota-project PROJECT");
      return;
    }
  job->req.project = g_strdup (project);
  job->req.endpoint = g_strdup_printf ("%sv1/projects/%s/locations/global/publishers/google/models/%s:generateContent",
                                       g->api_base, project, JN_AI_MODEL);
  if (g->token != NULL && g_get_monotonic_time () / G_USEC_PER_SEC < g->token_until)
    {
      jn_adc_clear (&adc);
      job->req.token = g_strdup (g->token);
      run_helper (job);
      return;
    }
  g_autofree char *form = jn_adc_token_form (&adc);
  jn_adc_clear (&adc);
  SoupMessage *msg = soup_message_new ("POST", g->token_url);
  g_autoptr (GBytes) body = g_bytes_new (form, strlen (form));
  memset (form, 0, strlen (form));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body);
  soup_session_send_and_read_async (g->session, msg, G_PRIORITY_DEFAULT, job->cancel, on_token, job);
  g_object_unref (msg);
}

static char *
gemini_describe (JnEnhancer *self)
{
  Gemini *g = (Gemini *) self;
  g_autofree char *text = NULL;
  JnAdc adc = { 0 };
  const char *project = g->project;
  if (project == NULL && g_file_get_contents (g->adc_file, &text, NULL, NULL) && jn_adc_parse (text, -1, &adc, NULL))
    project = adc.project;
  char *d = g_strdup_printf ("%s on Vertex AI, project %s", JN_AI_MODEL, project != NULL ? project : "(none set)");
  if (text != NULL)
    memset (text, 0, strlen (text));
  jn_adc_clear (&adc);
  return d;
}

static void
gemini_free (JnEnhancer *self)
{
  Gemini *g = (Gemini *) self;
  if (g->token != NULL)
    memset (g->token, 0, strlen (g->token));
  g_free (g->token);
  g_free (g->helper);
  g_free (g->adc_file);
  g_free (g->token_url);
  g_free (g->api_base);
  g_free (g->project);
  g_clear_object (&g->session);
  g_free (g);
}

JnEnhancer *
jn_gemini_enhancer_new (const JnGeminiConfig *c)
{
  Gemini *g = g_new0 (Gemini, 1);
  g->base.start = gemini_start;
  g->base.describe = gemini_describe;
  g->base.free = gemini_free;
  g->helper = g_strdup (c->helper);
  const char *env_adc = g_getenv ("GOOGLE_APPLICATION_CREDENTIALS");
  g->adc_file = c->adc_file != NULL ? g_strdup (c->adc_file)
              : env_adc != NULL && *env_adc != '\0' ? g_strdup (env_adc)
              : g_build_filename (g_get_user_config_dir (), "gcloud", "application_default_credentials.json", NULL);
  g->token_url = g_strdup (c->token_url != NULL ? c->token_url : JN_TOKEN_URL);
  g->api_base = g_strdup (c->api_base != NULL ? c->api_base : "https://aiplatform.googleapis.com/");
  const char *env_project = g_getenv ("GOOGLE_CLOUD_PROJECT");
  g->project = g_strdup (c->project != NULL ? c->project : env_project != NULL && *env_project != '\0' ? env_project : NULL);
  if (c->n_ports > 0)
    {
      g->n_ports = MIN (c->n_ports, (gsize) JN_AI_MAX_PORTS);
      memcpy (g->ports, c->ports, g->n_ports * sizeof (guint16));
    }
  else
    {
      g->ports[0] = 443;
      g->ports[1] = 80;
      g->n_ports = 2;
    }
  g->session = soup_session_new_with_options ("timeout", 30, NULL);
  return (JnEnhancer *) g;
}
