/* aiproto: everything Notes and its AI helper say, as pure functions.
 *
 *   your Google sign-in (ADC)  ->  a token request  ->  a 1-hour token
 *   note + fetched sources     ->  the request to Gemini
 *   Gemini's answer            ->  Markdown, or a plain-words error
 *   Notes  --request-->  journal-ai  --answer-->  Notes   (JSON on pipes)
 *
 * Only the short-lived access token ever goes to the helper; the refresh
 * token and client secret stay in Notes, and no error message ever holds
 * a secret. Page text stays in the helper: the answer names each source
 * and whether it was used, nothing more. */
#pragma once

#include <glib.h>

#define JN_TOKEN_URL "https://oauth2.googleapis.com/token"
#define JN_AI_MODEL  "gemini-3.8-flash"
#define JN_AI_MAX_PORTS 8

/* ---- sign-in ---------------------------------------------------------- */

typedef struct {
  char *client_id, *client_secret, *refresh_token;
  char *project; /* quota_project_id; NULL if the file names none */
} JnAdc;

/* Reads ~/.config/gcloud/application_default_credentials.json
 * ("authorized_user", from gcloud auth application-default login). */
gboolean jn_adc_parse      (const char *json, gssize len, JnAdc *out, GError **error);
/* Wipes and frees the secrets. */
void     jn_adc_clear      (JnAdc *adc);
/* The x-www-form-urlencoded body for JN_TOKEN_URL. */
char    *jn_adc_token_form (const JnAdc *adc);
#define JN_TOKEN_MAX_SECONDS (24 * 60 * 60)
/* The access token from the token endpoint's answer (HTTP `status`);
 * *expires_in is its lifetime in seconds, 0 to JN_TOKEN_MAX_SECONDS. */
char    *jn_token_parse    (const char *json, gssize len, int status, gint64 *expires_in, GError **error);

/* ---- what goes to Gemini ------------------------------------------------ */

typedef struct {
  char *url;
  char *text;    /* the page as text, or NULL */
  char *problem; /* why it was not used, or NULL */
} JnAiSource;

typedef struct {
  const char        *subject, *topic, *span;
  const char        *note;      /* stored lines (noteline.h) */
  JnAiSource        *sources;
  gsize              n_sources;
  const char *const *style;     /* other notes of this person, NULL-terminated, or NULL */
} JnAiInput;

/* The generateContent request body (JSON). */
char *jn_ai_build_body     (const JnAiInput *in);
/* The Markdown Gemini wrote, from its answer (HTTP `status`). */
char *jn_ai_parse_response (const char *json, gssize len, int status, GError **error);
/* The enhanced copy as stored and shown: a header the app writes (title,
 * when, which model, the numbered sources and which were not used), then
 * the AI's Markdown without a title it may have repeated. */
char *jn_ai_enhanced_copy  (const char *topic, const char *subject, const char *span, const char *model,
                            const JnAiSource *sources, gsize n_sources, const char *markdown);

/* ---- Notes <-> helper ---------------------------------------------------- */

typedef struct {
  char      *endpoint, *project, *token;
  guint16    ports[JN_AI_MAX_PORTS]; /* the only TCP ports the helper may connect to */
  gsize      n_ports;
  char     **links;                  /* the Sources lines, as typed */
  JnAiInput  input;                  /* sources are filled in by the helper */
} JnAiRequest;

char    *jn_ai_request_to_json   (const JnAiRequest *r);
/* Strings in `out` are newly allocated: free them with jn_ai_request_clear. */
gboolean jn_ai_request_from_json (const char *json, gssize len, JnAiRequest *out, GError **error);
void     jn_ai_request_clear     (JnAiRequest *r);

typedef struct {
  char       *markdown, *model; /* on success */
  char       *error;            /* on failure */
  JnAiSource *sources;          /* url and problem only */
  gsize       n_sources;
} JnAiAnswer;

char    *jn_ai_answer_to_json    (const char *markdown, const char *model, const JnAiSource *sources,
                                  gsize n_sources, const char *error);
gboolean jn_ai_answer_from_json  (const char *json, gssize len, JnAiAnswer *out, GError **error);
void     jn_ai_answer_clear      (JnAiAnswer *a);
