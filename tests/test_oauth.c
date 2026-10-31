/* Tests for "Sign in with Google" (src/notes/drive/oauth.c), LeetCode
 * style (see cases.h).
 *
 * Constraints: the challenge follows RFC 7636 exactly (its own example);
 * verifiers and states are fresh, long and URL-safe; a return with the
 * wrong state, an error or no code never gives a code; a token answer
 * without the Drive permission is refused; no error message ever holds
 * a token or the client secret. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "oauth.h"

#define SECRET "GOCSPX-test-client-secret"

static gboolean
no_secret (const char *message)
{
  return message == NULL || (strstr (message, SECRET) == NULL && strstr (message, "1//refresh") == NULL &&
                             strstr (message, "ya29.") == NULL);
}

static void
test_client_file (void)
{
  static const struct { const char *name, *json; gboolean ok; const char *says; } t[] = {
    { "a desktop client", "{\"installed\":{\"client_id\":\"123.apps.googleusercontent.com\",\"project_id\":\"p\","
                          "\"client_secret\":\"" SECRET "\",\"redirect_uris\":[\"http://localhost\"]}}", TRUE, NULL },
    { "a web client",     "{\"web\":{\"client_id\":\"1\",\"client_secret\":\"" SECRET "\"}}", FALSE, "Desktop app" },
    { "no secret",        "{\"installed\":{\"client_id\":\"1\"}}", FALSE, "not a Google client file" },
    { "not JSON",         "hello", FALSE, "not a Google client file" },
    { "a sign-in file instead", "{\"type\":\"authorized_user\",\"client_id\":\"1\",\"client_secret\":\"" SECRET
                          "\",\"refresh_token\":\"1//refresh\"}", FALSE, "not a Google client file" },
  };
  JrCases c = { "client file", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      JnOAuthClient cl = { 0 };
      g_autoptr (GError) error = NULL;
      gboolean ok = jn_oauth_client_parse (t[i].json, -1, &cl, &error);
      gboolean right = t[i].ok ? ok && g_strcmp0 (cl.client_id, "123.apps.googleusercontent.com") == 0 &&
                                 g_strcmp0 (cl.client_secret, SECRET) == 0
                               : !ok && error != NULL && strstr (error->message, t[i].says) != NULL &&
                                 no_secret (error->message) && cl.client_id == NULL;
      jr_case (&c, t[i].name, right, "Got ok=%d %s", ok, error != NULL ? error->message : "");
      jn_oauth_client_clear (&cl);
    }
  jr_cases_done (&c);
}

static void
test_pkce (void)
{
  /* RFC 7636, appendix B. */
  g_autofree char *rfc = jn_oauth_challenge ("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
  g_assert_cmpstr (rfc, ==, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");

  /* Fresh every time, the right length and characters. */
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (int i = 0; i < 200; i++)
    {
      char *v = jn_oauth_verifier_new ();
      g_assert_cmpuint (strlen (v), ==, 64);
      g_assert_cmpuint (strspn (v, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"), ==, 64);
      g_assert_false (g_hash_table_contains (seen, v));
      g_hash_table_add (seen, v);
      g_autofree char *s = jn_oauth_state_new ();
      g_assert_cmpuint (strlen (s), ==, 32);
      g_assert_cmpuint (strspn (s, "0123456789abcdef"), ==, 32);
      g_autofree char *ch = jn_oauth_challenge (v);
      g_assert_cmpuint (strlen (ch), ==, 43); /* 32 bytes, base64url, no padding */
      g_assert_null (strpbrk (ch, "+/="));
    }
  g_hash_table_unref (seen);
}

static void
test_auth_url (void)
{
  JnOAuthClient c = { (char *) "123.apps.googleusercontent.com", (char *) SECRET };
  g_autofree char *url = jn_oauth_auth_url (&c, "http://127.0.0.1:43123/", "CHALLENGE-x_y", "abc123");
  g_autoptr (GUri) u = g_uri_parse (url, G_URI_FLAGS_ENCODED_QUERY, NULL);
  g_assert_nonnull (u);
  g_assert_cmpstr (g_uri_get_scheme (u), ==, "https");
  g_assert_cmpstr (g_uri_get_host (u), ==, "accounts.google.com");
  g_autoptr (GHashTable) q = g_uri_parse_params (g_uri_get_query (u), -1, "&", G_URI_PARAMS_NONE, NULL);
  static const struct { const char *key, *want; } want[] = {
    { "client_id", "123.apps.googleusercontent.com" }, { "redirect_uri", "http://127.0.0.1:43123/" },
    { "response_type", "code" }, { "scope", JN_DRIVE_SCOPE }, { "code_challenge", "CHALLENGE-x_y" },
    { "code_challenge_method", "S256" }, { "state", "abc123" }, { "access_type", "offline" }, { "prompt", "consent" },
  };
  JrCases k = { "sign-in page", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (want); i++)
    jr_case (&k, want[i].key, g_strcmp0 (g_hash_table_lookup (q, want[i].key), want[i].want) == 0,
             "Expected %s  Got %s", want[i].want, (const char *) g_hash_table_lookup (q, want[i].key));
  jr_case (&k, "no secret in the address", strstr (url, SECRET) == NULL, "Got %s", url);
  jr_cases_done (&k);
}

static void
test_return (void)
{
  static const struct { const char *name, *path; const char *code, *says; } t[] = {
    { "a code",           "/?state=abc123&code=4%2F0Adeu5B&scope=https://www.googleapis.com/auth/drive.file",
                          "4/0Adeu5B", NULL },
    { "params reordered", "/?code=xyz&state=abc123", "xyz", NULL },
    { "you said no",      "/?error=access_denied&state=abc123", NULL, "did not allow" },
    { "wrong state",      "/?state=evil&code=xyz", NULL, "did not come from this sign-in" },
    { "no state",         "/?code=xyz", NULL, "did not come from this sign-in" },
    { "no code",          "/?state=abc123", NULL, "no sign-in code" },
    { "the favicon",      "/favicon.ico", NULL, "no sign-in code" },
    { "other error",      "/?error=server_error&state=abc123", NULL, "server_error" },
  };
  JrCases c = { "return", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      g_autofree char *code = NULL;
      g_autoptr (GError) error = NULL;
      gboolean ok = jn_oauth_parse_return (t[i].path, "abc123", &code, &error);
      gboolean right = t[i].code != NULL ? ok && g_strcmp0 (code, t[i].code) == 0
                                         : !ok && code == NULL && error != NULL && strstr (error->message, t[i].says);
      jr_case (&c, t[i].name, right, "Input %s  Got ok=%d code=%s %s", t[i].path, ok, code,
               error != NULL ? error->message : "");
    }
  jr_cases_done (&c);
}

static void
test_forms (void)
{
  JnOAuthClient c = { (char *) "123.apps", (char *) "s&cret=+" };
  g_autofree char *form = jn_oauth_code_form (&c, "4/0A&b", "verifier-1", "http://127.0.0.1:5/");
  g_autoptr (GHashTable) f = g_uri_parse_params (form, -1, "&", G_URI_PARAMS_NONE, NULL);
  g_assert_cmpstr (g_hash_table_lookup (f, "grant_type"), ==, "authorization_code");
  g_assert_cmpstr (g_hash_table_lookup (f, "code"), ==, "4/0A&b");
  g_assert_cmpstr (g_hash_table_lookup (f, "code_verifier"), ==, "verifier-1");
  g_assert_cmpstr (g_hash_table_lookup (f, "client_secret"), ==, "s&cret=+");
  g_assert_cmpstr (g_hash_table_lookup (f, "redirect_uri"), ==, "http://127.0.0.1:5/");
  g_autofree char *rf = jn_oauth_refresh_form (&c, "1//refresh+x");
  g_autoptr (GHashTable) r = g_uri_parse_params (rf, -1, "&", G_URI_PARAMS_NONE, NULL);
  g_assert_cmpstr (g_hash_table_lookup (r, "grant_type"), ==, "refresh_token");
  g_assert_cmpstr (g_hash_table_lookup (r, "refresh_token"), ==, "1//refresh+x");
}

static void
test_tokens (void)
{
  static const struct {
    const char *name, *json;
    int status;
    gboolean ok;
    const char *refresh;
    gint64 expires;
    const char *says;
  } t[] = {
    { "first sign-in", "{\"access_token\":\"ya29.a\",\"expires_in\":3599,\"refresh_token\":\"1//refresh\","
                       "\"scope\":\"" JN_DRIVE_SCOPE "\",\"token_type\":\"Bearer\"}", 200, TRUE, "1//refresh", 3599, NULL },
    { "a refresh",     "{\"access_token\":\"ya29.a\",\"expires_in\":3599,\"scope\":\"openid " JN_DRIVE_SCOPE "\"}",
                       200, TRUE, NULL, 3599, NULL },
    { "no scope given","{\"access_token\":\"ya29.a\",\"expires_in\":3599}", 200, TRUE, NULL, 3599, NULL },
    { "Drive unticked","{\"access_token\":\"ya29.a\",\"expires_in\":3599,\"scope\":\"openid\"}", 200, FALSE, NULL, 0,
                       "Google Drive" },
    { "wild lifetime", "{\"access_token\":\"ya29.a\",\"expires_in\":9000000000000}", 200, TRUE, NULL, 86400, NULL },
    { "revoked",       "{\"error\":\"invalid_grant\",\"error_description\":\"Token has been expired or revoked.\"}",
                       400, FALSE, NULL, 0, "Sign in" },
    { "server trouble", "<html>", 502, FALSE, NULL, 0, "502" },
  };
  JrCases c = { "tokens", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      JnOAuthTokens tok = { 0 };
      g_autoptr (GError) error = NULL;
      gboolean ok = jn_oauth_parse_tokens (t[i].json, -1, t[i].status, &tok, &error);
      gboolean right = t[i].ok ? ok && g_strcmp0 (tok.access, "ya29.a") == 0 && g_strcmp0 (tok.refresh, t[i].refresh) == 0 &&
                                 tok.expires_in == t[i].expires
                               : !ok && tok.access == NULL && error != NULL && strstr (error->message, t[i].says) != NULL &&
                                 no_secret (error->message);
      jr_case (&c, t[i].name, right, "Got ok=%d refresh=%s expires=%" G_GINT64_FORMAT " %s", ok, tok.refresh,
               tok.expires_in, error != NULL ? error->message : "");
      jn_oauth_tokens_clear (&tok);
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/oauth/client-file", test_client_file);
  g_test_add_func ("/oauth/pkce", test_pkce);
  g_test_add_func ("/oauth/auth-url", test_auth_url);
  g_test_add_func ("/oauth/return", test_return);
  g_test_add_func ("/oauth/forms", test_forms);
  g_test_add_func ("/oauth/tokens", test_tokens);
  return g_test_run ();
}
