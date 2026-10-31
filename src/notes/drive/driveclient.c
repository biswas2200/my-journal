/* driveclient: see driveclient.h. */
#include "driveclient.h"

#include <string.h>
#include <libsecret/secret.h>
#include <libsoup/soup.h>
#include "oauth.h"

#define SIGN_IN_TIMEOUT 300 /* seconds to finish on Google's page */

static void
wipe_free (char **s)
{
  if (*s != NULL)
    memset (*s, 0, strlen (*s));
  g_clear_pointer (s, g_free);
}

/* ---- the login keyring ----------------------------------------------------- */

static const SecretSchema *
schema (void)
{
  static const SecretSchema s = {
    "io.github.biswas2200.Journal.Notes.Drive", SECRET_SCHEMA_NONE,
    { { "kind", SECRET_SCHEMA_ATTRIBUTE_STRING }, { "NULL", 0 } },
    0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  };
  return &s;
}

static char *
keyring_load (gpointer user)
{
  (void) user;
  char *t = secret_password_lookup_sync (schema (), NULL, NULL, "kind", "refresh", NULL);
  char *out = g_strdup (t);
  secret_password_free (t);
  return out;
}

static gboolean
keyring_save (const char *token, gpointer user)
{
  (void) user;
  return secret_password_store_sync (schema (), SECRET_COLLECTION_DEFAULT, "Journal Notes: Google Drive", token, NULL,
                                     NULL, "kind", "refresh", NULL);
}

static void
keyring_clear (gpointer user)
{
  (void) user;
  secret_password_clear_sync (schema (), NULL, NULL, "kind", "refresh", NULL);
}

JnSecretStore
jn_keyring_store (void)
{
  return (JnSecretStore){ keyring_load, keyring_save, keyring_clear, NULL };
}

char *
jn_backup_client_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "journal-notes", "google-client.json", NULL);
}

/* ---- the client ---------------------------------------------------------------- */

typedef struct {
  JnBackup      base;
  char         *helper, *client_file, *auth_url, *token_url, *revoke_url, *api_base;
  guint16       ports[JN_DRIVE_MAX_PORTS];
  gsize         n_ports;
  JnSecretStore store;
  void        (*open_uri) (const char *uri, gpointer user);
  gpointer      open_user;
  SoupSession  *session;
  char         *refresh;     /* loaded once from the store */
  gboolean      loaded;
  char         *access;      /* cached access token */
  gint64        access_until; /* monotonic seconds */
} Google;

static const char *
refresh_token (Google *g)
{
  if (!g->loaded)
    {
      g->refresh = g->store.load (g->store.user);
      g->loaded = TRUE;
    }
  return g->refresh;
}

static void
forget (Google *g)
{
  wipe_free (&g->refresh);
  wipe_free (&g->access);
  g->loaded = TRUE;
  g->store.clear (g->store.user);
}

static gboolean
read_client (Google *g, JnOAuthClient *out, GError **error)
{
  g_autofree char *text = NULL;
  gsize len = 0;
  if (!g_file_get_contents (g->client_file, &text, &len, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Notes has no Google client file yet. Put the JSON of your \"Desktop app\" OAuth client at %s "
                   "(see README, \"Google Drive backup\").", g->client_file);
      return FALSE;
    }
  gboolean ok = jn_oauth_client_parse (text, (gssize) len, out, error);
  memset (text, 0, len);
  return ok;
}

static gboolean
google_can_sign_in (JnBackup *self)
{
  Google *g = (Google *) self;
  JnOAuthClient c = { 0 };
  gboolean ok = read_client (g, &c, NULL);
  jn_oauth_client_clear (&c);
  return ok;
}

static gboolean
google_signed_in (JnBackup *self)
{
  return refresh_token ((Google *) self) != NULL;
}

/* ---- signing in ----------------------------------------------------------------- */

typedef struct {
  Google         *g;
  JnOAuthClient   client;
  char           *verifier, *state, *redirect;
  GSocketService *service;
  GCancellable   *cancel;
  guint           timeout_id;
  JnSignInDone    done;
  gpointer        user;
  gboolean        finished;
} SignIn;

static void
sign_in_finish (SignIn *s, gboolean ok, const char *error)
{
  if (s->finished)
    return;
  s->finished = TRUE;
  g_clear_handle_id (&s->timeout_id, g_source_remove);
  if (s->service != NULL)
    {
      g_socket_service_stop (s->service);
      g_socket_listener_close (G_SOCKET_LISTENER (s->service));
    }
  s->done (ok, error, s->user);
  jn_oauth_client_clear (&s->client);
  wipe_free (&s->verifier);
  g_free (s->state);
  g_free (s->redirect);
  g_clear_object (&s->service);
  g_clear_object (&s->cancel);
  g_free (s);
}

static void
on_code_traded (GObject *src, GAsyncResult *res, gpointer data)
{
  SignIn *s = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  if (body == NULL)
    {
      g_autofree char *m = g_strdup_printf ("Google's sign-in service could not be reached (%s).", error->message);
      sign_in_finish (s, FALSE, m);
      return;
    }
  gsize len = 0;
  const char *data_ = g_bytes_get_data (body, &len);
  JnOAuthTokens t = { 0 };
  if (!jn_oauth_parse_tokens (data_, (gssize) len, (int) soup_message_get_status (msg), &t, &error))
    {
      sign_in_finish (s, FALSE, error->message);
      return;
    }
  if (t.refresh == NULL)
    {
      jn_oauth_tokens_clear (&t);
      sign_in_finish (s, FALSE, "Google gave no lasting sign-in; try signing in again.");
      return;
    }
  Google *g = s->g;
  if (!g->store.save (t.refresh, g->store.user))
    {
      jn_oauth_tokens_clear (&t);
      sign_in_finish (s, FALSE, "The sign-in could not be kept in your login keyring (is it unlocked?).");
      return;
    }
  wipe_free (&g->refresh);
  g->refresh = g_steal_pointer (&t.refresh);
  g->loaded = TRUE;
  wipe_free (&g->access);
  g->access = g_steal_pointer (&t.access);
  g->access_until = g_get_monotonic_time () / G_USEC_PER_SEC + MAX (t.expires_in - 120, 0);
  jn_oauth_tokens_clear (&t);
  sign_in_finish (s, TRUE, NULL);
}

static void
trade_code (SignIn *s, const char *code)
{
  g_autofree char *form = jn_oauth_code_form (&s->client, code, s->verifier, s->redirect);
  SoupMessage *msg = soup_message_new ("POST", s->g->token_url);
  g_autoptr (GBytes) body = g_bytes_new (form, strlen (form));
  memset (form, 0, strlen (form));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body);
  soup_session_send_and_read_async (s->g->session, msg, G_PRIORITY_DEFAULT, s->cancel, on_code_traded, s);
  g_object_unref (msg);
}

static const char PAGE_OK[] =
  "<!doctype html><meta charset=utf-8><title>Notes</title><body style=\"font:16px sans-serif;margin:3em\">"
  "<h2>Signed in</h2><p>You can close this tab and go back to Notes.</p>";
static const char PAGE_NOT[] =
  "<!doctype html><meta charset=utf-8><title>Notes</title><body style=\"font:16px sans-serif;margin:3em\">"
  "<h2>Not signed in</h2><p>Go back to Notes to see why, and try again.</p>";

/* The browser's one request to 127.0.0.1: "GET /?state=…&code=… HTTP/1.1". */
static gboolean
on_incoming (GSocketService *service, GSocketConnection *conn, GObject *source, gpointer data)
{
  (void) service; (void) source;
  SignIn *s = data;
  if (s->finished)
    return TRUE;
  /* Whatever connects must say its piece quickly. */
  g_socket_set_timeout (g_socket_connection_get_socket (conn), 5);
  GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  char buf[8192];
  gsize got = 0;
  /* Up to the end of the request line: a few hundred bytes at most. */
  while (got < sizeof buf - 1)
    {
      gssize n = g_input_stream_read (in, buf + got, sizeof buf - 1 - got, NULL, NULL);
      if (n <= 0)
        break;
      got += (gsize) n;
      buf[got] = '\0';
      if (strstr (buf, "\r\n") != NULL)
        break;
    }
  buf[got] = '\0';
  g_autofree char *path = NULL;
  if (g_str_has_prefix (buf, "GET "))
    {
      const char *start = buf + 4, *end = strchr (start, ' ');
      if (end != NULL)
        path = g_strndup (start, (gsize) (end - start));
    }
  /* Only an answer carrying this sign-in's state counts: anything else
   * (another program on this laptop, the browser asking for an icon) is
   * turned away and the sign-in goes on waiting. */
  const char *q = path != NULL ? strchr (path, '?') : NULL;
  g_autoptr (GHashTable) params = q != NULL ? g_uri_parse_params (q + 1, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
  gboolean ours = params != NULL && g_strcmp0 (g_hash_table_lookup (params, "state"), s->state) == 0;
  g_autofree char *code = NULL;
  g_autoptr (GError) error = NULL;
  gboolean ok = ours && jn_oauth_parse_return (path, s->state, &code, &error);
  const char *page = ok ? PAGE_OK : PAGE_NOT;
  g_autofree char *reply = g_strdup_printf ("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                                            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen (page), page);
  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  g_output_stream_write_all (out, reply, strlen (reply), NULL, NULL, NULL);
  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  if (!ours)
    return TRUE;
  if (!ok)
    sign_in_finish (s, FALSE, error != NULL ? error->message : "Google sent no sign-in code; try again.");
  else
    {
      g_socket_service_stop (s->service);
      trade_code (s, code);
    }
  return TRUE;
}

static gboolean
on_sign_in_timeout (gpointer data)
{
  SignIn *s = data;
  s->timeout_id = 0;
  sign_in_finish (s, FALSE, "The sign-in was not finished in 5 minutes; try again.");
  return G_SOURCE_REMOVE;
}

static void
open_in_browser (const char *uri, gpointer user)
{
  (void) user;
  g_app_info_launch_default_for_uri (uri, NULL, NULL);
}

static void
google_sign_in (JnBackup *self, GCancellable *cancel, JnSignInDone done, gpointer user)
{
  Google *g = (Google *) self;
  SignIn *s = g_new0 (SignIn, 1);
  s->g = g;
  s->done = done;
  s->user = user;
  s->cancel = cancel != NULL ? g_object_ref (cancel) : g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  if (!read_client (g, &s->client, &error))
    {
      sign_in_finish (s, FALSE, error->message);
      return;
    }
  /* 127.0.0.1 only, any free port: nothing else can reach it. */
  s->service = g_socket_service_new ();
  g_autoptr (GInetAddress) lo = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  g_autoptr (GSocketAddress) any = g_inet_socket_address_new (lo, 0);
  g_autoptr (GSocketAddress) bound = NULL;
  if (!g_socket_listener_add_address (G_SOCKET_LISTENER (s->service), any, G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error))
    {
      g_autofree char *m = g_strdup_printf ("Notes could not wait for Google's answer (%s).", error->message);
      sign_in_finish (s, FALSE, m);
      return;
    }
  s->redirect = g_strdup_printf ("http://127.0.0.1:%u/", g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound)));
  g_signal_connect (s->service, "incoming", G_CALLBACK (on_incoming), s);
  g_socket_service_start (s->service);
  s->verifier = jn_oauth_verifier_new ();
  s->state = jn_oauth_state_new ();
  g_autofree char *challenge = jn_oauth_challenge (s->verifier);
  g_autofree char *page = jn_oauth_auth_url (&s->client, s->redirect, challenge, s->state);
  if (g->auth_url != NULL && g_str_has_prefix (page, JN_OAUTH_AUTH_URL))
    {
      g_autofree char *moved = g_strconcat (g->auth_url, page + strlen (JN_OAUTH_AUTH_URL), NULL);
      g_free (page);
      page = g_steal_pointer (&moved);
    }
  s->timeout_id = g_timeout_add_seconds (SIGN_IN_TIMEOUT, on_sign_in_timeout, s);
  g->open_uri (page, g->open_user);
}

static void
on_revoked (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) data;
  g_autoptr (GBytes) b = soup_session_send_and_read_finish (SOUP_SESSION (src), res, NULL);
}

static void
google_sign_out (JnBackup *self)
{
  Google *g = (Google *) self;
  const char *refresh = refresh_token (g);
  if (refresh != NULL)
    {
      /* Best effort: if Google cannot be reached, it is forgotten here
       * anyway, and you can remove Notes at myaccount.google.com. */
      g_autofree char *escaped = g_uri_escape_string (refresh, NULL, FALSE);
      g_autofree char *form = g_strconcat ("token=", escaped, NULL);
      SoupMessage *msg = soup_message_new ("POST", g->revoke_url);
      g_autoptr (GBytes) body = g_bytes_new (form, strlen (form));
      soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body);
      soup_session_send_and_read_async (g->session, msg, G_PRIORITY_DEFAULT, NULL, on_revoked, NULL);
      g_object_unref (msg);
    }
  forget (g);
}

/* ---- backing up ---------------------------------------------------------------- */

typedef struct {
  Google        *g;
  JnDriveRequest req; /* strings owned here */
  GCancellable  *cancel;
  JnBackupDone   done;
  gpointer       user;
  GSubprocess   *proc;
} Job;

static void
job_finish (Job *job, const JnDriveAnswer *answer, const char *error)
{
  job->done (answer, error, job->user);
  jn_drive_request_clear (&job->req);
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
      job_finish (job, NULL, g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                               ? "Stopped." : "The Drive helper did not answer.");
      return;
    }
  JnDriveAnswer a = { 0 };
  if (!jn_drive_answer_from_json (out, -1, &a, NULL))
    job_finish (job, NULL, "The Drive helper stopped without an answer.");
  else
    {
      /* An expired sign-in: forget it, so the window offers Sign in. */
      if (a.error != NULL && strstr (a.error, "Sign in") != NULL && a.n_results == 0)
        forget (job->g);
      job_finish (job, &a, NULL);
    }
  jn_drive_answer_clear (&a);
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
      job_finish (job, NULL, "The Drive helper is missing (reinstall Journal).");
      return;
    }
  g_autofree char *json = jn_drive_request_to_json (&job->req);
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
                             : g_strdup_printf ("Google could not be reached (%s). Your notes are safe on this laptop; "
                                                "the backup will be tried again.", error->message);
      job_finish (job, NULL, m);
      return;
    }
  gsize len = 0;
  const char *data_ = g_bytes_get_data (body, &len);
  int status = (int) soup_message_get_status (msg);
  JnOAuthTokens t = { 0 };
  if (!jn_oauth_parse_tokens (data_, (gssize) len, status, &t, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        forget (job->g); /* revoked or expired: offer Sign in again */
      job_finish (job, NULL, error->message);
      return;
    }
  Google *g = job->g;
  wipe_free (&g->access);
  g->access = g_steal_pointer (&t.access);
  g->access_until = g_get_monotonic_time () / G_USEC_PER_SEC + MAX (t.expires_in - 120, 0); /* renew 2 min early */
  jn_oauth_tokens_clear (&t);
  job->req.token = g_strdup (g->access);
  run_helper (job);
}

static void
google_run (JnBackup *self, const char *root, const JnDriveItem *items, gsize n_items, GCancellable *cancel,
            JnBackupDone done, gpointer user)
{
  Google *g = (Google *) self;
  Job *job = g_new0 (Job, 1);
  job->g = g;
  job->done = done;
  job->user = user;
  job->cancel = cancel != NULL ? g_object_ref (cancel) : g_cancellable_new ();
  job->req.api_base = g_strdup (g->api_base);
  memcpy (job->req.ports, g->ports, sizeof g->ports);
  job->req.n_ports = g->n_ports;
  job->req.root = g_strdup (root);
  job->req.items = g_new0 (JnDriveItem, MAX (n_items, 1));
  job->req.n_items = n_items;
  for (gsize i = 0; i < n_items; i++)
    job->req.items[i] = (JnDriveItem){ items[i].topic, g_strdup (items[i].subject), g_strdup (items[i].name),
                                       g_strdup (items[i].markdown), g_strdup (items[i].doc),
                                       g_strdup (items[i].folder) };
  const char *refresh = refresh_token (g);
  if (refresh == NULL)
    {
      job_finish (job, NULL, "Not signed in to Google Drive. Sign in first.");
      return;
    }
  if (g->access != NULL && g_get_monotonic_time () / G_USEC_PER_SEC < g->access_until)
    {
      job->req.token = g_strdup (g->access);
      run_helper (job);
      return;
    }
  JnOAuthClient c = { 0 };
  g_autoptr (GError) error = NULL;
  if (!read_client (g, &c, &error))
    {
      job_finish (job, NULL, error->message);
      return;
    }
  g_autofree char *form = jn_oauth_refresh_form (&c, refresh);
  jn_oauth_client_clear (&c);
  SoupMessage *msg = soup_message_new ("POST", g->token_url);
  g_autoptr (GBytes) body = g_bytes_new (form, strlen (form));
  memset (form, 0, strlen (form));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body);
  soup_session_send_and_read_async (g->session, msg, G_PRIORITY_DEFAULT, job->cancel, on_token, job);
  g_object_unref (msg);
}

static void
google_free (JnBackup *self)
{
  Google *g = (Google *) self;
  g_free (g->helper);
  g_free (g->client_file);
  g_free (g->auth_url);
  g_free (g->token_url);
  g_free (g->revoke_url);
  g_free (g->api_base);
  wipe_free (&g->refresh);
  wipe_free (&g->access);
  g_clear_object (&g->session);
  g_free (g);
}

JnBackup *
jn_google_backup_new (const JnBackupConfig *config)
{
  Google *g = g_new0 (Google, 1);
  g->base = (JnBackup){ google_can_sign_in, google_signed_in, google_sign_in, google_sign_out, google_run,
                        google_free };
  g->helper = g_strdup (config->helper);
  g->client_file = config->client_file != NULL ? g_strdup (config->client_file) : jn_backup_client_path ();
  g->auth_url = g_strdup (config->auth_url); /* NULL: Google's own page */
  g->token_url = g_strdup (config->token_url != NULL ? config->token_url : JN_OAUTH_TOKEN_URL);
  g->revoke_url = g_strdup (config->revoke_url != NULL ? config->revoke_url : JN_OAUTH_REVOKE_URL);
  g->api_base = g_strdup (config->api_base != NULL ? config->api_base : JN_DRIVE_API);
  if (config->n_ports > 0)
    {
      g->n_ports = MIN (config->n_ports, (gsize) JN_DRIVE_MAX_PORTS);
      memcpy (g->ports, config->ports, g->n_ports * sizeof g->ports[0]);
    }
  else
    {
      g->ports[0] = 443;
      g->n_ports = 1;
    }
  g->store = config->store.load != NULL ? config->store : jn_keyring_store ();
  g->open_uri = config->open_uri != NULL ? config->open_uri : open_in_browser;
  g->open_user = config->open_user;
  g->session = soup_session_new_with_options ("timeout", 30, "user-agent", "Journal-Notes/1.0 (backup)", NULL);
  return &g->base;
}
