/* Tests for the Drive backup's messages (src/notes/drive/driveproto.c),
 * LeetCode style (see cases.h).
 *
 * Constraints: whatever Drive or the helper sends (broken JSON, wrong
 * types, missing fields) is read without crashing and gives a plain
 * answer; requests and answers survive the trip to the helper and back
 * unchanged; the "same text" check ignores how Google writes Markdown
 * but catches any missing, changed or extra word. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "driveproto.h"

/* ---- requests and answers survive the trip ---------------------------------------- */

static void
test_round_trip (void)
{
  JnDriveItem items[] = {
    { 7, (char *) "DSA", (char *) "Prefix sum", (char *) "# Prefix sum\n\n- pre[0] = 0\n", NULL, NULL },
    { 9, (char *) "गणित", (char *) "Café ☕ \"quoted\"", (char *) "x\\y\n\ttab", (char *) "doc-1", (char *) "fold-1" },
  };
  JnDriveRequest r = { (char *) "https://www.googleapis.com/", (char *) "ya29.test-token", { 443 }, 1,
                       (char *) "root-1", items, G_N_ELEMENTS (items) };
  g_autofree char *json = jn_drive_request_to_json (&r);
  JnDriveRequest back = { 0 };
  g_autoptr (GError) error = NULL;
  g_assert_true (jn_drive_request_from_json (json, -1, &back, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (back.api_base, ==, r.api_base);
  g_assert_cmpstr (back.token, ==, r.token);
  g_assert_cmpuint (back.n_ports, ==, 1);
  g_assert_cmpuint (back.ports[0], ==, 443);
  g_assert_cmpstr (back.root, ==, "root-1");
  g_assert_cmpuint (back.n_items, ==, 2);
  for (gsize i = 0; i < 2; i++)
    {
      g_assert_cmpint (back.items[i].topic, ==, items[i].topic);
      g_assert_cmpstr (back.items[i].subject, ==, items[i].subject);
      g_assert_cmpstr (back.items[i].name, ==, items[i].name);
      g_assert_cmpstr (back.items[i].markdown, ==, items[i].markdown);
      g_assert_cmpstr (back.items[i].doc, ==, items[i].doc);
      g_assert_cmpstr (back.items[i].folder, ==, items[i].folder);
    }
  jn_drive_request_clear (&back);

  JnDriveResult results[] = {
    { 7, (char *) "doc-7", (char *) "fold-1", (char *) "https://docs.google.com/document/d/doc-7", TRUE, FALSE, NULL },
    { 9, NULL, (char *) "fold-2", NULL, FALSE, TRUE, (char *) "Drive is full" },
  };
  JnDriveAnswer a = { (char *) "root-1", NULL, results, 2 };
  g_autofree char *ajson = jn_drive_answer_to_json (&a);
  JnDriveAnswer aback = { 0 };
  g_assert_true (jn_drive_answer_from_json (ajson, -1, &aback, NULL));
  g_assert_cmpstr (aback.root, ==, "root-1");
  g_assert_null (aback.error);
  g_assert_cmpuint (aback.n_results, ==, 2);
  for (gsize i = 0; i < 2; i++)
    {
      g_assert_cmpint (aback.results[i].topic, ==, results[i].topic);
      g_assert_cmpstr (aback.results[i].doc, ==, results[i].doc);
      g_assert_cmpstr (aback.results[i].folder, ==, results[i].folder);
      g_assert_cmpstr (aback.results[i].link, ==, results[i].link);
      g_assert_cmpint (aback.results[i].verified, ==, results[i].verified);
      g_assert_cmpint (aback.results[i].shared, ==, results[i].shared);
      g_assert_cmpstr (aback.results[i].error, ==, results[i].error);
    }
  jn_drive_answer_clear (&aback);

  /* The whole backup failed. */
  JnDriveAnswer failed = { NULL, (char *) "Sign in again", NULL, 0 };
  g_autofree char *fjson = jn_drive_answer_to_json (&failed);
  g_assert_true (jn_drive_answer_from_json (fjson, -1, &aback, NULL));
  g_assert_cmpstr (aback.error, ==, "Sign in again");
  g_assert_cmpuint (aback.n_results, ==, 0);
  jn_drive_answer_clear (&aback);
}

/* ---- line by line ------------------------------------------------------------ */

static void
test_lines (void)
{
  JnDriveResult r = { 7, (char *) "doc-7", (char *) "fold-1", (char *) "https://docs.google.com/d/doc-7", TRUE, FALSE,
                      NULL };
  g_autofree char *line = jn_drive_result_line (&r, "root-1");
  g_assert_null (strchr (line, '\n')); /* one line, whatever the strings hold */
  JnDriveResult back = { 0 };
  g_autofree char *root = NULL, *error = NULL;
  g_assert_cmpint (jn_drive_line_parse (line, -1, &back, &root, &error), ==, JN_DRIVE_LINE_RESULT);
  g_assert_cmpint (back.topic, ==, 7);
  g_assert_cmpstr (back.doc, ==, "doc-7");
  g_assert_cmpstr (back.folder, ==, "fold-1");
  g_assert_true (back.verified);
  g_assert_cmpstr (root, ==, "root-1");
  jn_drive_result_clear (&back);
  g_clear_pointer (&root, g_free);

  JnDriveResult failed = { 9, NULL, NULL, NULL, FALSE, FALSE, (char *) "Drive is full\nreally" };
  g_autofree char *fline = jn_drive_result_line (&failed, NULL);
  g_assert_null (strchr (fline, '\n'));
  g_assert_cmpint (jn_drive_line_parse (fline, -1, &back, &root, &error), ==, JN_DRIVE_LINE_RESULT);
  g_assert_cmpstr (back.error, ==, "Drive is full\nreally");
  g_assert_null (root);
  jn_drive_result_clear (&back);

  g_autofree char *end = jn_drive_end_line ("root-1", "Sign in again");
  g_assert_cmpint (jn_drive_line_parse (end, -1, &back, &root, &error), ==, JN_DRIVE_LINE_END);
  g_assert_cmpstr (root, ==, "root-1");
  g_assert_cmpstr (error, ==, "Sign in again");
  g_clear_pointer (&root, g_free);
  g_clear_pointer (&error, g_free);

  static const char *bad[] = { "", "{}", "[1]", "{\"topic\":\"x\"}", "{\"end\":5}", "nonsense", "{\"topic\":0}" };
  JrCases c = { "bad lines", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      JnDriveLine k = jn_drive_line_parse (bad[i], -1, &back, &root, &error);
      jr_case (&c, bad[i], k == JN_DRIVE_LINE_BAD && back.doc == NULL && root == NULL && error == NULL, "Got %d", k);
    }
  jr_cases_done (&c);

  /* A whole answer from lines; without its end line it was cut off. */
  g_autofree char *text = g_strdup_printf ("%s\n%s\n%s\n", line, fline, end);
  JnDriveAnswer a = { 0 };
  g_assert_true (jn_drive_answer_from_lines (text, &a));
  g_assert_cmpuint (a.n_results, ==, 2);
  g_assert_cmpstr (a.root, ==, "root-1");
  g_assert_cmpstr (a.error, ==, "Sign in again");
  g_assert_cmpint (a.results[0].topic, ==, 7);
  g_assert_cmpint (a.results[1].topic, ==, 9);
  jn_drive_answer_clear (&a);
  g_autofree char *cut = g_strdup_printf ("%s\n", line);
  g_assert_true (jn_drive_answer_from_lines (cut, &a));
  g_assert_cmpuint (a.n_results, ==, 1);
  g_assert_nonnull (a.error); /* cut off */
  jn_drive_answer_clear (&a);
  g_assert_false (jn_drive_answer_from_lines ("garbage\n", &a));
  jn_drive_answer_clear (&a);
}

/* ---- refused requests ---------------------------------------------------------- */

static void
test_bad_requests (void)
{
  static const struct { const char *name, *json; } t[] = {
    { "empty",              "" },
    { "not an object",      "[]" },
    { "no token",           "{\"api_base\":\"https://www.googleapis.com/\",\"ports\":[443],\"items\":[]}" },
    { "no address",         "{\"token\":\"t\",\"ports\":[443],\"items\":[]}" },
    { "address not http",   "{\"api_base\":\"file:///etc/\",\"token\":\"t\",\"ports\":[443],\"items\":[]}" },
    { "no ports",           "{\"api_base\":\"https://x/\",\"token\":\"t\",\"ports\":[],\"items\":[]}" },
    { "port out of range",  "{\"api_base\":\"https://x/\",\"token\":\"t\",\"ports\":[70000],\"items\":[]}" },
    { "too many ports",     "{\"api_base\":\"https://x/\",\"token\":\"t\",\"ports\":[1,2,3,4,5],\"items\":[]}" },
    { "items not a list",   "{\"api_base\":\"https://x/\",\"token\":\"t\",\"ports\":[443],\"items\":{}}" },
    { "item without topic", "{\"api_base\":\"https://x/\",\"token\":\"t\",\"ports\":[443],\"items\":[{\"name\":\"a\","
                            "\"subject\":\"b\",\"markdown\":\"c\"}]}" },
    { "item without text",  "{\"api_base\":\"https://x/\",\"token\":\"t\",\"ports\":[443],\"items\":[{\"topic\":1,"
                            "\"name\":\"a\",\"subject\":\"b\"}]}" },
  };
  JrCases c = { "bad requests", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      JnDriveRequest r = { 0 };
      g_autoptr (GError) error = NULL;
      gboolean ok = jn_drive_request_from_json (t[i].json, -1, &r, &error);
      jr_case (&c, t[i].name, !ok && error != NULL && r.token == NULL, "Input %s  Expected refused  Got %s", t[i].json,
               ok ? "read" : "refused but not cleaned up");
      jn_drive_request_clear (&r);
    }
  /* No items is fine: nothing to back up. */
  JnDriveRequest r = { 0 };
  jr_case (&c, "no items", jn_drive_request_from_json ("{\"api_base\":\"https://x/\",\"token\":\"t\",\"ports\":[443],"
                                                       "\"items\":[]}", -1, &r, NULL) && r.n_items == 0,
           "Expected read with 0 items");
  jn_drive_request_clear (&r);
  jr_cases_done (&c);
}

/* ---- Drive's answers ------------------------------------------------------------- */

static void
test_drive_answers (void)
{
  static const struct { const char *name, *json; gboolean ok; const char *id; gboolean trashed; const char *link; } f[] = {
    { "a file",       "{\"id\":\"abc\",\"trashed\":false,\"webViewLink\":\"https://docs.google.com/d/abc\"}", TRUE,
                      "abc", FALSE, "https://docs.google.com/d/abc" },
    { "in the bin",   "{\"id\":\"abc\",\"trashed\":true}", TRUE, "abc", TRUE, NULL },
    { "no trashed",   "{\"id\":\"abc\"}", TRUE, "abc", FALSE, NULL },
    { "no id",        "{\"trashed\":false}", FALSE, NULL, FALSE, NULL },
    { "id not text",  "{\"id\":5}", FALSE, NULL, FALSE, NULL },
    { "not JSON",     "<html>", FALSE, NULL, FALSE, NULL },
  };
  JrCases c = { "file", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (f); i++)
    {
      g_autofree char *id = NULL, *link = NULL;
      gboolean trashed = 99;
      gboolean ok = jn_drive_parse_file (f[i].json, -1, &id, &trashed, &link);
      gboolean right = ok == f[i].ok && (!ok || (g_strcmp0 (id, f[i].id) == 0 && trashed == f[i].trashed &&
                                                  g_strcmp0 (link, f[i].link) == 0));
      jr_case (&c, f[i].name, right, "Input %s  Got ok=%d id=%s trashed=%d link=%s", f[i].json, ok, id, trashed, link);
    }
  jr_cases_done (&c);

  static const struct { const char *name, *json; gboolean ok, shared; } p[] = {
    { "only you",       "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\",\"emailAddress\":\"me@x\"}]}", TRUE,
                        FALSE },
    { "a reader",       "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"},{\"type\":\"user\","
                        "\"role\":\"reader\",\"emailAddress\":\"them@x\"}]}", TRUE, TRUE },
    { "anyone with the link", "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"},{\"type\":\"anyone\","
                        "\"role\":\"reader\"}]}", TRUE, TRUE },
    { "a whole domain", "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"},{\"type\":\"domain\","
                        "\"role\":\"commenter\"}]}", TRUE, TRUE },
    { "a group writer", "{\"permissions\":[{\"type\":\"user\",\"role\":\"owner\"},{\"type\":\"group\","
                        "\"role\":\"writer\"}]}", TRUE, TRUE },
    { "empty list",     "{\"permissions\":[]}", TRUE, FALSE },
    { "no list",        "{}", FALSE, FALSE },
    { "broken entry",   "{\"permissions\":[5,{\"type\":\"user\",\"role\":\"owner\"}]}", TRUE, FALSE },
    { "not JSON",       "nope", FALSE, FALSE },
  };
  JrCases d = { "permissions", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (p); i++)
    {
      gboolean shared = 99;
      gboolean ok = jn_drive_parse_permissions (p[i].json, -1, &shared);
      jr_case (&d, p[i].name, ok == p[i].ok && (!ok || shared == p[i].shared), "Input %s  Got ok=%d shared=%d",
               p[i].json, ok, shared);
    }
  jr_cases_done (&d);

  static const struct { const char *name, *json; int status; const char *says; } e[] = {
    { "sign-in expired", "{\"error\":{\"code\":401,\"message\":\"Invalid Credentials\"}}", 401, "Sign in" },
    { "no permission",   "{\"error\":{\"code\":403,\"message\":\"Insufficient Permission\"}}", 403, "permission" },
    { "Drive full",      "{\"error\":{\"code\":403,\"message\":\"The user's Drive storage quota has been exceeded.\","
                         "\"errors\":[{\"reason\":\"storageQuotaExceeded\"}]}}", 403, "full" },
    { "rate limit",      "{\"error\":{\"code\":429,\"message\":\"Rate Limit Exceeded\"}}", 429, "try again" },
    { "server trouble",  "<html>oops</html>", 503, "503" },
    { "not found",       "{\"error\":{\"code\":404,\"message\":\"File not found: x.\"}}", 404, "File not found" },
  };
  JrCases g = { "errors", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (e); i++)
    {
      g_autofree char *msg = jn_drive_error (e[i].json, -1, e[i].status);
      jr_case (&g, e[i].name, msg != NULL && strstr (msg, e[i].says) != NULL && g_utf8_validate (msg, -1, NULL),
               "Expected \"%s\" in it  Got \"%s\"", e[i].says, msg);
    }
  jr_cases_done (&g);
}

/* ---- finding a file Notes made before ---------------------------------------------- */

static void
test_find (void)
{
  static const struct { const char *name, *parent, *want; } t[] = {
    { "Prefix sum", "fold-1", "name = 'Prefix sum' and mimeType = '" JN_DRIVE_DOC_MIME "' and 'fold-1' in parents and trashed = false" },
    { "it's \\ odd", "root", "name = 'it\\'s \\\\ odd' and mimeType = '" JN_DRIVE_DOC_MIME "' and 'root' in parents and trashed = false" },
  };
  JrCases c = { "find query", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      g_autofree char *q = jn_drive_find_query (t[i].name, JN_DRIVE_DOC_MIME, t[i].parent);
      g_autofree char *plain = g_uri_unescape_string (q, NULL);
      jr_case (&c, t[i].name, g_strcmp0 (plain, t[i].want) == 0 && strpbrk (q, " '&=") == NULL,
               "Expected %s  Got %s (%s)", t[i].want, plain, q);
    }
  jr_cases_done (&c);
  g_autofree char *id = NULL, *link = NULL;
  g_assert_true (jn_drive_parse_first ("{\"files\":[{\"id\":\"a\",\"webViewLink\":\"https://d/a\"},{\"id\":\"b\"}]}", -1,
                                       &id, &link));
  g_assert_cmpstr (id, ==, "a");
  g_assert_cmpstr (link, ==, "https://d/a");
  g_autofree char *none = NULL;
  g_assert_false (jn_drive_parse_first ("{\"files\":[]}", -1, &none, NULL));
  g_assert_false (jn_drive_parse_first ("{\"files\":[5]}", -1, &none, NULL));
  g_assert_false (jn_drive_parse_first ("nope", -1, &none, NULL));
  g_assert_null (none);
}

/* ---- the upload itself ------------------------------------------------------------- */

static void
test_upload_body (void)
{
  g_autofree char *meta = jn_drive_metadata ("Prefix \"sum\"", JN_DRIVE_DOC_MIME, "fold-1");
  g_assert_nonnull (strstr (meta, "\"name\":\"Prefix \\\"sum\\\"\""));
  g_assert_nonnull (strstr (meta, "\"mimeType\":\"" JN_DRIVE_DOC_MIME "\""));
  g_assert_nonnull (strstr (meta, "\"parents\":[\"fold-1\"]"));
  g_autofree char *top = jn_drive_metadata ("Notes", JN_DRIVE_FOLDER_MIME, NULL);
  g_assert_null (strstr (top, "parents")); /* in My Drive itself */

  /* The note's text may hold anything, even a line like a boundary. */
  const char *md = "# Prefix sum\n--not-a-boundary\n- é ✓";
  g_autofree char *type = NULL;
  g_autoptr (GBytes) body = jn_drive_multipart (meta, md, &type);
  g_assert_true (g_str_has_prefix (type, "multipart/related; boundary="));
  const char *boundary = type + strlen ("multipart/related; boundary=");
  gsize n = 0;
  const char *data = g_bytes_get_data (body, &n);
  g_autofree char *text = g_strndup (data, n);
  g_assert_null (strstr (md, boundary)); /* a boundary never in the text */
  g_autofree char *open = g_strdup_printf ("--%s\r\n", boundary);
  g_autofree char *close = g_strdup_printf ("\r\n--%s--", boundary);
  g_assert_true (g_str_has_prefix (text, open));
  g_assert_nonnull (strstr (text, "Content-Type: application/json; charset=UTF-8\r\n\r\n"));
  g_assert_nonnull (strstr (text, meta));
  g_assert_nonnull (strstr (text, "Content-Type: text/markdown; charset=UTF-8\r\n\r\n# Prefix sum\n--not-a-boundary\n- é ✓"));
  g_assert_nonnull (strstr (text, close));
}

/* ---- the same text? ------------------------------------------------------------------ */

static void
test_same_text (void)
{
  const char *note = "# Prefix sum\n\nSubject: DSA\n\n## 1 Nov 2026, 14:05–15:40\n\nSources:\n"
                     "- <https://cp-algorithms.com/x>\n\n### Idea\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n"
                     "→ range sum in O(1)\n\\# not a heading\nplain é ✓";
  static const struct { const char *name, *doc; gboolean same; } t[] = {
    { "itself", NULL, TRUE },
    { "Google's bullets and spacing",
      "# Prefix sum\n\nSubject: DSA\n\n## 1 Nov 2026, 14:05–15:40\n\nSources:\n\n* <https://cp-algorithms.com/x>\n\n"
      "### Idea\n\n* pre\\[0\\] = 0\n    * pre\\[i+1\\] = pre\\[i\\] + a\\[i\\]\n\n→ range sum in O(1)\n\n\\# not a heading\n\n"
      "plain é ✓\n", TRUE },
    { "a link written out",
      "# Prefix sum\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- [https://cp-algorithms.com/x](https://cp-algorithms.com/x)\n"
      "### Idea\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n→ range sum in O(1)\n\\# not a heading\nplain é ✓", TRUE },
    { "bold heading marks",
      "# **Prefix sum**\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- <https://cp-algorithms.com/x>\n"
      "### Idea\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n→ range sum in O(1)\n\\# not a heading\nplain é ✓", TRUE },
    { "a word missing",
      "# Prefix sum\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- <https://cp-algorithms.com/x>\n"
      "### Idea\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n→ range sum in\n\\# not a heading\nplain é ✓", FALSE },
    { "a word changed",
      "# Prefix sum\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- <https://cp-algorithms.com/x>\n"
      "### Idea\n- pre[0] = 1\n  - pre[i+1] = pre[i] + a[i]\n→ range sum in O(1)\n\\# not a heading\nplain é ✓", FALSE },
    { "cut off at the end",
      "# Prefix sum\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- <https://cp-algorithms.com/x>\n"
      "### Idea\n- pre[0] = 0", FALSE },
    { "an extra line",
      "# Prefix sum\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- <https://cp-algorithms.com/x>\n"
      "### Idea\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n→ range sum in O(1)\n\\# not a heading\nplain é ✓\nextra", FALSE },
    { "lines swapped",
      "# Prefix sum\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- <https://cp-algorithms.com/x>\n"
      "### Idea\n  - pre[i+1] = pre[i] + a[i]\n- pre[0] = 0\n→ range sum in O(1)\n\\# not a heading\nplain é ✓", FALSE },
    { "accent lost",
      "# Prefix sum\nSubject: DSA\n## 1 Nov 2026, 14:05–15:40\nSources:\n- <https://cp-algorithms.com/x>\n"
      "### Idea\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n→ range sum in O(1)\n\\# not a heading\nplain e ✓", FALSE },
    { "empty doc", "", FALSE },
  };
  JrCases c = { "same text", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      gboolean got = jn_drive_same_text (note, t[i].doc != NULL ? t[i].doc : note);
      jr_case (&c, t[i].name, got == t[i].same, "Expected %s  Got %s", t[i].same ? "same" : "different",
               got ? "same" : "different");
    }
  /* Both empty: the same (an empty topic). */
  jr_case (&c, "both empty", jn_drive_same_text ("", "\n\n"), "Expected same");
  jr_cases_done (&c);
}

/* Google Docs' real Markdown, recorded in the live check of 1 Nov 2026
 * (tests/data): what Notes sent, and what Google gave back. Google
 * escapes freely, writes links out, keeps "*" as text, and joined a line
 * that came right after a bullet onto it; none of that is a different
 * text. But it dropped "<tags>", taken for web code: that one is. */
static char *
data (const char *name)
{
  g_autofree char *path = g_build_filename (g_getenv ("JN_TEST_DATA"), name, NULL);
  char *text = NULL;
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  return text;
}

static void
test_same_text_google (void)
{
  static const struct { const char *name, *note, *doc; gboolean same; } t[] = {
    { "Paging: escaped link, arrow joined to its bullet", "notes-paging-2026-11-01.md",
      "google-paging-2026-11-01.md", TRUE },
    { "Prefix sum: \"<tags>\" dropped by Google", "notes-prefix-sum-2026-11-01.md",
      "google-prefix-sum-2026-11-01.md", FALSE },
  };
  JrCases c = { "same text (Google's own)", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      g_autofree char *note = data (t[i].note), *doc = data (t[i].doc);
      gboolean got = jn_drive_same_text (note, doc);
      jr_case (&c, t[i].name, got == t[i].same, "Expected %s  Got %s", t[i].same ? "same" : "different",
               got ? "same" : "different");
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/driveproto/round-trip", test_round_trip);
  g_test_add_func ("/driveproto/lines", test_lines);
  g_test_add_func ("/driveproto/bad-requests", test_bad_requests);
  g_test_add_func ("/driveproto/drive-answers", test_drive_answers);
  g_test_add_func ("/driveproto/upload-body", test_upload_body);
  g_test_add_func ("/driveproto/find", test_find);
  g_test_add_func ("/driveproto/same-text", test_same_text);
  g_test_add_func ("/driveproto/same-text-google", test_same_text_google);
  return g_test_run ();
}
