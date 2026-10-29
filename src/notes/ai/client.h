/* client: Notes asking for an enhanced copy, without ever handing out
 * your Google sign-in.
 *
 *   1. read your sign-in (ADC file) and trade it for a one-hour access
 *      token at Google (cached in memory, wiped when done);
 *   2. start the AI helper (journal-ai) with an empty environment, give
 *      it the token, the note, its links and the ports it may use;
 *   3. read its answer.
 * The refresh token and client secret never leave Notes; the helper sees
 * only the token. A JnEnhancer is what the window calls, so tests can
 * put a stand-in in its place. */
#pragma once

#include <gio/gio.h>
#include "aiproto.h"

/* `answer` on success (markdown set) or `error`; both owned by the caller
 * of done for the call only. */
typedef void (*JnEnhanceDone) (const JnAiAnswer *answer, const char *error, gpointer user);

typedef struct JnEnhancer JnEnhancer;
struct JnEnhancer {
  void (*start) (JnEnhancer *self, const JnAiInput *input, char **links, GCancellable *cancel, JnEnhanceDone done,
                 gpointer user);
  /* Which project and model, for the consent question ("p · gemini-3.8-flash"). */
  char *(*describe) (JnEnhancer *self);
  void (*free) (JnEnhancer *self);
};

typedef struct {
  const char *helper;    /* journal-ai */
  const char *adc_file;  /* NULL: $GOOGLE_APPLICATION_CREDENTIALS or ~/.config/gcloud/... */
  const char *token_url; /* NULL: JN_TOKEN_URL */
  const char *api_base;  /* NULL: https://aiplatform.googleapis.com/ */
  const char *project;   /* NULL: $GOOGLE_CLOUD_PROJECT, else the sign-in's quota project */
  guint16     ports[JN_AI_MAX_PORTS]; /* none given: 443 and 80 */
  gsize       n_ports;
} JnGeminiConfig;

JnEnhancer *jn_gemini_enhancer_new (const JnGeminiConfig *config);
