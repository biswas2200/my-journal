/* driveclient: see driveclient.h. */
#include "driveclient.h"

#include <string.h>
#include <libsecret/secret.h>
#include <libsoup/soup.h>
#include "oauth.h"
#include "openuri.h"

#define SIGN_IN_TIMEOUT 300 /* seconds to finish on Google's page */
#define VISIT_TIMEOUT   5   /* seconds for whatever connects to say its piece */

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

typedef struct SignIn SignIn;
typedef struct Job Job;

typedef struct {
  JnBackup      base;
  char         *helper, *client_file, *auth_url, *token_url, *revoke_url, *api_base;
  guint16       ports[JN_DRIVE_MAX_PORTS];
  gsize         n_ports;
  JnSecretStore store;
  gboolean    (*open_uri) (const char *uri, gpointer user, GError **error);
  gpointer      open_user;
  SoupSession  *session;
  char         *refresh;     /* loaded once from the store */
  gboolean      loaded;
  char         *access;      /* cached access token */
  gint64        access_until; /* monotonic seconds */
  SignIn       *signing;     /* the sign-in under way, if any */
  GPtrArray    *jobs;        /* backups under way */
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

/* One sign-in, from opening Google's page to keeping the refresh token.
 * It ends exactly once (sign_in_finish): signed in, failed, or stopped
 * (a new sign-in, its cancellable, or the client freed as Notes closes).
 * A trade of the code still on its way holds a reference, so its answer
 * finds this alive and, the sign-in being over, drops it. */
struct SignIn {
  int             refs;
  Google         *g;          /* NULL once over */
  JnOAuthClient   client;
  char           *verifier, *state, *redirect;
  GSocketService *service;    /* the return address, 127.0.0.1 */
  GPtrArray      *visits;     /* connections to it still being read */
  GCancellable   *stop;       /* stops the trade of the code */
  GSource        *cancelled;  /* watches the caller's cancellable */
  guint           timeout_id;
  JnSignInDone    done;
  gpointer        user;
  gboolean        finished;
};

/* One connection to the return address, read without waiting on it:
 * whatever connects (the browser, or any program here) may send its
 * request slowly, or nothing at all. */
typedef struct {
  SignIn            *s;       /* NULL once the sign-in is over */
  GSocketConnection *conn;
  GCancellable      *cancel;  /* the sign-in ended, or VISIT_TIMEOUT passed */
  guint              timeout_id;
  gsize              got;
  char               buf[8192];
} Visit;

static void
sign_in_unref (SignIn *s)
{
  if (--s->refs > 0)
    return;
  g_ptr_array_unref (s->visits);
  g_object_unref (s->stop);
  g_free (s);
}

static void
visit_free (Visit *v)
{
  g_clear_handle_id (&v->timeout_id, g_source_remove);
  g_io_stream_close (G_IO_STREAM (v->conn), NULL, NULL);
  g_object_unref (v->conn);
  g_object_unref (v->cancel);
  g_free (v);
}

/* Ends the sign-in: closes the return address, wipes the secrets, and
 * tells the caller (ok FALSE with error NULL: stopped, nothing to say). */
static void
sign_in_finish (SignIn *s, gboolean ok, const char *error)
{
  if (s->finished)
    return;
  s->finished = TRUE;
  if (s->g != NULL && s->g->signing == s)
    s->g->signing = NULL;
  s->g = NULL;
  g_clear_handle_id (&s->timeout_id, g_source_remove);
  if (s->cancelled != NULL)
    {
      g_source_destroy (s->cancelled);
      g_clear_pointer (&s->cancelled, g_source_unref);
    }
  if (s->service != NULL)
    {
      g_signal_handlers_disconnect_by_data (s->service, s);
      g_socket_service_stop (s->service);
      g_socket_listener_close (G_SOCKET_LISTENER (s->service));
      g_clear_object (&s->service);
    }
  for (guint i = 0; i < s->visits->len; i++)
    {
      Visit *v = g_ptr_array_index (s->visits, i);
      v->s = NULL; /* its read ends cancelled and frees it */
      g_cancellable_cancel (v->cancel);
    }
  g_ptr_array_set_size (s->visits, 0);
  g_cancellable_cancel (s->stop);
  jn_oauth_client_clear (&s->client);
  wipe_free (&s->verifier);
  g_clear_pointer (&s->state, g_free);
  g_clear_pointer (&s->redirect, g_free);
  s->done (ok, error, s->user);
  sign_in_unref (s);
}

/* Google's answer to the code: keep the refresh token, use the access
 * token. */
static void
keep_tokens (SignIn *s, SoupMessage *msg, GBytes *body, const GError *error)
{
  if (body == NULL)
    {
      g_autofree char *m = g_strdup_printf ("Google's sign-in service could not be reached (%s).", error->message);
      sign_in_finish (s, FALSE, m);
      return;
    }
  gsize len = 0;
  const char *data = g_bytes_get_data (body, &len);
  JnOAuthTokens t = { 0 };
  g_autoptr (GError) parse_error = NULL;
  if (!jn_oauth_parse_tokens (data, (gssize) len, (int) soup_message_get_status (msg), &t, &parse_error))
    {
      sign_in_finish (s, FALSE, parse_error->message);
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
on_code_traded (GObject *src, GAsyncResult *res, gpointer data)
{
  SignIn *s = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  if (!s->finished) /* else it was stopped meanwhile: drop the answer */
    keep_tokens (s, msg, body, error);
  sign_in_unref (s);
}

static void
trade_code (SignIn *s, const char *code)
{
  g_autofree char *form = jn_oauth_code_form (&s->client, code, s->verifier, s->redirect);
  SoupMessage *msg = soup_message_new ("POST", s->g->token_url);
  g_autoptr (GBytes) body = g_bytes_new (form, strlen (form));
  memset (form, 0, strlen (form));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body);
  s->refs++; /* for on_code_traded */
  soup_session_send_and_read_async (s->g->session, msg, G_PRIORITY_DEFAULT, s->stop, on_code_traded, s);
  g_object_unref (msg);
}

static const char PAGE_OK[] =
  "<!doctype html><meta charset=utf-8><title>Notes</title><body style=\"font:16px sans-serif;margin:3em\">"
  "<h2>Signed in</h2><p>You can close this tab and go back to Notes.</p>";
static const char PAGE_NOT[] =
  "<!doctype html><meta charset=utf-8><title>Notes</title><body style=\"font:16px sans-serif;margin:3em\">"
  "<h2>Not signed in</h2><p>Go back to Notes to see why, and try again.</p>";

/* A whole request line arrived (or as much as ever will): "GET
 * /?state=…&code=… HTTP/1.1". Only one carrying this sign-in's state
 * counts: anything else (another program here, the browser asking for an
 * icon) is turned away and the sign-in goes on waiting. */
static void
answer_visit (SignIn *s, Visit *v)
{
  g_autofree char *path = NULL;
  if (g_str_has_prefix (v->buf, "GET "))
    {
      const char *start = v->buf + 4, *end = strpbrk (start, " \r\n");
      if (end != NULL)
        path = g_strndup (start, (gsize) (end - start));
    }
  const char *q = path != NULL ? strchr (path, '?') : NULL;
  g_autoptr (GHashTable) params = q != NULL ? g_uri_parse_params (q + 1, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
  gboolean ours = params != NULL && g_strcmp0 (g_hash_table_lookup (params, "state"), s->state) == 0;
  g_autofree char *code = NULL;
  g_autoptr (GError) error = NULL;
  gboolean ok = ours && jn_oauth_parse_return (path, s->state, &code, &error);
  const char *page = ok ? PAGE_OK : PAGE_NOT;
  g_autofree char *reply = g_strdup_printf ("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                                            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen (page), page);
  /* A few hundred bytes into an empty socket: this does not wait. */
  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (v->conn));
  g_output_stream_write_all (out, reply, strlen (reply), NULL, NULL, NULL);
  if (!ours)
    return;
  if (!ok)
    sign_in_finish (s, FALSE, error != NULL ? error->message : "Google sent no sign-in code; try again.");
  else
    {
      g_socket_service_stop (s->service); /* one answer is all it takes */
      trade_code (s, code);
    }
}

static void read_more (Visit *v);

static void
on_read (GObject *src, GAsyncResult *res, gpointer data)
{
  Visit *v = data;
  gssize n = g_input_stream_read_finish (G_INPUT_STREAM (src), res, NULL);
  if (n > 0)
    {
      v->got += (gsize) n;
      v->buf[v->got] = '\0';
      if (strstr (v->buf, "\r\n") == NULL && v->got < sizeof v->buf - 1)
        {
          read_more (v);
          return;
        }
    }
  SignIn *s = v->s;
  gboolean cut_off = g_cancellable_is_cancelled (v->cancel); /* too slow, or the sign-in ended */
  if (s != NULL)
    g_ptr_array_remove_fast (s->visits, v);
  if (s != NULL && !cut_off && v->got > 0)
    answer_visit (s, v);
  visit_free (v);
}

static void
read_more (Visit *v)
{
  GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (v->conn));
  g_input_stream_read_async (in, v->buf + v->got, sizeof v->buf - 1 - v->got, G_PRIORITY_DEFAULT, v->cancel, on_read,
                             v);
}

static gboolean
on_visit_timeout (gpointer data)
{
  Visit *v = data;
  v->timeout_id = 0;
  g_cancellable_cancel (v->cancel);
  return G_SOURCE_REMOVE;
}

static gboolean
on_incoming (GSocketService *service, GSocketConnection *conn, GObject *source, gpointer data)
{
  (void) service; (void) source;
  SignIn *s = data;
  Visit *v = g_new0 (Visit, 1);
  v->s = s;
  v->conn = g_object_ref (conn);
  v->cancel = g_cancellable_new ();
  v->timeout_id = g_timeout_add_seconds (VISIT_TIMEOUT, on_visit_timeout, v);
  g_ptr_array_add (s->visits, v);
  read_more (v);
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

static gboolean
on_sign_in_cancelled (GCancellable *cancel, gpointer data)
{
  (void) cancel;
  SignIn *s = data;
  g_clear_pointer (&s->cancelled, g_source_unref); /* this source ends now */
  sign_in_finish (s, FALSE, NULL);
  return G_SOURCE_REMOVE;
}

/* Not as Notes' child: through the desktop's portal (see openuri.h). */
static gboolean
open_in_browser (const char *uri, gpointer user, GError **error)
{
  (void) user;
  return jr_open_uri (uri, error);
}

static void
google_sign_in (JnBackup *self, GCancellable *cancel, JnSignInDone done, gpointer user)
{
  Google *g = (Google *) self;
  /* One at a time: a new sign-in (you clicked again, the old tab left
   * open) replaces the old one, which ends quietly. */
  if (g->signing != NULL)
    sign_in_finish (g->signing, FALSE, NULL);
  SignIn *s = g_new0 (SignIn, 1);
  s->refs = 1;
  s->g = g;
  s->done = done;
  s->user = user;
  s->visits = g_ptr_array_new ();
  s->stop = g_cancellable_new ();
  g->signing = s;
  if (cancel != NULL)
    {
      s->cancelled = g_cancellable_source_new (cancel);
      g_source_set_callback (s->cancelled, G_SOURCE_FUNC (on_sign_in_cancelled), s, NULL);
      g_source_attach (s->cancelled, NULL);
    }
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
  if (g->auth_url != NULL) /* a test's own sign-in page */
    {
      g_autofree char *moved = g_strconcat (g->auth_url, page + strlen (JN_OAUTH_AUTH_URL), NULL);
      g_free (page);
      page = g_steal_pointer (&moved);
    }
  s->timeout_id = g_timeout_add_seconds (SIGN_IN_TIMEOUT, on_sign_in_timeout, s);
  if (!g->open_uri (page, g->open_user, &error))
    {
      g_autofree char *m = g_strdup_printf ("Notes could not open your web browser for Google's sign-in page (%s). "
                                            "Choose a default web browser in Settings, then sign in again.",
                                            error != NULL ? error->message : "no reason given");
      sign_in_finish (s, FALSE, m);
    }
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

struct Job {
  Google        *g;      /* NULL once the client is freed */        /* NULL once the client is freed */
  JnDriveRequest req;      /* strings owned here */
  GCancellable  *cancel;
  JnBackupEach   each;
  JnBackupDone   done;
  gpointer       user;
  GSubprocess   *proc;
  GDataInputStream *lines; /* the helper's answer, line by line */
  char          *json;     /* the request, wiped once written */
  GArray        *results;  /* JnDriveResult, every one handed to `each` */
  char          *root, *end_error;
  gboolean       ended;
};

static void
job_free (Job *job)
{
  if (job->g != NULL)
    g_ptr_array_remove_fast (job->g->jobs, job);
  jn_drive_request_clear (&job->req);
  if (job->json != NULL)
    memset (job->json, 0, strlen (job->json)); /* it held the token */
  g_free (job->json);
  for (guint i = 0; job->results != NULL && i < job->results->len; i++)
    jn_drive_result_clear (&g_array_index (job->results, JnDriveResult, i));
  if (job->results != NULL)
    g_array_unref (job->results);
  g_free (job->root);
  g_free (job->end_error);
  g_clear_object (&job->lines);
  g_clear_object (&job->cancel);
  g_clear_object (&job->proc);
  g_free (job);
}

/* Before the helper ran: only an error. */
static void
job_fail (Job *job, const char *error)
{
  job->done (NULL, error, job->user);
  job_free (job);
}

/* The helper is done (or was stopped): everything it reported, and why
 * it stopped early if it did. */
static void
job_end (Job *job, const char *stopped)
{
  JnDriveAnswer a = { job->root, (char *) stopped, (JnDriveResult *) job->results->data, job->results->len };
  if (stopped == NULL && !job->ended)
    a.error = (char *) "The Drive helper stopped before it finished; the topics it did are kept, the others stay "
                       "marked for the next backup.";
  else if (stopped == NULL)
    a.error = job->end_error;
  /* An expired or revoked sign-in: forget it, so the window offers Sign
   * in again. */
  if (a.error != NULL && strstr (a.error, "Sign in") != NULL && job->g != NULL)
    forget (job->g);
  job->done (&a, NULL, job->user);
  job_free (job);
}

static void
on_line (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *job = data;
  g_autoptr (GError) error = NULL;
  gsize len = 0;
  g_autofree char *line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (src), res, &len, &error);
  if (line == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          /* Not only stop listening: stop the helper, so it makes no Doc
           * that Notes would not hear of. */
          g_subprocess_force_exit (job->proc);
          job_end (job, "Stopped. The topics already done are kept; the others stay marked for the next backup.");
        }
      else
        job_end (job, NULL); /* end of its answer */
      return;
    }
  JnDriveResult r;
  char *root = NULL, *err = NULL;
  JnDriveLine kind = jn_drive_line_parse (line, (gssize) len, &r, &root, &err);
  if (root != NULL)
    {
      g_free (job->root);
      job->root = root;
    }
  if (kind == JN_DRIVE_LINE_RESULT)
    {
      g_array_append_val (job->results, r);
      job->each (&r, job->root, job->user);
    }
  else if (kind == JN_DRIVE_LINE_END)
    {
      job->ended = TRUE;
      g_free (job->end_error);
      job->end_error = err;
    }
  g_data_input_stream_read_line_async (job->lines, G_PRIORITY_DEFAULT, job->cancel, on_line, job);
}

static void
on_request_written (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *job = data;
  g_output_stream_write_all_finish (G_OUTPUT_STREAM (src), res, NULL, NULL);
  g_output_stream_close (G_OUTPUT_STREAM (src), NULL, NULL); /* the helper reads to the end */
  memset (job->json, 0, strlen (job->json));
  g_clear_pointer (&job->json, g_free);
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
      job_fail (job, "The Drive helper is missing (reinstall Journal).");
      return;
    }
  job->results = g_array_new (FALSE, TRUE, sizeof (JnDriveResult));
  job->json = jn_drive_request_to_json (&job->req);
  g_output_stream_write_all_async (g_subprocess_get_stdin_pipe (job->proc), job->json, strlen (job->json),
                                   G_PRIORITY_DEFAULT, NULL, on_request_written, job);
  job->lines = g_data_input_stream_new (g_subprocess_get_stdout_pipe (job->proc));
  g_data_input_stream_read_line_async (job->lines, G_PRIORITY_DEFAULT, job->cancel, on_line, job);
}

static void
on_token (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *job = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  if (job->g == NULL) /* the client was freed meanwhile */
    {
      job_fail (job, "Stopped.");
      return;
    }
  if (body == NULL)
    {
      g_autofree char *m = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                             ? g_strdup ("Stopped.")
                             : g_strdup_printf ("Google could not be reached (%s). Your notes are safe on this laptop; "
                                                "the backup will be tried again.", error->message);
      job_fail (job, m);
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
      job_fail (job, error->message);
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
            JnBackupEach each, JnBackupDone done, gpointer user)
{
  Google *g = (Google *) self;
  Job *job = g_new0 (Job, 1);
  job->g = g;
  g_ptr_array_add (g->jobs, job);
  job->each = each;
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
      job_fail (job, "Not signed in to Google Drive. Sign in first.");
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
      job_fail (job, error->message);
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
  /* Notes is closing: a sign-in under way ends quietly; a backup under
   * way loses its client (its helper is stopped, its answer still comes). */
  if (g->signing != NULL)
    sign_in_finish (g->signing, FALSE, NULL);
  for (guint i = 0; i < g->jobs->len; i++)
    {
      Job *job = g_ptr_array_index (g->jobs, i);
      job->g = NULL;
      if (job->proc != NULL)
        g_subprocess_force_exit (job->proc);
    }
  g_ptr_array_unref (g->jobs);
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
  g->jobs = g_ptr_array_new ();
  return &g->base;
}
