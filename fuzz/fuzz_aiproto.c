/* Fuzz target: every message Notes and its AI helper read. The first
 * byte picks which (Gemini's answer, the sign-in token, the ADC file,
 * Notes' request to the helper, the helper's answer to Notes) and, for
 * the HTTP answers, the status; the rest is the message (any bytes).
 *
 * Text that comes out is valid UTF-8, so the screens can show it. An
 * error never holds the token or the refresh token. A request or answer
 * that is read, written and read again comes back the same. */
#define FUZZ_NOTES
#include <string.h>
#include "fuzz.h"
#include "aiproto.h"

static void
check_text (const char *s)
{
  FUZZ_CHECK (s == NULL || g_utf8_validate (s, -1, NULL));
}

static void
check_error (GError *error, gboolean failed)
{
  FUZZ_CHECK ((error != NULL) == failed);
  if (error != NULL)
    {
      FUZZ_CHECK (error->message != NULL && *error->message != '\0');
      check_text (error->message);
    }
}

/* A secret worth looking for in an error: long enough to mean something. */
static void
check_secret_not_in (const char *secret, GError *error)
{
  if (secret != NULL && error != NULL && strlen (secret) >= 8)
    FUZZ_CHECK (strstr (error->message, secret) == NULL);
}

static void
gemini (const char *json, gssize len, int status)
{
  GError *error = NULL;
  g_autofree char *markdown = jn_ai_parse_response (json, len, status, &error);
  check_error (error, markdown == NULL);
  check_text (markdown);
  if (markdown != NULL)
    {
      FUZZ_CHECK (status == 200 && *markdown != '\0');
      JnAiSource s = { (char *) "https://example.org/", NULL, (char *) "could not be read" };
      g_autofree char *copy = jn_ai_enhanced_copy ("Topic", "Subject", "today", JN_AI_MODEL, &s, 1, markdown);
      check_text (copy);
    }
  g_clear_error (&error);
}

static void
token (const char *json, gssize len, int status)
{
  GError *error = NULL;
  gint64 expires = -1;
  g_autofree char *t = jn_token_parse (json, len, status, &expires, &error);
  check_error (error, t == NULL);
  check_text (t);
  if (t != NULL)
    FUZZ_CHECK (status == 200 && expires >= 0 && expires <= JN_TOKEN_MAX_SECONDS);
  g_clear_error (&error);
}

static void
adc (const char *json, gssize len)
{
  GError *error = NULL;
  JnAdc a = { 0 };
  gboolean ok = jn_adc_parse (json, len, &a, &error);
  check_error (error, !ok);
  if (ok)
    {
      FUZZ_CHECK (a.client_id != NULL && a.client_secret != NULL && a.refresh_token != NULL);
      g_autofree char *form = jn_adc_token_form (&a);
      FUZZ_CHECK (form != NULL && strchr (form, ' ') == NULL && strchr (form, '\n') == NULL);
    }
  g_clear_error (&error);
  jn_adc_clear (&a);
  /* A refresh token in a file that is not right is still never shown. */
  g_autofree char *copy = g_strndup (json, (gsize) len);
  const char *rt = strstr (copy, "\"refresh_token\":\"");
  if (rt != NULL && !ok)
    {
      g_autofree char *value = g_strndup (rt + 17, strcspn (rt + 17, "\"\\"));
      jn_adc_parse (json, len, &a, &error);
      check_secret_not_in (value, error);
      g_clear_error (&error);
      jn_adc_clear (&a);
    }
}

static gboolean
same_strv (char **a, char **b)
{
  guint n = a != NULL ? g_strv_length (a) : 0, m = b != NULL ? g_strv_length (b) : 0;
  if (n != m)
    return FALSE;
  for (guint i = 0; i < n; i++)
    if (strcmp (a[i], b[i]) != 0)
      return FALSE;
  return TRUE;
}

static void
request (const char *json, gssize len)
{
  GError *error = NULL;
  JnAiRequest r;
  gboolean ok = jn_ai_request_from_json (json, len, &r, &error);
  check_error (error, !ok);
  g_clear_error (&error);
  if (!ok)
    {
      jn_ai_request_clear (&r);
      return;
    }
  FUZZ_CHECK (r.n_ports >= 1 && r.n_ports <= JN_AI_MAX_PORTS);
  for (gsize i = 0; i < r.n_ports; i++)
    FUZZ_CHECK (r.ports[i] >= 1);
  FUZZ_CHECK (g_str_has_prefix (r.endpoint, "https://") || g_str_has_prefix (r.endpoint, "http://"));
  check_text (r.endpoint);
  check_text (r.token);
  check_text (r.input.note);
  g_autofree char *out = jn_ai_request_to_json (&r);
  JnAiRequest b;
  FUZZ_CHECK (jn_ai_request_from_json (out, -1, &b, NULL));
  FUZZ_CHECK (strcmp (r.endpoint, b.endpoint) == 0 && strcmp (r.token, b.token) == 0);
  FUZZ_CHECK (g_strcmp0 (r.project, b.project) == 0);
  FUZZ_CHECK (r.n_ports == b.n_ports && memcmp (r.ports, b.ports, sizeof r.ports) == 0);
  FUZZ_CHECK (strcmp (r.input.note, b.input.note) == 0 && strcmp (r.input.topic, b.input.topic) == 0);
  FUZZ_CHECK (strcmp (r.input.subject, b.input.subject) == 0 && strcmp (r.input.span, b.input.span) == 0);
  FUZZ_CHECK (same_strv (r.links, b.links));
  FUZZ_CHECK (same_strv ((char **) r.input.style, (char **) b.input.style));
  /* The body for Gemini can be made from any request. */
  g_autofree char *body = jn_ai_build_body (&r.input);
  check_text (body);
  jn_ai_request_clear (&r);
  jn_ai_request_clear (&b);
}

static void
answer (const char *json, gssize len)
{
  GError *error = NULL;
  JnAiAnswer a;
  gboolean ok = jn_ai_answer_from_json (json, len, &a, &error);
  check_error (error, !ok);
  g_clear_error (&error);
  if (!ok)
    {
      jn_ai_answer_clear (&a);
      return;
    }
  FUZZ_CHECK (a.markdown != NULL || a.error != NULL);
  check_text (a.markdown);
  check_text (a.error);
  for (gsize i = 0; i < a.n_sources; i++)
    {
      FUZZ_CHECK (a.sources[i].url != NULL);
      check_text (a.sources[i].url);
      check_text (a.sources[i].problem);
    }
  g_autofree char *out = jn_ai_answer_to_json (a.markdown, a.model, a.sources, a.n_sources, a.error);
  JnAiAnswer b;
  FUZZ_CHECK (jn_ai_answer_from_json (out, -1, &b, NULL));
  FUZZ_CHECK (g_strcmp0 (a.markdown, b.markdown) == 0 && g_strcmp0 (a.error, b.error) == 0);
  FUZZ_CHECK (g_strcmp0 (a.model, b.model) == 0 && a.n_sources == b.n_sources);
  for (gsize i = 0; i < a.n_sources; i++)
    FUZZ_CHECK (strcmp (a.sources[i].url, b.sources[i].url) == 0 &&
                g_strcmp0 (a.sources[i].problem, b.sources[i].problem) == 0);
  jn_ai_answer_clear (&a);
  jn_ai_answer_clear (&b);
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  if (size < 1)
    return 0;
  static const int STATUS[] = { 200, 200, 200, 400, 401, 403, 429, 500 };
  int status = STATUS[(data[0] >> 4) % G_N_ELEMENTS (STATUS)];
  const char *json = (const char *) data + 1;
  gssize len = (gssize) size - 1;
  switch (data[0] % 5)
    {
    case 0: gemini (json, len, status); break;
    case 1: token (json, len, status); break;
    case 2: adc (json, len); break;
    case 3: request (json, len); break;
    default: answer (json, len); break;
    }
  return 0;
}
