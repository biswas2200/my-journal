/* The real Drive helper (journal-drive) against a fake Google Drive on
 * this machine.
 *
 * The fake keeps files in memory as Drive does: ids, names, parents, a
 * bin, Docs made from uploaded Markdown and read back as Markdown in
 * Google's own style ("*" bullets, escaped brackets), and permissions.
 * Faults can be switched on: a full Drive, an expired sign-in, a Doc
 * that reads back with a line missing, a Doc shared with someone.
 *
 * Checked: the first backup makes My Drive/Notes/<subject>/<topic>; the
 * next one updates the same Docs in place (no new files) and follows a
 * renamed topic or subject; a Doc or folder you deleted (or put in the
 * bin) is made again where it belongs; a Doc that does not read back the
 * same is reported, not called backed up; a shared one is flagged; a
 * full Drive fails that topic only, in plain words; an expired sign-in
 * stops the backup with one plain message; the token only ever travels
 * as the Authorization header; a port Notes did not allow is never
 * reached. */
#include <string.h>
#include <libsoup/soup.h>
#include "cases.h"
#include "driveproto.h"
#include "jsonsafe.h"

static const char *helper;
static char *base;
static GMutex lock;

/* ---- the fake Drive ---------------------------------------------------------- */

typedef struct {
  char    *id, *name, *mime, *parent, *content;
  gboolean trashed, shared;
} File;

static GHashTable *files;    /* id -> File */
static int next_id;
static int requests;         /* HTTP requests seen */
static gboolean fail_auth;   /* every request: 401 */
static char *full_name;      /* creating a Doc with this name: storage full */
static char *lossy_id;       /* exporting this Doc drops its last line */
static gboolean bad_token;   /* a request came without the right token */

static void
file_free (gpointer p)
{
  File *f = p;
  g_free (f->id);
  g_free (f->name);
  g_free (f->mime);
  g_free (f->parent);
  g_free (f->content);
  g_free (f);
}

static void
reply_json (SoupServerMessage *msg, guint status, const char *json)
{
  soup_server_message_set_status (msg, status, NULL);
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, json, strlen (json));
}

static void
reply_file (SoupServerMessage *msg, const File *f)
{
  g_autofree char *json = g_strdup_printf ("{\"id\":\"%s\",\"name\":\"%s\",\"trashed\":%s,"
                                           "\"webViewLink\":\"https://docs.google.com/document/d/%s/edit\"}",
                                           f->id, f->name, f->trashed ? "true" : "false", f->id);
  reply_json (msg, 200, json);
}

/* Markdown as Google writes it back: "*" bullets, escaped brackets. */
static char *
google_style (const char *md)
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
media_of (const char *body, gsize len, const char *content_type, char **metadata)
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

static File *
add_file (const char *name, const char *mime, const char *parent, const char *content)
{
  File *f = g_new0 (File, 1);
  f->id = g_strdup_printf ("id%d", ++next_id);
  f->name = g_strdup (name);
  f->mime = g_strdup (mime);
  f->parent = g_strdup (parent);
  f->content = g_strdup (content);
  g_hash_table_insert (files, f->id, f);
  return f;
}

static void
serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user)
{
  (void) server; (void) user;
  g_mutex_lock (&lock);
  requests++;
  const char *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *h = soup_server_message_get_request_headers (msg);
  if (g_strcmp0 (soup_message_headers_get_one (h, "Authorization"), "Bearer ya29.test-token") != 0)
    bad_token = TRUE;
  SoupMessageBody *body = soup_server_message_get_request_body (msg);
  const char *data = body->data != NULL ? body->data : "";
  if (fail_auth)
    {
      reply_json (msg, 401, "{\"error\":{\"code\":401,\"message\":\"Invalid Credentials\"}}");
      g_mutex_unlock (&lock);
      return;
    }
  g_auto (GStrv) seg = g_strsplit (path, "/", -1); /* "", drive|upload, ... */
  guint n = g_strv_length (seg);
  gboolean upload = n >= 2 && strcmp (seg[1], "upload") == 0;
  const char *id = upload ? (n >= 6 ? seg[5] : NULL) : (n >= 5 ? seg[4] : NULL);
  File *f = id != NULL ? g_hash_table_lookup (files, id) : NULL;
  const char *ctype = soup_message_headers_get_content_type (h, NULL);
  g_autofree char *full_type = g_strdup (soup_message_headers_get_one (h, "Content-Type"));

  if (strcmp (method, "GET") == 0 && f != NULL && n == 5)
    reply_file (msg, f);
  else if (strcmp (method, "GET") == 0 && f != NULL && n == 6 && strcmp (seg[5], "export") == 0)
    {
      g_autofree char *out = google_style (f->content != NULL ? f->content : "");
      if (lossy_id != NULL && strcmp (f->id, lossy_id) == 0)
        {
          char *last = g_strrstr (out, "\n\n* ");
          if (last != NULL)
            *last = '\0';
        }
      soup_server_message_set_status (msg, 200, NULL);
      soup_server_message_set_response (msg, "text/markdown", SOUP_MEMORY_COPY, out, strlen (out));
    }
  else if (strcmp (method, "GET") == 0 && f != NULL && n == 6 && strcmp (seg[5], "permissions") == 0)
    reply_json (msg, 200, f->shared ? "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"},{\"type\":\"anyone\","
                                      "\"role\":\"reader\"}]}"
                                    : "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"}]}");
  else if (strcmp (method, "POST") == 0 && !upload)
    {
      /* A folder: JSON metadata only. */
      JsonParser *p;
      JsonObject *o = jn_json_object (data, (gssize) body->length, &p);
      JsonArray *parents = jn_json_arr (o, "parents");
      File *nf = add_file (jn_json_str (o, "name"), jn_json_str (o, "mimeType"),
                           parents != NULL ? json_array_get_string_element (parents, 0) : "root", NULL);
      g_object_unref (p);
      reply_file (msg, nf);
    }
  else if ((strcmp (method, "POST") == 0 || strcmp (method, "PATCH") == 0) && upload &&
           g_strcmp0 (g_hash_table_lookup (query, "uploadType"), "multipart") == 0 && ctype != NULL)
    {
      g_autofree char *meta = NULL;
      g_autofree char *media = media_of (data, (gsize) body->length, full_type, &meta);
      JsonParser *p;
      JsonObject *o = jn_json_object (meta, -1, &p);
      const char *name = jn_json_str (o, "name");
      if (strcmp (method, "POST") == 0 && full_name != NULL && g_strcmp0 (name, full_name) == 0)
        reply_json (msg, 403, "{\"error\":{\"code\":403,\"message\":\"The user's Drive storage quota has been "
                              "exceeded.\",\"errors\":[{\"reason\":\"storageQuotaExceeded\"}]}}");
      else if (strcmp (method, "POST") == 0)
        {
          JsonArray *parents = jn_json_arr (o, "parents");
          File *nf = add_file (name, jn_json_str (o, "mimeType"),
                               parents != NULL ? json_array_get_string_element (parents, 0) : "root", media);
          reply_file (msg, nf);
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
          reply_file (msg, f);
        }
      else
        reply_json (msg, 404, "{\"error\":{\"code\":404,\"message\":\"File not found.\"}}");
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
      reply_file (msg, f);
    }
  else
    reply_json (msg, 404, "{\"error\":{\"code\":404,\"message\":\"File not found.\"}}");
  g_mutex_unlock (&lock);
}

static gpointer
server_thread (gpointer data)
{
  GMainContext *ctx = data;
  g_main_context_push_thread_default (ctx);
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, serve, NULL, NULL);
  g_assert_true (soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris (server);
  char *b = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  g_atomic_pointer_set (&base, b);
  g_main_loop_run (g_main_loop_new (ctx, FALSE));
  return NULL;
}

static int
port (void)
{
  g_autoptr (GUri) u = g_uri_parse (base, G_URI_FLAGS_NONE, NULL);
  return g_uri_get_port (u);
}

/* ---- running the helper ----------------------------------------------------- */

typedef struct {
  const char *subject, *name, *markdown;
  char *doc, *folder; /* carried from one backup to the next, as Notes does */
} Topic;

static char *root;

static JnDriveAnswer
backup (Topic *topics, gsize n, int allowed_port)
{
  JnDriveItem *items = g_new0 (JnDriveItem, MAX (n, 1));
  for (gsize i = 0; i < n; i++)
    items[i] = (JnDriveItem){ (gint64) i + 1, (char *) topics[i].subject, (char *) topics[i].name,
                              (char *) topics[i].markdown, topics[i].doc, topics[i].folder };
  JnDriveRequest r = { base, (char *) "ya29.test-token", { (guint16) allowed_port }, 1, root, items, n };
  g_autofree char *json = jn_drive_request_to_json (&r);
  g_free (items);
  const char *argv[] = { helper, NULL };
  g_autoptr (GSubprocess) p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                                                 NULL);
  g_assert_nonnull (p);
  g_autofree char *out = NULL;
  g_autoptr (GError) error = NULL;
  g_assert_true (g_subprocess_communicate_utf8 (p, json, NULL, &out, NULL, &error));
  JnDriveAnswer a = { 0 };
  g_assert_true (jn_drive_answer_from_json (out, -1, &a, NULL));
  if (a.root != NULL)
    {
      g_free (root);
      root = g_strdup (a.root);
    }
  for (gsize i = 0; i < a.n_results; i++)
    {
      Topic *t = &topics[a.results[i].topic - 1];
      if (a.results[i].doc != NULL)
        {
          g_free (t->doc);
          t->doc = g_strdup (a.results[i].doc);
        }
      if (a.results[i].folder != NULL)
        {
          g_free (t->folder);
          t->folder = g_strdup (a.results[i].folder);
        }
    }
  return a;
}

static File *
file (const char *id)
{
  return id != NULL ? g_hash_table_lookup (files, id) : NULL;
}

/* Deleting a folder deletes what is in it, as in Drive. */
static void
delete_tree (const char *id)
{
  GPtrArray *children = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, files);
  while (g_hash_table_iter_next (&it, &key, &value))
    if (g_strcmp0 (((File *) value)->parent, id) == 0)
      g_ptr_array_add (children, g_strdup (key));
  for (guint i = 0; i < children->len; i++)
    delete_tree (children->pdata[i]);
  g_ptr_array_unref (children);
  g_hash_table_remove (files, id);
}

static int
count_files (void)
{
  return (int) g_hash_table_size (files);
}

static void
test_backups (void)
{
  Topic t[] = {
    { "DSA", "Prefix sum", "# Prefix sum\n\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]", NULL, NULL },
    { "DSA", "Heaps", "# Heaps\n\n- sift down", NULL, NULL },
    { "OS", "Paging", "# Paging\n\n- page table", NULL, NULL },
  };
  JrCases c = { "drive helper", 0, 0 };

  /* First backup: My Drive/Notes/<subject>/<topic>. */
  JnDriveAnswer a = backup (t, 3, port ());
  gboolean all_ok = a.error == NULL && a.n_results == 3;
  for (gsize i = 0; all_ok && i < 3; i++)
    all_ok = a.results[i].verified && !a.results[i].shared && a.results[i].error == NULL &&
             g_str_has_prefix (a.results[i].link, "https://docs.google.com/");
  jr_case (&c, "first backup: all verified", all_ok, "error=%s n=%zu", a.error, a.n_results);
  File *r = file (root);
  jr_case (&c, "a Notes folder in My Drive", r != NULL && g_strcmp0 (r->name, "Notes") == 0 &&
           g_strcmp0 (r->mime, JN_DRIVE_FOLDER_MIME) == 0 && g_strcmp0 (r->parent, "root") == 0, "root %s", root);
  File *dsa = file (t[0].folder), *os = file (t[2].folder);
  jr_case (&c, "one folder per subject, in Notes", dsa != NULL && os != NULL && dsa != os &&
           g_strcmp0 (t[1].folder, t[0].folder) == 0 && g_strcmp0 (dsa->name, "DSA") == 0 &&
           g_strcmp0 (dsa->parent, root) == 0 && g_strcmp0 (os->parent, root) == 0, "folders %s %s", t[0].folder,
           t[2].folder);
  File *doc = file (t[0].doc);
  jr_case (&c, "a Doc per topic in its folder", doc != NULL && g_strcmp0 (doc->name, "Prefix sum") == 0 &&
           g_strcmp0 (doc->mime, JN_DRIVE_DOC_MIME) == 0 && g_strcmp0 (doc->parent, t[0].folder) == 0 &&
           g_strcmp0 (doc->content, t[0].markdown) == 0, "doc %s", t[0].doc);
  jr_case (&c, "6 files in all", count_files () == 6, "Got %d", count_files ());
  jn_drive_answer_clear (&a);

  /* Next backup: the same Docs, updated in place; renames followed. */
  g_autofree char *first_doc = g_strdup (t[0].doc);
  t[0].markdown = "# Prefix sum\n\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n- O(1) per query";
  t[1].name = "Heaps (binary)";
  a = backup (t, 3, port ());
  jr_case (&c, "updated in place, no new files", count_files () == 6 && g_strcmp0 (t[0].doc, first_doc) == 0 &&
           g_strcmp0 (file (t[0].doc)->content, t[0].markdown) == 0, "files %d", count_files ());
  jr_case (&c, "a renamed topic renames its Doc", g_strcmp0 (file (t[1].doc)->name, "Heaps (binary)") == 0,
           "Got %s", file (t[1].doc)->name);
  jn_drive_answer_clear (&a);

  /* A renamed subject renames its folder. */
  t[2].subject = "Operating Systems";
  a = backup (t, 3, port ());
  jr_case (&c, "a renamed subject renames its folder", g_strcmp0 (file (t[2].folder)->name, "Operating Systems") == 0,
           "Got %s", file (t[2].folder)->name);
  jn_drive_answer_clear (&a);

  /* You put a Doc in the bin: it is made again in its folder. */
  file (t[1].doc)->trashed = TRUE;
  g_autofree char *binned = g_strdup (t[1].doc);
  a = backup (t, 3, port ());
  jr_case (&c, "a binned Doc made again", g_strcmp0 (t[1].doc, binned) != 0 && file (t[1].doc) != NULL &&
           g_strcmp0 (file (t[1].doc)->parent, t[1].folder) == 0 && a.results[1].verified, "doc %s", t[1].doc);
  jn_drive_answer_clear (&a);

  /* You deleted a subject's folder: the folder and its Docs are made again. */
  delete_tree (t[2].folder);
  g_autofree char *gone = g_strdup (t[2].folder);
  a = backup (t, 3, port ());
  jr_case (&c, "a deleted folder made again", g_strcmp0 (t[2].folder, gone) != 0 && file (t[2].folder) != NULL &&
           g_strcmp0 (file (t[2].doc)->parent, t[2].folder) == 0 && a.results[2].verified, "folder %s", t[2].folder);
  jn_drive_answer_clear (&a);

  /* You put a subject's folder in the bin: Drive shows what is in it as
   * in the bin too; both are made again. */
  file (t[0].folder)->trashed = TRUE;
  file (t[0].doc)->trashed = TRUE;
  file (t[1].doc)->trashed = TRUE;
  g_autofree char *binned_folder = g_strdup (t[0].folder);
  a = backup (t, 3, port ());
  jr_case (&c, "a binned folder made again, its Docs too", g_strcmp0 (t[0].folder, binned_folder) != 0 &&
           g_strcmp0 (t[1].folder, t[0].folder) == 0 && g_strcmp0 (file (t[0].doc)->parent, t[0].folder) == 0 &&
           g_strcmp0 (file (t[1].doc)->parent, t[0].folder) == 0 && a.results[0].verified && a.results[1].verified,
           "folder %s", t[0].folder);
  jn_drive_answer_clear (&a);

  /* You deleted the Notes folder itself. */
  delete_tree (root);
  g_autofree char *old_root = g_strdup (root);
  a = backup (t, 3, port ());
  jr_case (&c, "a deleted Notes folder made again", g_strcmp0 (root, old_root) != 0 && file (root) != NULL &&
           g_strcmp0 (file (t[0].folder)->parent, root) == 0, "root %s", root);
  jn_drive_answer_clear (&a);

  /* A Doc that reads back with a line missing: not called backed up. */
  g_mutex_lock (&lock);
  lossy_id = g_strdup (t[0].doc);
  g_mutex_unlock (&lock);
  a = backup (t, 3, port ());
  jr_case (&c, "a Doc that reads back different is reported", !a.results[0].verified && a.results[0].error != NULL &&
           strstr (a.results[0].error, "not the same") != NULL && a.results[1].verified, "Got %s", a.results[0].error);
  jn_drive_answer_clear (&a);
  g_clear_pointer (&lossy_id, g_free);

  /* Someone else can open a Doc: flagged. */
  file (t[2].doc)->shared = TRUE;
  a = backup (t, 3, port ());
  jr_case (&c, "a shared Doc is flagged", a.results[2].shared && !a.results[0].shared, "shared=%d",
           a.results[2].shared);
  jn_drive_answer_clear (&a);
  file (t[2].doc)->shared = FALSE;

  /* Drive is full: that topic fails in plain words, the others go on. */
  Topic more[] = { { "DSA", "Tries", "# Tries", NULL, NULL }, { "DSA", "Graphs", "# Graphs", NULL, NULL } };
  more[0].folder = g_strdup (t[0].folder);
  more[1].folder = g_strdup (t[0].folder);
  full_name = g_strdup ("Tries");
  a = backup (more, 2, port ());
  jr_case (&c, "Drive full: that topic only", a.error == NULL && a.results[0].error != NULL &&
           strstr (a.results[0].error, "full") != NULL && !a.results[0].verified && a.results[1].verified,
           "Got %s", a.results[0].error);
  jn_drive_answer_clear (&a);
  g_clear_pointer (&full_name, g_free);

  /* The sign-in expired: one plain message, nothing half done. */
  fail_auth = TRUE;
  a = backup (t, 3, port ());
  jr_case (&c, "sign-in expired", a.error != NULL && strstr (a.error, "Sign in") != NULL &&
           strstr (a.error, "ya29") == NULL, "Got %s", a.error);
  jn_drive_answer_clear (&a);
  fail_auth = FALSE;

  /* A port Notes did not allow: Drive is never reached. */
  int before = requests;
  a = backup (t, 3, port () == 443 ? 444 : 443);
  jr_case (&c, "a port not allowed is never reached", requests == before && a.error != NULL, "requests %d -> %d, %s",
           before, requests, a.error);
  jn_drive_answer_clear (&a);

  jr_case (&c, "the token only as the Authorization header", !bad_token, "a request came without it");
  jr_cases_done (&c);
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      g_free (t[i].doc);
      g_free (t[i].folder);
    }
  for (gsize i = 0; i < G_N_ELEMENTS (more); i++)
    {
      g_free (more[i].doc);
      g_free (more[i].folder);
    }
}

/* A broken request: a plain error, never a crash. */
static void
test_bad_request (void)
{
  const char *argv[] = { helper, NULL };
  g_autoptr (GSubprocess) p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                                                 NULL);
  g_autofree char *out = NULL;
  g_assert_true (g_subprocess_communicate_utf8 (p, "{\"nonsense\":1}", NULL, &out, NULL, NULL));
  JnDriveAnswer a = { 0 };
  g_assert_true (jn_drive_answer_from_json (out, -1, &a, NULL));
  g_assert_nonnull (a.error);
  g_assert_nonnull (strstr (a.error, "not valid"));
  jn_drive_answer_clear (&a);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (argc < 2)
    {
      g_printerr ("usage: test_drive_helper PATH-TO-journal-drive\n");
      return 1;
    }
  helper = argv[1];
  files = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, file_free);
  g_thread_new ("fake-drive", server_thread, g_main_context_new ());
  while (g_atomic_pointer_get (&base) == NULL)
    g_usleep (1000);
  g_test_add_func ("/drive-helper/backups", test_backups);
  g_test_add_func ("/drive-helper/bad-request", test_bad_request);
  return g_test_run ();
}
