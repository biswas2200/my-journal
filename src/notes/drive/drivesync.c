/* drivesync: see drivesync.h. */
#include "drivesync.h"

#include <string.h>

#define FILE_FIELDS "fields=id,trashed,webViewLink"

typedef struct {
  SoupSession          *session;
  const JnDriveRequest *req;
  char                 *auth;    /* "Bearer <token>", wiped at the end */
  char                 *fatal;   /* the run stopped: why */
  char                 *root;    /* the Notes folder, once known */
  GHashTable           *folders; /* old folder id or "name:<subject>" -> folder id */
} Sync;

/* One request to Drive. Returns the body (NULL only when Drive could not
 * be reached: then the run stops) and the HTTP status. */
static GBytes *
call (Sync *s, const char *method, const char *path, const char *content_type, GBytes *body, guint *status)
{
  *status = 0;
  if (s->fatal != NULL)
    return NULL;
  g_autofree char *url = g_strconcat (s->req->api_base, path, NULL);
  SoupMessage *msg = soup_message_new (method, url);
  if (msg == NULL)
    {
      s->fatal = g_strdup ("the Google Drive address is not valid");
      return NULL;
    }
  soup_message_headers_replace (soup_message_get_request_headers (msg), "Authorization", s->auth);
  if (body != NULL)
    soup_message_set_request_body_from_bytes (msg, content_type, body);
  g_autoptr (GError) error = NULL;
  GBytes *reply = soup_session_send_and_read (s->session, msg, NULL, &error);
  *status = soup_message_get_status (msg);
  g_object_unref (msg);
  if (reply == NULL)
    s->fatal = g_strdup_printf ("Google Drive could not be reached (%s). Your notes are safe on this laptop; the "
                                "backup will be tried again.", error->message);
  else if (*status == 401)
    {
      gsize n = 0;
      const char *data = g_bytes_get_data (reply, &n);
      s->fatal = jn_drive_error (data, (gssize) n, 401);
    }
  return reply;
}

static gboolean
ok (guint status)
{
  return status >= 200 && status < 300;
}

/* Drive's error for a failed call, in plain words. */
static char *
error_of (GBytes *reply, guint status)
{
  gsize n = 0;
  const char *data = reply != NULL ? g_bytes_get_data (reply, &n) : "";
  return jn_drive_error (data, (gssize) n, (int) status);
}

/* A file's id from Drive's answer, unless it is in the bin. */
static char *
live_id (GBytes *reply, guint status, char **link)
{
  if (reply == NULL || !ok (status))
    return NULL;
  gsize n = 0;
  const char *data = g_bytes_get_data (reply, &n);
  char *id = NULL;
  gboolean trashed = FALSE;
  if (!jn_drive_parse_file (data, (gssize) n, &id, &trashed, link) || trashed)
    {
      g_clear_pointer (&id, g_free);
      if (link != NULL)
        g_clear_pointer (link, g_free);
    }
  return id;
}

/* A file Notes made earlier: `name` of type `mime` in `parent` ("root":
 * My Drive), not in the bin; NULL if there is none. Asked before making
 * anything, so a Doc whose record Notes lost (a backup stopped just after
 * Drive made it, an old notes.db restored) is found, not made twice. */
static char *
find (Sync *s, const char *name, const char *mime, const char *parent, char **link)
{
  g_autofree char *q = jn_drive_find_query (name, mime, parent);
  g_autofree char *path = g_strdup_printf ("drive/v3/files?q=%s&fields=files(id,webViewLink)&pageSize=1", q);
  guint status;
  g_autoptr (GBytes) reply = call (s, "GET", path, NULL, NULL, &status);
  if (reply == NULL || !ok (status))
    return NULL;
  gsize n = 0;
  const char *data = g_bytes_get_data (reply, &n);
  char *id = NULL;
  jn_drive_parse_first (data, (gssize) n, &id, link);
  return id;
}

/* A folder named `name` in `parent` (NULL: My Drive itself): the one Notes
 * made earlier, or a new one. */
static char *
make_folder (Sync *s, const char *name, const char *parent, char **error)
{
  char *found = find (s, name, JN_DRIVE_FOLDER_MIME, parent != NULL ? parent : "root", NULL);
  if (found != NULL || s->fatal != NULL)
    return found;
  g_autofree char *meta = jn_drive_metadata (name, JN_DRIVE_FOLDER_MIME, parent);
  g_autoptr (GBytes) body = g_bytes_new (meta, strlen (meta));
  guint status;
  g_autoptr (GBytes) reply = call (s, "POST", "drive/v3/files?" FILE_FIELDS, "application/json", body, &status);
  char *id = live_id (reply, status, NULL);
  if (id == NULL && s->fatal == NULL)
    *error = error_of (reply, status);
  return id;
}

/* The Notes folder: the one from before, or a new one. */
static const char *
root (Sync *s, char **error)
{
  if (s->root != NULL)
    return s->root;
  if (s->req->root != NULL)
    {
      g_autofree char *path = g_strdup_printf ("drive/v3/files/%s?" FILE_FIELDS, s->req->root);
      guint status;
      g_autoptr (GBytes) reply = call (s, "GET", path, NULL, NULL, &status);
      s->root = live_id (reply, status, NULL);
    }
  if (s->root == NULL && s->fatal == NULL)
    s->root = make_folder (s, JN_DRIVE_ROOT_NAME, NULL, error);
  return s->root;
}

/* The subject's folder: the one from before (renamed to the subject's
 * name now), or a new one in the Notes folder; once per subject per run. */
static const char *
folder (Sync *s, const JnDriveItem *it, char **error)
{
  g_autofree char *by_name = g_strconcat ("name:", it->subject, NULL);
  const char *known = it->folder != NULL ? g_hash_table_lookup (s->folders, it->folder) : NULL;
  if (known == NULL)
    known = g_hash_table_lookup (s->folders, by_name);
  if (known != NULL)
    return known;
  char *id = NULL;
  if (it->folder != NULL)
    {
      g_autofree char *meta = jn_drive_metadata (it->subject, NULL, NULL);
      g_autoptr (GBytes) body = g_bytes_new (meta, strlen (meta));
      g_autofree char *path = g_strdup_printf ("drive/v3/files/%s?" FILE_FIELDS, it->folder);
      guint status;
      g_autoptr (GBytes) reply = call (s, "PATCH", path, "application/json", body, &status);
      id = live_id (reply, status, NULL);
    }
  if (id == NULL && s->fatal == NULL)
    {
      const char *parent = root (s, error);
      if (parent != NULL)
        id = make_folder (s, it->subject, parent, error);
    }
  if (id == NULL)
    return NULL;
  if (it->folder != NULL)
    g_hash_table_insert (s->folders, g_strdup (it->folder), g_strdup (id));
  g_hash_table_insert (s->folders, g_steal_pointer (&by_name), id);
  return id;
}

/* Replaces the text and name of Doc `doc` in place. Its id, or NULL: gone
 * or in the bin (*refused stays FALSE: make it again), or Drive refused
 * (*refused TRUE, *error says why). */
static char *
update_doc (Sync *s, const JnDriveItem *it, const char *doc, char **link, gboolean *refused, char **error)
{
  g_autofree char *meta = jn_drive_metadata (it->name, NULL, NULL);
  g_autofree char *type = NULL;
  g_autoptr (GBytes) body = jn_drive_multipart (meta, it->markdown, &type);
  g_autofree char *path = g_strdup_printf ("upload/drive/v3/files/%s?uploadType=multipart&" FILE_FIELDS, doc);
  guint status;
  g_autoptr (GBytes) reply = call (s, "PATCH", path, type, body, &status);
  char *id = live_id (reply, status, link);
  if (id == NULL && s->fatal == NULL && status != 404 && !ok (status))
    {
      *refused = TRUE;
      *error = error_of (reply, status);
    }
  return id;
}

/* The topic's Doc: the one from before, replaced in place; else one Notes
 * made earlier without hearing back (found by name), replaced in place;
 * else a new Doc in `parent`. */
static char *
put_doc (Sync *s, const JnDriveItem *it, const char *parent, char **link, char **error)
{
  gboolean refused = FALSE;
  char *id = it->doc != NULL ? update_doc (s, it, it->doc, link, &refused, error) : NULL;
  if (id == NULL && !refused && s->fatal == NULL)
    {
      g_autofree char *found = find (s, it->name, JN_DRIVE_DOC_MIME, parent, NULL);
      if (found != NULL)
        id = update_doc (s, it, found, link, &refused, error);
    }
  if (id == NULL && !refused && s->fatal == NULL)
    {
      g_autofree char *meta = jn_drive_metadata (it->name, JN_DRIVE_DOC_MIME, parent);
      g_autofree char *type = NULL;
      g_autoptr (GBytes) body = jn_drive_multipart (meta, it->markdown, &type);
      guint status;
      g_autoptr (GBytes) reply = call (s, "POST", "upload/drive/v3/files?uploadType=multipart&" FILE_FIELDS, type,
                                       body, &status);
      id = live_id (reply, status, link);
      if (id == NULL && s->fatal == NULL)
        *error = error_of (reply, status);
    }
  return id;
}

/* Read back as Markdown: the same text as the note? */
static gboolean
same_on_drive (Sync *s, const JnDriveItem *it, const char *doc, char **error)
{
  g_autofree char *path = g_strdup_printf ("drive/v3/files/%s/export?mimeType=text/markdown", doc);
  guint status;
  g_autoptr (GBytes) reply = call (s, "GET", path, NULL, NULL, &status);
  if (reply == NULL)
    return FALSE;
  if (!ok (status))
    {
      *error = error_of (reply, status);
      return FALSE;
    }
  gsize n = 0;
  const char *data = g_bytes_get_data (reply, &n);
  g_autofree char *text = g_strndup (data, n);
  if (!jn_drive_same_text (it->markdown, text))
    {
      *error = g_strdup ("The Doc on Google Drive did not read back as the same text as your note (not the same), "
                         "so it is not counted as backed up; it will be tried again.");
      return FALSE;
    }
  return TRUE;
}

/* Can anyone besides you open it? FALSE also when that cannot be read. */
static gboolean
shared (Sync *s, const char *doc)
{
  g_autofree char *path = g_strdup_printf ("drive/v3/files/%s/permissions?fields=permissions(type,role)", doc);
  guint status;
  g_autoptr (GBytes) reply = call (s, "GET", path, NULL, NULL, &status);
  gboolean out = FALSE;
  if (reply != NULL && ok (status))
    {
      gsize n = 0;
      const char *data = g_bytes_get_data (reply, &n);
      jn_drive_parse_permissions (data, (gssize) n, &out);
    }
  return out;
}

JnDriveAnswer
jn_drive_sync (SoupSession *session, const JnDriveRequest *request, JnDriveEach each, gpointer user)
{
  Sync s = { session, request, g_strconcat ("Bearer ", request->token, NULL), NULL, NULL,
             g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free) };
  JnDriveAnswer a = { 0 };
  a.results = g_new0 (JnDriveResult, MAX (request->n_items, 1));
  for (gsize i = 0; i < request->n_items && s.fatal == NULL; i++)
    {
      const JnDriveItem *it = &request->items[i];
      JnDriveResult *r = &a.results[a.n_results++];
      r->topic = it->topic;
      const char *parent = folder (&s, it, &r->error);
      if (parent != NULL)
        {
          r->folder = g_strdup (parent);
          r->doc = put_doc (&s, it, parent, &r->link, &r->error);
        }
      if (r->doc != NULL)
        {
          r->verified = same_on_drive (&s, it, r->doc, &r->error);
          r->shared = shared (&s, r->doc);
        }
      /* A Doc made just before the run stopped is still reported (not
       * verified), so the next backup replaces it instead of making a
       * second one. */
      if (s.fatal != NULL)
        r->verified = FALSE;
      if (each != NULL)
        each (r, s.root, user);
    }
  a.error = g_steal_pointer (&s.fatal);
  a.root = g_steal_pointer (&s.root);
  memset (s.auth, 0, strlen (s.auth));
  g_free (s.auth);
  g_hash_table_unref (s.folders);
  return a;
}
