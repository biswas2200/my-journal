/* A fake Google on this machine for the tests (see fake_google.h). */
#include "fake_google.h"

#include <string.h>
#include "jsonsafe.h"

char *fg_base;
GMutex fg_lock;
GHashTable *fg_files;
int fg_next_id;
int fg_requests;
gboolean fg_fail_auth;
char *fg_full_name;
char *fg_lossy_id;
gboolean fg_bad_token;
char *fg_last_verifier, *fg_last_redirect, *fg_revoked;
int fg_token_calls;
gboolean fg_refresh_revoked;
int fg_delay_ms;

/* ---- the fake sign-in service ------------------------------------------------- */

static void fg_reply_json (SoupServerMessage *msg, guint status, const char *json);

/* /oauth/token: a code or a refresh token for tokens; /oauth/revoke. */
static void
fg_serve_sign_in (SoupServerMessage *msg, const char *path)
{
  SoupMessageBody *body = soup_server_message_get_request_body (msg);
  g_autoptr (GHashTable) form = g_uri_parse_params (body->data != NULL ? body->data : "", (gssize) body->length, "&",
                                                    G_URI_PARAMS_WWW_FORM, NULL);
  const char *grant = form != NULL ? g_hash_table_lookup (form, "grant_type") : NULL;
  if (strcmp (path, "/oauth/revoke") == 0)
    {
      g_free (fg_revoked);
      fg_revoked = g_strdup (form != NULL ? g_hash_table_lookup (form, "token") : NULL);
      fg_reply_json (msg, 200, "{}");
      return;
    }
  fg_token_calls++;
  if (g_strcmp0 (grant, "authorization_code") == 0 && g_strcmp0 (g_hash_table_lookup (form, "code"), FG_CODE) == 0 &&
      g_strcmp0 (g_hash_table_lookup (form, "client_secret"), FG_SECRET) == 0)
    {
      g_free (fg_last_verifier);
      g_free (fg_last_redirect);
      fg_last_verifier = g_strdup (g_hash_table_lookup (form, "code_verifier"));
      fg_last_redirect = g_strdup (g_hash_table_lookup (form, "redirect_uri"));
      fg_reply_json (msg, 200, "{\"access_token\":\"" FG_ACCESS "\",\"expires_in\":3599,\"refresh_token\":\""
                               FG_REFRESH "\",\"scope\":\"https://www.googleapis.com/auth/drive.file\"}");
    }
  else if (g_strcmp0 (grant, "refresh_token") == 0 && !fg_refresh_revoked &&
           g_strcmp0 (g_hash_table_lookup (form, "refresh_token"), FG_REFRESH) == 0)
    fg_reply_json (msg, 200, "{\"access_token\":\"" FG_ACCESS "\",\"expires_in\":3599,"
                             "\"scope\":\"https://www.googleapis.com/auth/drive.file\"}");
  else
    fg_reply_json (msg, 400, "{\"error\":\"invalid_grant\",\"error_description\":\"Token has been expired or revoked.\"}");
}


static void
fg_file_free (gpointer p)
{
  FgFile *f = p;
  g_free (f->id);
  g_free (f->name);
  g_free (f->mime);
  g_free (f->parent);
  g_free (f->content);
  g_free (f);
}

static void
fg_reply_json (SoupServerMessage *msg, guint status, const char *json)
{
  soup_server_message_set_status (msg, status, NULL);
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, json, strlen (json));
}

static void
fg_reply_file (SoupServerMessage *msg, const FgFile *f)
{
  g_autofree char *json = g_strdup_printf ("{\"id\":\"%s\",\"name\":\"%s\",\"trashed\":%s,"
                                           "\"webViewLink\":\"https://docs.google.com/document/d/%s/edit\"}",
                                           f->id, f->name, f->trashed ? "true" : "false", f->id);
  fg_reply_json (msg, 200, json);
}

/* Markdown as Google writes it back: "*" bullets, escaped brackets. */
static char *
fg_google_style (const char *md)
{
  GString *s = g_string_new (NULL);
  g_auto (GStrv) lines = g_strsplit (md, "\n", -1);
  for (char **l = lines; *l != NULL; l++)
    {
      const char *t = *l;
      gsize indent = strspn (t, " ");
      if (t[indent] == '-' && t[indent + 1] == ' ')
        {
          g_string_append_len (s, t, (gssize) indent);
          g_string_append (s, "* ");
          t += indent + 2;
        }
      for (; *t != '\0'; t++)
        {
          if (*t == '[' || *t == ']')
            g_string_append_c (s, '\\');
          g_string_append_c (s, *t);
        }
      g_string_append (s, "\n\n");
    }
  return g_string_free (s, FALSE);
}

/* The Markdown part of a multipart upload. */
static char *
fg_media_of (const char *body, gsize len, const char *content_type, char **metadata)
{
  const char *b = strstr (content_type, "boundary=");
  if (b == NULL)
    return NULL;
  g_autofree char *sep = g_strdup_printf ("--%s", b + strlen ("boundary="));
  g_autofree char *text = g_strndup (body, len);
  g_auto (GStrv) parts = g_strsplit (text, sep, -1);
  char *media = NULL;
  for (char **p = parts; *p != NULL; p++)
    {
      const char *head_end = strstr (*p, "\r\n\r\n");
      if (head_end == NULL)
        continue;
      g_autofree char *payload = g_strdup (head_end + 4);
      gsize n = strlen (payload);
      if (n >= 2 && payload[n - 2] == '\r' && payload[n - 1] == '\n')
        payload[n - 2] = '\0';
      if (strstr (*p, "application/json") != NULL && strstr (*p, "application/json") < head_end)
        *metadata = g_steal_pointer (&payload);
      else if (strstr (*p, "text/markdown") != NULL)
        media = g_steal_pointer (&payload);
    }
  return media;
}

static FgFile *
fg_add_file (const char *name, const char *mime, const char *parent, const char *content)
{
  FgFile *f = g_new0 (FgFile, 1);
  f->id = g_strdup_printf ("id%d", ++fg_next_id);
  f->name = g_strdup (name);
  f->mime = g_strdup (mime);
  f->parent = g_strdup (parent);
  f->content = g_strdup (content);
  g_hash_table_insert (fg_files, f->id, f);
  return f;
}

/* The value of `key = '...'` (or `'...' in parents` for key NULL) in a
 * Drive query, unescaped. */
static char *
fg_query_value (const char *q, const char *key)
{
  const char *at;
  if (key != NULL)
    {
      g_autofree char *pre = g_strdup_printf ("%s = '", key);
      at = strstr (q, pre);
      if (at == NULL)
        return NULL;
      at += strlen (pre);
    }
  else
    {
      at = strstr (q, "' in parents");
      if (at == NULL)
        return NULL;
      while (at > q && !(at[-1] == '\'' && (at - 1 == q || at[-2] != '\\')))
        at--;
    }
  GString *v = g_string_new (NULL);
  for (; *at != '\0' && *at != '\''; at++)
    {
      if (*at == '\\' && at[1] != '\0')
        at++;
      g_string_append_c (v, *at);
    }
  return g_string_free (v, FALSE);
}

/* files.list with a q: the first live file matching name, type, parent. */
static void
fg_list (SoupServerMessage *msg, const char *q)
{
  g_autofree char *name = fg_query_value (q, "name");
  g_autofree char *mime = fg_query_value (q, "mimeType");
  g_autofree char *parent = fg_query_value (q, NULL);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, fg_files);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      FgFile *f = value;
      if (!f->trashed && g_strcmp0 (f->name, name) == 0 && g_strcmp0 (f->mime, mime) == 0 &&
          g_strcmp0 (f->parent, parent) == 0)
        {
          g_autofree char *json = g_strdup_printf ("{\"files\":[{\"id\":\"%s\",\"webViewLink\":"
                                                   "\"https://docs.google.com/document/d/%s/edit\"}]}", f->id, f->id);
          fg_reply_json (msg, 200, json);
          return;
        }
    }
  fg_reply_json (msg, 200, "{\"files\":[]}");
}

static void
fg_serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user)
{
  (void) server; (void) user;
  g_mutex_lock (&fg_lock);
  if (g_str_has_prefix (path, "/oauth/"))
    {
      fg_serve_sign_in (msg, path);
      g_mutex_unlock (&fg_lock);
      return;
    }
  fg_requests++;
  if (fg_delay_ms > 0)
    {
      g_mutex_unlock (&fg_lock);
      g_usleep ((gulong) fg_delay_ms * 1000); /* a slow connection */
      g_mutex_lock (&fg_lock);
    }
  const char *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *h = soup_server_message_get_request_headers (msg);
  if (g_strcmp0 (soup_message_headers_get_one (h, "Authorization"), "Bearer " FG_ACCESS) != 0)
    fg_bad_token = TRUE;
  SoupMessageBody *body = soup_server_message_get_request_body (msg);
  const char *data = body->data != NULL ? body->data : "";
  if (fg_fail_auth)
    {
      fg_reply_json (msg, 401, "{\"error\":{\"code\":401,\"message\":\"Invalid Credentials\"}}");
      g_mutex_unlock (&fg_lock);
      return;
    }
  g_auto (GStrv) seg = g_strsplit (path, "/", -1); /* "", drive|upload, ... */
  guint n = g_strv_length (seg);
  gboolean upload = n >= 2 && strcmp (seg[1], "upload") == 0;
  const char *id = upload ? (n >= 6 ? seg[5] : NULL) : (n >= 5 ? seg[4] : NULL);
  FgFile *f = id != NULL ? g_hash_table_lookup (fg_files, id) : NULL;
  const char *ctype = soup_message_headers_get_content_type (h, NULL);
  g_autofree char *full_type = g_strdup (soup_message_headers_get_one (h, "Content-Type"));

  if (strcmp (method, "GET") == 0 && !upload && n == 4 && query != NULL && g_hash_table_lookup (query, "q") != NULL)
    fg_list (msg, g_hash_table_lookup (query, "q"));
  else if (strcmp (method, "GET") == 0 && f != NULL && n == 5)
    fg_reply_file (msg, f);
  else if (strcmp (method, "GET") == 0 && f != NULL && n == 6 && strcmp (seg[5], "export") == 0)
    {
      g_autofree char *out = fg_google_style (f->content != NULL ? f->content : "");
      if (fg_lossy_id != NULL && strcmp (f->id, fg_lossy_id) == 0)
        {
          char *last = g_strrstr (out, "\n\n* ");
          if (last != NULL)
            *last = '\0';
        }
      soup_server_message_set_status (msg, 200, NULL);
      soup_server_message_set_response (msg, "text/markdown", SOUP_MEMORY_COPY, out, strlen (out));
    }
  else if (strcmp (method, "GET") == 0 && f != NULL && n == 6 && strcmp (seg[5], "permissions") == 0)
    fg_reply_json (msg, 200, f->shared ? "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"},{\"type\":\"anyone\","
                                      "\"role\":\"reader\"}]}"
                                    : "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"}]}");
  else if (strcmp (method, "POST") == 0 && !upload)
    {
      /* A folder: JSON metadata only. */
      JsonParser *p;
      JsonObject *o = jn_json_object (data, (gssize) body->length, &p);
      JsonArray *parents = jn_json_arr (o, "parents");
      FgFile *nf = fg_add_file (jn_json_str (o, "name"), jn_json_str (o, "mimeType"),
                           parents != NULL ? json_array_get_string_element (parents, 0) : "root", NULL);
      g_object_unref (p);
      fg_reply_file (msg, nf);
    }
  else if ((strcmp (method, "POST") == 0 || strcmp (method, "PATCH") == 0) && upload &&
           g_strcmp0 (g_hash_table_lookup (query, "uploadType"), "multipart") == 0 && ctype != NULL)
    {
      g_autofree char *meta = NULL;
      g_autofree char *media = fg_media_of (data, (gsize) body->length, full_type, &meta);
      JsonParser *p;
      JsonObject *o = jn_json_object (meta, -1, &p);
      const char *name = jn_json_str (o, "name");
      if (strcmp (method, "POST") == 0 && fg_full_name != NULL && g_strcmp0 (name, fg_full_name) == 0)
        fg_reply_json (msg, 403, "{\"error\":{\"code\":403,\"message\":\"The user's Drive storage quota has been "
                              "exceeded.\",\"errors\":[{\"reason\":\"storageQuotaExceeded\"}]}}");
      else if (strcmp (method, "POST") == 0)
        {
          JsonArray *parents = jn_json_arr (o, "parents");
          FgFile *nf = fg_add_file (name, jn_json_str (o, "mimeType"),
                               parents != NULL ? json_array_get_string_element (parents, 0) : "root", media);
          fg_reply_file (msg, nf);
        }
      else if (f != NULL && !f->trashed)
        {
          g_free (f->content);
          f->content = g_strdup (media);
          if (name != NULL)
            {
              g_free (f->name);
              f->name = g_strdup (name);
            }
          fg_reply_file (msg, f);
        }
      else
        fg_reply_json (msg, 404, "{\"error\":{\"code\":404,\"message\":\"File not found.\"}}");
      g_object_unref (p);
    }
  else if (strcmp (method, "PATCH") == 0 && !upload && f != NULL)
    {
      JsonParser *p;
      JsonObject *o = jn_json_object (data, (gssize) body->length, &p);
      if (jn_json_str (o, "name") != NULL)
        {
          g_free (f->name);
          f->name = g_strdup (jn_json_str (o, "name"));
        }
      g_object_unref (p);
      fg_reply_file (msg, f);
    }
  else
    fg_reply_json (msg, 404, "{\"error\":{\"code\":404,\"message\":\"File not found.\"}}");
  g_mutex_unlock (&fg_lock);
}

static gpointer
fg_server_thread (gpointer data)
{
  GMainContext *ctx = data;
  g_main_context_push_thread_default (ctx);
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, fg_serve, NULL, NULL);
  g_assert_true (soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris (server);
  char *b = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  g_atomic_pointer_set (&fg_base, b);
  g_main_loop_run (g_main_loop_new (ctx, FALSE));
  return NULL;
}

int
fg_port (void)
{
  g_autoptr (GUri) u = g_uri_parse (fg_base, G_URI_FLAGS_NONE, NULL);
  return g_uri_get_port (u);
}

FgFile *
fg_file (const char *id)
{
  return id != NULL ? g_hash_table_lookup (fg_files, id) : NULL;
}

/* Deleting a folder deletes what is in it, as in Drive. */
void
fg_delete_tree (const char *id)
{
  GPtrArray *children = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, fg_files);
  while (g_hash_table_iter_next (&it, &key, &value))
    if (g_strcmp0 (((FgFile *) value)->parent, id) == 0)
      g_ptr_array_add (children, g_strdup (key));
  for (guint i = 0; i < children->len; i++)
    fg_delete_tree (children->pdata[i]);
  g_ptr_array_unref (children);
  g_hash_table_remove (fg_files, id);
}

int
fg_count (void)
{
  return (int) g_hash_table_size (fg_files);
}


void
fg_start (void)
{
  fg_files = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, fg_file_free);
  g_thread_new ("fake-google", fg_server_thread, g_main_context_new ());
  while (g_atomic_pointer_get (&fg_base) == NULL)
    g_usleep (1000);
}
