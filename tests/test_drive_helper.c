/* The real Drive helper (journal-drive) against a fake Google Drive on
 * this machine.
 *
 * The fake (fake_google.h) keeps files as Drive does: ids, names, parents, a
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
#include "fake_google.h"

static const char *helper;
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
  JnDriveRequest r = { fg_base, (char *) "ya29.test-token", { (guint16) allowed_port }, 1, root, items, n };
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
  JnDriveAnswer a = backup (t, 3, fg_port ());
  gboolean all_ok = a.error == NULL && a.n_results == 3;
  for (gsize i = 0; all_ok && i < 3; i++)
    all_ok = a.results[i].verified && !a.results[i].shared && a.results[i].error == NULL &&
             g_str_has_prefix (a.results[i].link, "https://docs.google.com/");
  jr_case (&c, "first backup: all verified", all_ok, "error=%s n=%zu", a.error, a.n_results);
  FgFile *r = fg_file (root);
  jr_case (&c, "a Notes folder in My Drive", r != NULL && g_strcmp0 (r->name, "Notes") == 0 &&
           g_strcmp0 (r->mime, JN_DRIVE_FOLDER_MIME) == 0 && g_strcmp0 (r->parent, "root") == 0, "root %s", root);
  FgFile *dsa = fg_file (t[0].folder), *os = fg_file (t[2].folder);
  jr_case (&c, "one folder per subject, in Notes", dsa != NULL && os != NULL && dsa != os &&
           g_strcmp0 (t[1].folder, t[0].folder) == 0 && g_strcmp0 (dsa->name, "DSA") == 0 &&
           g_strcmp0 (dsa->parent, root) == 0 && g_strcmp0 (os->parent, root) == 0, "folders %s %s", t[0].folder,
           t[2].folder);
  FgFile *doc = fg_file (t[0].doc);
  jr_case (&c, "a Doc per topic in its folder", doc != NULL && g_strcmp0 (doc->name, "Prefix sum") == 0 &&
           g_strcmp0 (doc->mime, JN_DRIVE_DOC_MIME) == 0 && g_strcmp0 (doc->parent, t[0].folder) == 0 &&
           g_strcmp0 (doc->content, t[0].markdown) == 0, "doc %s", t[0].doc);
  jr_case (&c, "6 fg_files in all", fg_count () == 6, "Got %d", fg_count ());
  jn_drive_answer_clear (&a);

  /* Next backup: the same Docs, updated in place; renames followed. */
  g_autofree char *first_doc = g_strdup (t[0].doc);
  t[0].markdown = "# Prefix sum\n\n- pre[0] = 0\n  - pre[i+1] = pre[i] + a[i]\n- O(1) per query";
  t[1].name = "Heaps (binary)";
  a = backup (t, 3, fg_port ());
  jr_case (&c, "updated in place, no new files", fg_count () == 6 && g_strcmp0 (t[0].doc, first_doc) == 0 &&
           g_strcmp0 (fg_file (t[0].doc)->content, t[0].markdown) == 0, "files %d", fg_count ());
  jr_case (&c, "a renamed topic renames its Doc", g_strcmp0 (fg_file (t[1].doc)->name, "Heaps (binary)") == 0,
           "Got %s", fg_file (t[1].doc)->name);
  jn_drive_answer_clear (&a);

  /* A renamed subject renames its folder. */
  t[2].subject = "Operating Systems";
  a = backup (t, 3, fg_port ());
  jr_case (&c, "a renamed subject renames its folder", g_strcmp0 (fg_file (t[2].folder)->name, "Operating Systems") == 0,
           "Got %s", fg_file (t[2].folder)->name);
  jn_drive_answer_clear (&a);

  /* You put a Doc in the bin: it is made again in its folder. */
  fg_file (t[1].doc)->trashed = TRUE;
  g_autofree char *binned = g_strdup (t[1].doc);
  a = backup (t, 3, fg_port ());
  jr_case (&c, "a binned Doc made again", g_strcmp0 (t[1].doc, binned) != 0 && fg_file (t[1].doc) != NULL &&
           g_strcmp0 (fg_file (t[1].doc)->parent, t[1].folder) == 0 && a.results[1].verified, "doc %s", t[1].doc);
  jn_drive_answer_clear (&a);

  /* You deleted a subject's folder: the folder and its Docs are made again. */
  fg_delete_tree (t[2].folder);
  g_autofree char *gone = g_strdup (t[2].folder);
  a = backup (t, 3, fg_port ());
  jr_case (&c, "a deleted folder made again", g_strcmp0 (t[2].folder, gone) != 0 && fg_file (t[2].folder) != NULL &&
           g_strcmp0 (fg_file (t[2].doc)->parent, t[2].folder) == 0 && a.results[2].verified, "folder %s", t[2].folder);
  jn_drive_answer_clear (&a);

  /* You put a subject's folder in the bin: Drive shows what is in it as
   * in the bin too; both are made again. */
  fg_file (t[0].folder)->trashed = TRUE;
  fg_file (t[0].doc)->trashed = TRUE;
  fg_file (t[1].doc)->trashed = TRUE;
  g_autofree char *binned_folder = g_strdup (t[0].folder);
  a = backup (t, 3, fg_port ());
  jr_case (&c, "a binned folder made again, its Docs too", g_strcmp0 (t[0].folder, binned_folder) != 0 &&
           g_strcmp0 (t[1].folder, t[0].folder) == 0 && g_strcmp0 (fg_file (t[0].doc)->parent, t[0].folder) == 0 &&
           g_strcmp0 (fg_file (t[1].doc)->parent, t[0].folder) == 0 && a.results[0].verified && a.results[1].verified,
           "folder %s", t[0].folder);
  jn_drive_answer_clear (&a);

  /* You deleted the Notes folder itself. */
  fg_delete_tree (root);
  g_autofree char *old_root = g_strdup (root);
  a = backup (t, 3, fg_port ());
  jr_case (&c, "a deleted Notes folder made again", g_strcmp0 (root, old_root) != 0 && fg_file (root) != NULL &&
           g_strcmp0 (fg_file (t[0].folder)->parent, root) == 0, "root %s", root);
  jn_drive_answer_clear (&a);

  /* A Doc that reads back with a line missing: not called backed up. */
  g_mutex_lock (&fg_lock);
  fg_lossy_id = g_strdup (t[0].doc);
  g_mutex_unlock (&fg_lock);
  a = backup (t, 3, fg_port ());
  jr_case (&c, "a Doc that reads back different is reported", !a.results[0].verified && a.results[0].error != NULL &&
           strstr (a.results[0].error, "not the same") != NULL && a.results[1].verified, "Got %s", a.results[0].error);
  jn_drive_answer_clear (&a);
  g_clear_pointer (&fg_lossy_id, g_free);

  /* Someone else can open a Doc: flagged. */
  fg_file (t[2].doc)->shared = TRUE;
  a = backup (t, 3, fg_port ());
  jr_case (&c, "a shared Doc is flagged", a.results[2].shared && !a.results[0].shared, "shared=%d",
           a.results[2].shared);
  jn_drive_answer_clear (&a);
  fg_file (t[2].doc)->shared = FALSE;

  /* Drive is full: that topic fails in plain words, the others go on. */
  Topic more[] = { { "DSA", "Tries", "# Tries", NULL, NULL }, { "DSA", "Graphs", "# Graphs", NULL, NULL } };
  more[0].folder = g_strdup (t[0].folder);
  more[1].folder = g_strdup (t[0].folder);
  fg_full_name = g_strdup ("Tries");
  a = backup (more, 2, fg_port ());
  jr_case (&c, "Drive full: that topic only", a.error == NULL && a.results[0].error != NULL &&
           strstr (a.results[0].error, "full") != NULL && !a.results[0].verified && a.results[1].verified,
           "Got %s", a.results[0].error);
  jn_drive_answer_clear (&a);
  g_clear_pointer (&fg_full_name, g_free);

  /* The sign-in expired: one plain message, nothing half done. */
  fg_fail_auth = TRUE;
  a = backup (t, 3, fg_port ());
  jr_case (&c, "sign-in expired", a.error != NULL && strstr (a.error, "Sign in") != NULL &&
           strstr (a.error, "ya29") == NULL, "Got %s", a.error);
  jn_drive_answer_clear (&a);
  fg_fail_auth = FALSE;

  /* A fg_port Notes did not allow: Drive is never reached. */
  int before = fg_requests;
  a = backup (t, 3, fg_port () == 443 ? 444 : 443);
  jr_case (&c, "a fg_port not allowed is never reached", fg_requests == before && a.error != NULL, "requests %d -> %d, %s",
           before, fg_requests, a.error);
  jn_drive_answer_clear (&a);

  jr_case (&c, "the token only as the Authorization header", !fg_bad_token, "a request came without it");
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
  fg_start ();
  g_test_add_func ("/drive-helper/backups", test_backups);
  g_test_add_func ("/drive-helper/bad-request", test_bad_request);
  return g_test_run ();
}
