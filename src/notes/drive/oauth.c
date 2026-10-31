/* oauth: see oauth.h. */
#include "oauth.h"

#include <string.h>
#include <sys/random.h>
#include <gio/gio.h>
#include "jsonsafe.h"

#define SIGN_IN_AGAIN "Sign in to Google again in Notes."
#define MAX_LIFETIME (24 * 60 * 60)

static void
wipe_free (char **s)
{
  if (*s != NULL)
    memset (*s, 0, strlen (*s));
  g_clear_pointer (s, g_free);
}

/* ---- the client file -------------------------------------------------------- */

gboolean
jn_oauth_client_parse (const char *json, gssize len, JnOAuthClient *out, GError **error)
{
  *out = (JnOAuthClient){ 0 };
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  JsonObject *installed = jn_json_obj (o, "installed");
  const char *id = jn_json_str (installed, "client_id"), *secret = jn_json_str (installed, "client_secret");
  gboolean ok = id != NULL && secret != NULL;
  if (ok)
    {
      out->client_id = g_strdup (id);
      out->client_secret = g_strdup (secret);
    }
  else if (jn_json_obj (o, "web") != NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "This is a \"Web application\" client. Notes needs one of type \"Desktop app\" (Google Cloud "
                         "Console, APIs & Services, Credentials, Create OAuth client ID).");
  else
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "That is not a Google client file. Download the JSON of your \"Desktop app\" OAuth client "
                         "from Google Cloud Console (APIs & Services, Credentials).");
  g_object_unref (p);
  return ok;
}

void
jn_oauth_client_clear (JnOAuthClient *c)
{
  g_clear_pointer (&c->client_id, g_free);
  wipe_free (&c->client_secret);
}

/* ---- PKCE and state ------------------------------------------------------------- */

/* Random bytes from the kernel's generator, never a predictable one. */
static void
random_bytes (guint8 *buf, gsize n)
{
  gsize got = 0;
  while (got < n)
    {
      ssize_t r = getrandom (buf + got, n - got, 0);
      if (r > 0)
        got += (gsize) r;
    }
}

char *
jn_oauth_verifier_new (void)
{
  static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  guint8 raw[64];
  random_bytes (raw, sizeof raw);
  char *v = g_malloc (sizeof raw + 1);
  for (gsize i = 0; i < sizeof raw; i++)
    v[i] = set[raw[i] % (sizeof set - 1)]; /* 66 characters: a tiny bias, still 385 bits */
  v[sizeof raw] = '\0';
  return v;
}

char *
jn_oauth_challenge (const char *verifier)
{
  guint8 digest[32];
  gsize len = sizeof digest;
  GChecksum *sum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (sum, (const guchar *) verifier, (gssize) strlen (verifier));
  g_checksum_get_digest (sum, digest, &len);
  g_checksum_free (sum);
  char *b = g_base64_encode (digest, len);
  /* base64url, no padding. */
  for (char *c = b; *c != '\0'; c++)
    if (*c == '+')
      *c = '-';
    else if (*c == '/')
      *c = '_';
  char *eq = strchr (b, '=');
  if (eq != NULL)
    *eq = '\0';
  return b;
}

char *
jn_oauth_state_new (void)
{
  guint8 raw[16];
  random_bytes (raw, sizeof raw);
  GString *s = g_string_new (NULL);
  for (gsize i = 0; i < sizeof raw; i++)
    g_string_append_printf (s, "%02x", raw[i]);
  return g_string_free (s, FALSE);
}

/* ---- the sign-in page and the return ----------------------------------------- */

static void
param (GString *s, const char *key, const char *value)
{
  g_autofree char *v = g_uri_escape_string (value, NULL, FALSE);
  g_string_append_printf (s, "%s%s=%s", s->len > 0 ? "&" : "", key, v);
}

char *
jn_oauth_auth_url (const JnOAuthClient *c, const char *redirect_uri, const char *challenge, const char *state)
{
  GString *q = g_string_new (NULL);
  param (q, "client_id", c->client_id);
  param (q, "redirect_uri", redirect_uri);
  param (q, "response_type", "code");
  param (q, "scope", JN_DRIVE_SCOPE);
  param (q, "code_challenge", challenge);
  param (q, "code_challenge_method", "S256");
  param (q, "state", state);
  param (q, "access_type", "offline"); /* a refresh token, so you sign in once */
  param (q, "prompt", "consent");      /* …and Google sends one every time */
  g_autofree char *query = g_string_free (q, FALSE);
  return g_strconcat (JN_OAUTH_AUTH_URL "?", query, NULL);
}

gboolean
jn_oauth_parse_return (const char *path_and_query, const char *state, char **code, GError **error)
{
  *code = NULL;
  const char *q = strchr (path_and_query, '?');
  g_autoptr (GHashTable) params = q != NULL ? g_uri_parse_params (q + 1, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
  const char *got_state = params != NULL ? g_hash_table_lookup (params, "state") : NULL;
  const char *err = params != NULL ? g_hash_table_lookup (params, "error") : NULL;
  const char *got_code = params != NULL ? g_hash_table_lookup (params, "code") : NULL;
  if (g_strcmp0 (err, "access_denied") == 0)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         "You did not allow Notes to use Google Drive, so it is not signed in.");
  else if (got_state == NULL && got_code == NULL && err == NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Google sent no sign-in code; try again.");
  else if (g_strcmp0 (got_state, state) != 0)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "That answer did not come from this sign-in, so it was ignored; try again.");
  else if (err != NULL)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Google's sign-in failed (%s); try again.", err);
  else if (got_code == NULL || *got_code == '\0')
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Google sent no sign-in code; try again.");
  else
    *code = g_strdup (got_code);
  return *code != NULL;
}

/* ---- the token endpoint ------------------------------------------------------- */

char *
jn_oauth_code_form (const JnOAuthClient *c, const char *code, const char *verifier, const char *redirect_uri)
{
  GString *s = g_string_new (NULL);
  param (s, "grant_type", "authorization_code");
  param (s, "code", code);
  param (s, "code_verifier", verifier);
  param (s, "client_id", c->client_id);
  param (s, "client_secret", c->client_secret);
  param (s, "redirect_uri", redirect_uri);
  return g_string_free (s, FALSE);
}

char *
jn_oauth_refresh_form (const JnOAuthClient *c, const char *refresh_token)
{
  GString *s = g_string_new (NULL);
  param (s, "grant_type", "refresh_token");
  param (s, "refresh_token", refresh_token);
  param (s, "client_id", c->client_id);
  param (s, "client_secret", c->client_secret);
  return g_string_free (s, FALSE);
}

/* Is `scope` (space-separated) holding JN_DRIVE_SCOPE? */
static gboolean
has_drive (const char *scope)
{
  g_auto (GStrv) parts = g_strsplit (scope, " ", -1);
  return g_strv_contains ((const char *const *) parts, JN_DRIVE_SCOPE);
}

gboolean
jn_oauth_parse_tokens (const char *json, gssize len, int status, JnOAuthTokens *out, GError **error)
{
  *out = (JnOAuthTokens){ 0 };
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  const char *err = jn_json_str (o, "error"), *access = jn_json_str (o, "access_token");
  const char *scope = jn_json_str (o, "scope");
  if (g_strcmp0 (err, "invalid_grant") == 0 || g_strcmp0 (err, "invalid_client") == 0 ||
      g_strcmp0 (err, "unauthorized_client") == 0)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         "Your Google sign-in has expired or was removed. " SIGN_IN_AGAIN);
  else if (o == NULL || status != 200)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Google's sign-in service answered with HTTP %d; try again "
                 "in a minute.", status);
  else if (access == NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Google's sign-in service gave no token. " SIGN_IN_AGAIN);
  else if (scope != NULL && !has_drive (scope))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         "Notes was not allowed to use Google Drive (the box on Google's page was left unticked). "
                         SIGN_IN_AGAIN);
  else
    {
      out->access = g_strdup (access);
      out->refresh = g_strdup (jn_json_str (o, "refresh_token"));
      gint64 expires = 0;
      jn_json_int (o, "expires_in", &expires);
      out->expires_in = CLAMP (expires, 0, MAX_LIFETIME);
    }
  g_object_unref (p);
  return out->access != NULL;
}

void
jn_oauth_tokens_clear (JnOAuthTokens *t)
{
  wipe_free (&t->access);
  wipe_free (&t->refresh);
  t->expires_in = 0;
}
