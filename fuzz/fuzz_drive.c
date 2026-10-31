/* Fuzz target: everything the Drive backup reads that comes from
 * outside: Google Drive's answers, Google's sign-in answers and client
 * file, the browser's return to the sign-in address, and the messages
 * between Notes and its Drive helper. The first byte picks which (and,
 * for HTTP answers, the status); the rest is the message.
 *
 * Text that comes out is valid UTF-8; no error ever holds a token; a
 * request read, written and read again comes back the same; a return
 * with the wrong state never gives a code; a text is always the same as
 * itself. A wrong state never giving a code is in test_oauth's table. */
#define FUZZ_NOTES
#include <string.h>
#include "fuzz.h"
#include "driveproto.h"
#include "oauth.h"

static void
check_text (const char *s)
{
  FUZZ_CHECK (s == NULL || g_utf8_validate (s, -1, NULL));
}

static void
check_no_token (const char *message, const char *data, gsize len)
{
  /* Any "access_token" or "refresh_token" value in the input must not
   * come back in a message. */
  static const char *const keys[] = { "\"access_token\":\"", "\"refresh_token\":\"" };
  g_autofree char *copy = g_strndup (data, len);
  for (gsize k = 0; message != NULL && k < G_N_ELEMENTS (keys); k++)
    {
      const char *at = strstr (copy, keys[k]);
      if (at == NULL)
        continue;
      at += strlen (keys[k]);
      g_autofree char *value = g_strndup (at, strcspn (at, "\"\\"));
      if (strlen (value) >= 8)
        FUZZ_CHECK (strstr (message, value) == NULL);
    }
}

static void
request (const char *json, gsize len)
{
  JnDriveRequest r;
  g_autoptr (GError) error = NULL;
  gboolean ok = jn_drive_request_from_json (json, (gssize) len, &r, &error);
  FUZZ_CHECK (ok == (error == NULL));
  if (!ok)
    {
      FUZZ_CHECK (r.token == NULL && r.items == NULL);
      return;
    }
  FUZZ_CHECK (r.n_ports >= 1 && r.n_ports <= JN_DRIVE_MAX_PORTS && r.n_items <= JN_DRIVE_MAX_ITEMS);
  check_text (r.token);
  check_text (r.root);
  g_autofree char *out = jn_drive_request_to_json (&r);
  JnDriveRequest b;
  FUZZ_CHECK (jn_drive_request_from_json (out, -1, &b, NULL));
  FUZZ_CHECK (strcmp (r.token, b.token) == 0 && strcmp (r.api_base, b.api_base) == 0 && r.n_items == b.n_items);
  FUZZ_CHECK (g_strcmp0 (r.root, b.root) == 0 && memcmp (r.ports, b.ports, sizeof r.ports) == 0);
  for (gsize i = 0; i < r.n_items; i++)
    {
      FUZZ_CHECK (r.items[i].topic == b.items[i].topic);
      FUZZ_CHECK (strcmp (r.items[i].markdown, b.items[i].markdown) == 0);
      FUZZ_CHECK (g_strcmp0 (r.items[i].doc, b.items[i].doc) == 0);
      FUZZ_CHECK (g_strcmp0 (r.items[i].folder, b.items[i].folder) == 0);
    }
  jn_drive_request_clear (&r);
  jn_drive_request_clear (&b);
}

static void
lines (const char *text, gsize len)
{
  g_autofree char *copy = g_strndup (text, len);
  JnDriveAnswer a;
  jn_drive_answer_from_lines (copy, &a);
  check_text (a.root);
  check_text (a.error);
  for (gsize i = 0; i < a.n_results; i++)
    {
      FUZZ_CHECK (a.results[i].topic > 0);
      check_text (a.results[i].doc);
      check_text (a.results[i].error);
    }
  jn_drive_answer_clear (&a);
}

static void
drive_answers (const char *json, gsize len, int status)
{
  g_autofree char *id = NULL, *link = NULL;
  gboolean trashed = FALSE;
  if (jn_drive_parse_file (json, (gssize) len, &id, &trashed, &link))
    {
      FUZZ_CHECK (id != NULL);
      check_text (id);
      check_text (link);
    }
  gboolean shared = FALSE;
  jn_drive_parse_permissions (json, (gssize) len, &shared);
  g_autofree char *first = NULL, *first_link = NULL;
  if (jn_drive_parse_first (json, (gssize) len, &first, &first_link))
    check_text (first);
  g_autofree char *msg = jn_drive_error (json, (gssize) len, status);
  FUZZ_CHECK (msg != NULL && *msg != '\0');
  check_text (msg);
  check_no_token (msg, json, len);
}

static void
sign_in (const char *json, gsize len, int status)
{
  JnOAuthTokens t;
  g_autoptr (GError) error = NULL;
  gboolean ok = jn_oauth_parse_tokens (json, (gssize) len, status, &t, &error);
  FUZZ_CHECK (ok == (t.access != NULL) && ok == (error == NULL));
  if (ok)
    {
      FUZZ_CHECK (status == 200 && t.expires_in >= 0 && t.expires_in <= 24 * 60 * 60);
      check_text (t.access);
      check_text (t.refresh);
    }
  else
    {
      check_text (error->message);
      check_no_token (error->message, json, len);
    }
  jn_oauth_tokens_clear (&t);

  JnOAuthClient c;
  g_autoptr (GError) cerr = NULL;
  if (jn_oauth_client_parse (json, (gssize) len, &c, &cerr))
    {
      check_text (c.client_id);
      g_autofree char *url = jn_oauth_auth_url (&c, "http://127.0.0.1:1/", "challenge", "state");
      FUZZ_CHECK (g_str_has_prefix (url, JN_OAUTH_AUTH_URL "?"));
    }
  else
    FUZZ_CHECK (cerr != NULL && c.client_secret == NULL);
  jn_oauth_client_clear (&c);
}

static void
browser_return (const char *path, gsize len)
{
  g_autofree char *copy = g_strndup (path, len);
  g_autofree char *code = NULL;
  g_autoptr (GError) error = NULL;
  gboolean ok = jn_oauth_parse_return (copy, "the-state", &code, &error);
  FUZZ_CHECK (ok == (code != NULL) && ok == (error == NULL));
  if (ok)
    FUZZ_CHECK (*code != '\0');
  else
    check_text (error->message);
}

static void
same (const char *text, gsize len)
{
  g_autofree char *copy = g_strndup (text, len);
  FUZZ_CHECK (jn_drive_same_text (copy, copy));
  gsize half = strlen (copy) / 2;
  g_autofree char *a = g_strndup (copy, half);
  jn_drive_same_text (a, copy + half);
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  if (size < 1)
    return 0;
  static const int STATUS[] = { 200, 200, 400, 401, 403, 404, 429, 503 };
  int status = STATUS[(data[0] >> 4) % G_N_ELEMENTS (STATUS)];
  const char *text = (const char *) data + 1;
  gsize len = size - 1;
  switch (data[0] % 6)
    {
    case 0: request (text, len); break;
    case 1: lines (text, len); break;
    case 2: drive_answers (text, len, status); break;
    case 3: sign_in (text, len, status); break;
    case 4: browser_return (text, len); break;
    default: same (text, len); break;
    }
  return 0;
}
