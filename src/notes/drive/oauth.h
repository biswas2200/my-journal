/* oauth: "Sign in with Google" for Notes' Drive backup, as pure
 * functions (signin.c does the browser and the network).
 *
 * Google's flow for desktop apps (OAuth 2.0 for installed apps, with
 * PKCE, RFC 7636):
 *
 *   1. Notes makes a one-time secret (the verifier) and a random state,
 *      and opens Google's sign-in page in your browser with the
 *      verifier's hash (the challenge) and a return address on this
 *      laptop, http://127.0.0.1:<port>/, where it listens for a moment.
 *   2. You sign in and allow "see, edit, create and delete only the
 *      Google Drive files you use with this app" (drive.file); Google
 *      sends the browser back with a one-time code and the same state.
 *   3. Notes trades the code and the verifier for an access token (one
 *      hour) and a refresh token (kept in your login keyring).
 *
 * The app is yours: its client id and secret come from the "Desktop
 * app" OAuth client you make in your Google Cloud project (Google says a
 * desktop client's secret is not a secret; the verifier is what protects
 * the code). */
#pragma once

#include <glib.h>

#define JN_OAUTH_AUTH_URL  "https://accounts.google.com/o/oauth2/v2/auth"
#define JN_OAUTH_TOKEN_URL "https://oauth2.googleapis.com/token"
#define JN_OAUTH_REVOKE_URL "https://oauth2.googleapis.com/revoke"
/* Only files Notes made itself: never the rest of your Drive. */
#define JN_DRIVE_SCOPE     "https://www.googleapis.com/auth/drive.file"

typedef struct {
  char *client_id, *client_secret;
} JnOAuthClient;

/* The client file Google gives you for a "Desktop app" client
 * ({"installed": {"client_id": …, "client_secret": …}}). Refuses other
 * kinds (a "web" client cannot use the return address on this laptop). */
gboolean jn_oauth_client_parse   (const char *json, gssize len, JnOAuthClient *out, GError **error);
void     jn_oauth_client_clear   (JnOAuthClient *c);

/* A fresh verifier: 64 characters from A-Z a-z 0-9 - . _ ~ . */
char    *jn_oauth_verifier_new   (void);
/* Its challenge: base64url (no padding) of its SHA-256. */
char    *jn_oauth_challenge      (const char *verifier);
/* A fresh state: 32 random hex characters. */
char    *jn_oauth_state_new      (void);

/* Google's sign-in page for this client, return address, challenge and
 * state; asks for drive.file and a refresh token. */
char    *jn_oauth_auth_url       (const JnOAuthClient *c, const char *redirect_uri, const char *challenge,
                                  const char *state);

/* The browser's return ("/?state=…&code=…" or "…&error=access_denied"):
 * the code, if the state matches; otherwise an error in plain words. */
gboolean jn_oauth_parse_return   (const char *path_and_query, const char *state, char **code, GError **error);

/* x-www-form-urlencoded bodies for JN_OAUTH_TOKEN_URL. */
char    *jn_oauth_code_form      (const JnOAuthClient *c, const char *code, const char *verifier,
                                  const char *redirect_uri);
char    *jn_oauth_refresh_form   (const JnOAuthClient *c, const char *refresh_token);

/* The token endpoint's answer (HTTP `status`): the access token, and the
 * refresh token when there is one (the first sign-in). Refuses an answer
 * without drive.file in its scope (you unticked it on Google's page). */
typedef struct {
  char  *access, *refresh;
  gint64 expires_in; /* seconds, 0 to a day */
} JnOAuthTokens;

gboolean jn_oauth_parse_tokens   (const char *json, gssize len, int status, JnOAuthTokens *out, GError **error);
void     jn_oauth_tokens_clear   (JnOAuthTokens *t);
