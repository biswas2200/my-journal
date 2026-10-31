/* driveproto: see driveproto.h. */
#include "driveproto.h"

#include <string.h>
#include <gio/gio.h>
#include "jsonsafe.h"
#include "markdown.h"

#define SIGN_IN_AGAIN "Sign in to Google again in Notes."

/* ---- small builders ------------------------------------------------------- */

static void
add_string (JsonBuilder *b, const char *name, const char *value)
{
  if (value == NULL)
    return;
  json_builder_set_member_name (b, name);
  json_builder_add_string_value (b, value);
}

static void
add_ports (JsonBuilder *b, const guint16 *ports, gsize n)
{
  json_builder_set_member_name (b, "ports");
  json_builder_begin_array (b);
  for (gsize i = 0; i < n; i++)
    json_builder_add_int_value (b, ports[i]);
  json_builder_end_array (b);
}

/* ---- Notes -> helper -------------------------------------------------------- */

char *
jn_drive_request_to_json (const JnDriveRequest *r)
{
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  add_string (b, "api_base", r->api_base);
  add_string (b, "token", r->token);
  add_ports (b, r->ports, r->n_ports);
  add_string (b, "root", r->root);
  json_builder_set_member_name (b, "items");
  json_builder_begin_array (b);
  for (gsize i = 0; i < r->n_items; i++)
    {
      const JnDriveItem *it = &r->items[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "topic");
      json_builder_add_int_value (b, it->topic);
      add_string (b, "subject", it->subject);
      add_string (b, "name", it->name);
      add_string (b, "markdown", it->markdown);
      add_string (b, "doc", it->doc);
      add_string (b, "folder", it->folder);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
  return jn_json_to_string (b);
}

/* An http(s) address (Google's, or a test server). */
static gboolean
is_web (const char *s)
{
  if (s == NULL)
    return FALSE;
  if (g_str_has_prefix (s, "https://"))
    return TRUE;
  return g_str_has_prefix (s, "http://");
}

static gboolean
refuse (GError **error, JnDriveRequest *out, JsonParser *p)
{
  jn_drive_request_clear (out);
  g_object_unref (p);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the request to the Drive helper is not valid");
  return FALSE;
}

gboolean
jn_drive_request_from_json (const char *json, gssize len, JnDriveRequest *out, GError **error)
{
  *out = (JnDriveRequest){ 0 };
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  const char *base = jn_json_str (o, "api_base"), *token = jn_json_str (o, "token");
  JsonArray *ports = jn_json_arr (o, "ports"), *items = jn_json_arr (o, "items");
  gboolean web = is_web (base);
  if (!web || token == NULL || ports == NULL || items == NULL)
    return refuse (error, out, p);
  guint np = json_array_get_length (ports), ni = json_array_get_length (items);
  if (np < 1 || np > JN_DRIVE_MAX_PORTS || ni > JN_DRIVE_MAX_ITEMS)
    return refuse (error, out, p);
  for (guint i = 0; i < np; i++)
    {
      JsonNode *n = json_array_get_element (ports, i);
      gint64 v = JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_INT64 ? json_node_get_int (n) : 0;
      if (v < 1 || v > 65535)
        return refuse (error, out, p);
      out->ports[i] = (guint16) v;
    }
  out->n_ports = np;
  out->items = g_new0 (JnDriveItem, MAX (ni, 1));
  for (guint i = 0; i < ni; i++)
    {
      JsonNode *n = json_array_get_element (items, i);
      JsonObject *it = JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
      gint64 topic = 0;
      const char *subject = jn_json_str (it, "subject"), *name = jn_json_str (it, "name");
      const char *md = jn_json_str (it, "markdown");
      if (!jn_json_int (it, "topic", &topic) || topic < 1 || subject == NULL || name == NULL || md == NULL)
        return refuse (error, out, p);
      JnDriveItem *d = &out->items[out->n_items++];
      d->topic = topic;
      d->subject = g_strdup (subject);
      d->name = g_strdup (name);
      d->markdown = g_strdup (md);
      d->doc = g_strdup (jn_json_str (it, "doc"));
      d->folder = g_strdup (jn_json_str (it, "folder"));
    }
  out->api_base = g_strdup (base);
  out->token = g_strdup (token);
  out->root = g_strdup (jn_json_str (o, "root"));
  g_object_unref (p);
  return TRUE;
}

void
jn_drive_request_clear (JnDriveRequest *r)
{
  if (r->token != NULL)
    memset (r->token, 0, strlen (r->token)); /* a live access token */
  g_free (r->token);
  g_free (r->api_base);
  g_free (r->root);
  for (gsize i = 0; i < r->n_items; i++)
    {
      g_free (r->items[i].subject);
      g_free (r->items[i].name);
      g_free (r->items[i].markdown);
      g_free (r->items[i].doc);
      g_free (r->items[i].folder);
    }
  g_free (r->items);
  *r = (JnDriveRequest){ 0 };
}

/* ---- helper -> Notes -------------------------------------------------------- */

char *
jn_drive_answer_to_json (const JnDriveAnswer *a)
{
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  add_string (b, "root", a->root);
  add_string (b, "error", a->error);
  json_builder_set_member_name (b, "results");
  json_builder_begin_array (b);
  for (gsize i = 0; i < a->n_results; i++)
    {
      const JnDriveResult *r = &a->results[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "topic");
      json_builder_add_int_value (b, r->topic);
      add_string (b, "doc", r->doc);
      add_string (b, "folder", r->folder);
      add_string (b, "link", r->link);
      json_builder_set_member_name (b, "verified");
      json_builder_add_boolean_value (b, r->verified);
      json_builder_set_member_name (b, "shared");
      json_builder_add_boolean_value (b, r->shared);
      add_string (b, "error", r->error);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
  return jn_json_to_string (b);
}

gboolean
jn_drive_answer_from_json (const char *json, gssize len, JnDriveAnswer *out, GError **error)
{
  *out = (JnDriveAnswer){ 0 };
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  JsonArray *results = jn_json_arr (o, "results");
  if (results == NULL)
    {
      g_object_unref (p);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the Drive helper gave no answer");
      return FALSE;
    }
  out->root = g_strdup (jn_json_str (o, "root"));
  out->error = g_strdup (jn_json_str (o, "error"));
  guint n = json_array_get_length (results);
  out->results = g_new0 (JnDriveResult, MAX (n, 1));
  for (guint i = 0; i < n; i++)
    {
      JsonNode *node = json_array_get_element (results, i);
      JsonObject *r = JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
      gint64 topic = 0;
      if (!jn_json_int (r, "topic", &topic))
        continue;
      JnDriveResult *d = &out->results[out->n_results++];
      d->topic = topic;
      d->doc = g_strdup (jn_json_str (r, "doc"));
      d->folder = g_strdup (jn_json_str (r, "folder"));
      d->link = g_strdup (jn_json_str (r, "link"));
      d->verified = jn_json_bool (r, "verified");
      d->shared = jn_json_bool (r, "shared");
      d->error = g_strdup (jn_json_str (r, "error"));
    }
  g_object_unref (p);
  return TRUE;
}

void
jn_drive_answer_clear (JnDriveAnswer *a)
{
  g_free (a->root);
  g_free (a->error);
  for (gsize i = 0; i < a->n_results; i++)
    {
      g_free (a->results[i].doc);
      g_free (a->results[i].folder);
      g_free (a->results[i].link);
      g_free (a->results[i].error);
    }
  g_free (a->results);
  *a = (JnDriveAnswer){ 0 };
}

/* ---- Drive's own messages ------------------------------------------------------ */

char *
jn_drive_metadata (const char *name, const char *mime, const char *parent)
{
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  add_string (b, "name", name);
  add_string (b, "mimeType", mime);
  if (parent != NULL)
    {
      json_builder_set_member_name (b, "parents");
      json_builder_begin_array (b);
      json_builder_add_string_value (b, parent);
      json_builder_end_array (b);
    }
  json_builder_end_object (b);
  return jn_json_to_string (b);
}

GBytes *
jn_drive_multipart (const char *metadata, const char *markdown, char **content_type)
{
  /* A random boundary, made again in the (astronomically unlikely) case
   * that the text holds it. */
  g_autofree char *boundary = NULL;
  do
    {
      g_free (boundary);
      boundary = g_strdup_printf ("jn-%08x%08x%08x", g_random_int (), g_random_int (), g_random_int ());
    }
  while (strstr (markdown, boundary) != NULL || strstr (metadata, boundary) != NULL);
  GString *s = g_string_new (NULL);
  g_string_append_printf (s, "--%s\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n%s\r\n", boundary, metadata);
  g_string_append_printf (s, "--%s\r\nContent-Type: text/markdown; charset=UTF-8\r\n\r\n", boundary);
  g_string_append (s, markdown);
  g_string_append_printf (s, "\r\n--%s--\r\n", boundary);
  *content_type = g_strdup_printf ("multipart/related; boundary=%s", boundary);
  return g_string_free_to_bytes (s);
}

gboolean
jn_drive_parse_file (const char *json, gssize len, char **id, gboolean *trashed, char **link)
{
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  const char *i = jn_json_str (o, "id");
  if (i != NULL)
    {
      *id = g_strdup (i);
      *trashed = jn_json_bool (o, "trashed");
      if (link != NULL)
        *link = g_strdup (jn_json_str (o, "webViewLink"));
    }
  g_object_unref (p);
  return i != NULL;
}

gboolean
jn_drive_parse_permissions (const char *json, gssize len, gboolean *shared)
{
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  JsonArray *list = jn_json_arr (o, "permissions");
  if (list != NULL)
    {
      *shared = FALSE;
      for (guint i = 0; i < json_array_get_length (list); i++)
        {
          JsonNode *n = json_array_get_element (list, i);
          JsonObject *perm = JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
          const char *role = jn_json_str (perm, "role");
          /* The owner is you; any other role, for anyone, is sharing. */
          if (role != NULL && strcmp (role, "owner") != 0)
            *shared = TRUE;
        }
    }
  g_object_unref (p);
  return list != NULL;
}

char *
jn_drive_error (const char *json, gssize len, int status)
{
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  JsonObject *err = jn_json_obj (o, "error");
  const char *msg = jn_json_str (err, "message");
  JsonArray *errors = jn_json_arr (err, "errors");
  const char *reason = NULL;
  if (errors != NULL && json_array_get_length (errors) > 0 &&
      JSON_NODE_HOLDS_OBJECT (json_array_get_element (errors, 0)))
    reason = jn_json_str (json_array_get_object_element (errors, 0), "reason");
  char *out;
  if (status == 401)
    out = g_strdup ("Google did not accept the sign-in (it may have expired or been removed). " SIGN_IN_AGAIN);
  else if (g_strcmp0 (reason, "storageQuotaExceeded") == 0)
    out = g_strdup ("Your Google Drive is full, so the backup could not be saved. Free some space and back up again.");
  else if (status == 429 || g_strcmp0 (reason, "rateLimitExceeded") == 0 ||
           g_strcmp0 (reason, "userRateLimitExceeded") == 0)
    out = g_strdup ("Google Drive is busy; try again in a minute.");
  else if (status == 403)
    out = g_strdup_printf ("Google Drive refused: no permission for that (%s). " SIGN_IN_AGAIN,
                           msg != NULL ? msg : "HTTP 403");
  else if (msg != NULL)
    out = g_strdup_printf ("Google Drive refused: %s", msg);
  else
    out = g_strdup_printf ("Google Drive answered with HTTP %d.", status);
  g_object_unref (p);
  return out;
}

/* ---- checking the copy ---------------------------------------------------------- */

/* The words of a Markdown text, in order, one space between: what it
 * says, whatever markup says it. */
static char *
words (const char *markdown)
{
  GString *all = g_string_new (NULL);
  g_autoptr (GPtrArray) blocks = jn_md_blocks (markdown);
  for (guint i = 0; i < blocks->len; i++)
    {
      const JnMdBlock *b = blocks->pdata[i];
      if (b->text == NULL)
        continue;
      if (b->kind == JN_MD_CODE || b->kind == JN_MD_SVG)
        {
          g_string_append_c (all, ' ');
          g_string_append (all, b->text);
          continue;
        }
      g_autoptr (GPtrArray) spans = jn_md_spans (b->text);
      for (guint k = 0; k < spans->len; k++)
        {
          const JnMdSpan *s = spans->pdata[k];
          g_string_append_c (all, ' ');
          if (s->kind == JN_SPAN_CITE)
            g_string_append_printf (all, "[%s]", s->text);
          else
            g_string_append (all, s->text);
        }
    }
  /* Collapse every run of white space into one space. */
  g_auto (GStrv) parts = g_strsplit_set (all->str, " \t\n\r", -1);
  g_string_free (all, TRUE);
  GString *out = g_string_new (NULL);
  for (char **w = parts; *w != NULL; w++)
    if (**w != '\0')
      {
        if (out->len > 0)
          g_string_append_c (out, ' ');
        g_string_append (out, *w);
      }
  return g_string_free (out, FALSE);
}

gboolean
jn_drive_same_text (const char *note, const char *doc)
{
  g_autofree char *a = words (note != NULL ? note : "");
  g_autofree char *b = words (doc != NULL ? doc : "");
  return strcmp (a, b) == 0;
}

/* ---- the helper's answer, line by line ------------------------------------------- */

static void
add_result (JsonBuilder *b, const JnDriveResult *r)
{
  json_builder_set_member_name (b, "topic");
  json_builder_add_int_value (b, r->topic);
  add_string (b, "doc", r->doc);
  add_string (b, "folder", r->folder);
  add_string (b, "link", r->link);
  json_builder_set_member_name (b, "verified");
  json_builder_add_boolean_value (b, r->verified);
  json_builder_set_member_name (b, "shared");
  json_builder_add_boolean_value (b, r->shared);
  add_string (b, "error", r->error);
}

char *
jn_drive_result_line (const JnDriveResult *r, const char *root)
{
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  add_result (b, r);
  add_string (b, "root", root);
  json_builder_end_object (b);
  return jn_json_to_string (b); /* compact: strings carry "\n" escaped */
}

char *
jn_drive_end_line (const char *root, const char *error)
{
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "end");
  json_builder_add_boolean_value (b, TRUE);
  add_string (b, "root", root);
  add_string (b, "error", error);
  json_builder_end_object (b);
  return jn_json_to_string (b);
}

void
jn_drive_result_clear (JnDriveResult *r)
{
  g_free (r->doc);
  g_free (r->folder);
  g_free (r->link);
  g_free (r->error);
  *r = (JnDriveResult){ 0 };
}

JnDriveLine
jn_drive_line_parse (const char *line, gssize len, JnDriveResult *result, char **root, char **error)
{
  *result = (JnDriveResult){ 0 };
  *root = *error = NULL;
  JsonParser *p;
  JsonObject *o = jn_json_object (line, len, &p);
  gint64 topic = 0;
  JnDriveLine kind = JN_DRIVE_LINE_BAD;
  if (jn_json_bool (o, "end"))
    {
      kind = JN_DRIVE_LINE_END;
      *error = g_strdup (jn_json_str (o, "error"));
    }
  else if (jn_json_int (o, "topic", &topic) && topic > 0)
    {
      kind = JN_DRIVE_LINE_RESULT;
      result->topic = topic;
      result->doc = g_strdup (jn_json_str (o, "doc"));
      result->folder = g_strdup (jn_json_str (o, "folder"));
      result->link = g_strdup (jn_json_str (o, "link"));
      result->verified = jn_json_bool (o, "verified");
      result->shared = jn_json_bool (o, "shared");
      result->error = g_strdup (jn_json_str (o, "error"));
    }
  if (kind != JN_DRIVE_LINE_BAD)
    *root = g_strdup (jn_json_str (o, "root"));
  g_object_unref (p);
  return kind;
}

gboolean
jn_drive_answer_from_lines (const char *text, JnDriveAnswer *out)
{
  *out = (JnDriveAnswer){ 0 };
  g_auto (GStrv) lines = g_strsplit (text != NULL ? text : "", "\n", -1);
  GArray *results = g_array_new (FALSE, TRUE, sizeof (JnDriveResult));
  gboolean ended = FALSE, any = FALSE;
  for (char **l = lines; *l != NULL; l++)
    {
      if (**l == '\0')
        continue;
      JnDriveResult r;
      char *root = NULL, *error = NULL;
      JnDriveLine kind = jn_drive_line_parse (*l, -1, &r, &root, &error);
      if (kind == JN_DRIVE_LINE_BAD)
        continue;
      any = TRUE;
      if (root != NULL)
        {
          g_free (out->root);
          out->root = root;
        }
      if (kind == JN_DRIVE_LINE_RESULT)
        g_array_append_val (results, r);
      else
        {
          ended = TRUE;
          g_free (out->error);
          out->error = error;
        }
    }
  if (any && !ended && out->error == NULL)
    out->error = g_strdup ("The Drive helper stopped before it finished; the topics it did are kept, the others "
                           "stay marked for the next backup.");
  out->n_results = results->len;
  out->results = (JnDriveResult *) g_array_free (results, FALSE);
  return any;
}

/* ---- finding a file Notes made before ---------------------------------------------- */

/* A string for Drive's query language: in single quotes, with \ and '
 * escaped by a backslash. */
static void
quoted (GString *q, const char *s)
{
  g_string_append_c (q, '\'');
  for (const char *p = s; *p != '\0'; p++)
    {
      if (*p == '\'' || *p == '\\')
        g_string_append_c (q, '\\');
      g_string_append_c (q, *p);
    }
  g_string_append_c (q, '\'');
}

char *
jn_drive_find_query (const char *name, const char *mime, const char *parent)
{
  GString *q = g_string_new ("name = ");
  quoted (q, name);
  g_string_append (q, " and mimeType = ");
  quoted (q, mime);
  g_string_append (q, " and ");
  quoted (q, parent);
  g_string_append (q, " in parents and trashed = false");
  char *escaped = g_uri_escape_string (q->str, NULL, FALSE);
  g_string_free (q, TRUE);
  return escaped;
}

gboolean
jn_drive_parse_first (const char *json, gssize len, char **id, char **link)
{
  *id = NULL;
  JsonParser *p;
  JsonObject *o = jn_json_object (json, len, &p);
  JsonArray *files = jn_json_arr (o, "files");
  if (files != NULL && json_array_get_length (files) > 0 && JSON_NODE_HOLDS_OBJECT (json_array_get_element (files, 0)))
    {
      JsonObject *f = json_array_get_object_element (files, 0);
      *id = g_strdup (jn_json_str (f, "id"));
      if (*id != NULL && link != NULL)
        *link = g_strdup (jn_json_str (f, "webViewLink"));
    }
  g_object_unref (p);
  return *id != NULL;
}
