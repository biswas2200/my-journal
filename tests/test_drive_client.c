/* Notes' Drive client (src/notes/drive/driveclient.c) with a fake Google
 * (fake_google.h), a fake browser and the real Drive helper.
 *
 * The fake browser does what you would on Google's page: it reads the
 * sign-in address Notes opened and comes back to the address on this
 * laptop with a code (or "access denied"). The refresh token is kept in
 * a stand-in for the login keyring, so no real keyring is touched.
 *
 * Checked: without a client file Notes says where to put it; a sign-in
 * trades the code with the very verifier whose challenge was on the page
 * and keeps the refresh token; a request without the sign-in's state
 * (another program poking the port) is ignored; saying no on Google's
 * page signs nothing in; a backup works right after signing in and, in a
 * fresh start, from the kept refresh token; a revoked sign-in is
 * forgotten (Sign in is offered again); signing out revokes the token
 * at Google and forgets it here. */
#include <string.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include "cases.h"
#include "driveclient.h"
#include "fake_google.h"
#include "oauth.h"

static const char *helper;
static char *client_file;

/* ---- the stand-in keyring ------------------------------------------------------ */

static char *kept;

static char *
store_load (gpointer user)
{
  (void) user;
  return g_strdup (kept);
}

static gboolean
store_save (const char *token, gpointer user)
{
  (void) user;
  g_free (kept);
  kept = g_strdup (token);
  return TRUE;
}

static void
store_clear (gpointer user)
{
  (void) user;
  g_clear_pointer (&kept, g_free);
}

/* ---- the fake browser ------------------------------------------------------------ */

typedef enum { ALLOW, DENY, INTRUDER_FIRST } Answer;
static Answer browser_answer;
static char *page_challenge, *page_redirect;

typedef struct {
  char *redirect, *state;
  Answer answer;
} Visit;

static gpointer
browse (gpointer data)
{
  Visit *v = data;
  SoupSession *s = soup_session_new ();
  if (v->answer == INTRUDER_FIRST)
    {
      g_autofree char *poke = g_strdup_printf ("%s?state=guessed&code=evil", v->redirect);
      SoupMessage *m = soup_message_new ("GET", poke);
      g_autoptr (GBytes) b = soup_session_send_and_read (s, m, NULL, NULL);
      g_object_unref (m);
    }
  g_autofree char *esc_state = g_uri_escape_string (v->state, NULL, FALSE);
  g_autofree char *back = v->answer == DENY
                            ? g_strdup_printf ("%s?error=access_denied&state=%s", v->redirect, esc_state)
                            : g_strdup_printf ("%s?state=%s&code=%s", v->redirect, esc_state, "4%2Ftest-code");
  SoupMessage *m = soup_message_new ("GET", back);
  g_autoptr (GBytes) b = soup_session_send_and_read (s, m, NULL, NULL);
  g_object_unref (m);
  g_object_unref (s);
  g_free (v->redirect);
  g_free (v->state);
  g_free (v);
  return NULL;
}

static void
open_page (const char *uri, gpointer user)
{
  (void) user;
  g_autoptr (GUri) u = g_uri_parse (uri, G_URI_FLAGS_ENCODED_QUERY, NULL);
  g_autoptr (GHashTable) q = g_uri_parse_params (g_uri_get_query (u), -1, "&", G_URI_PARAMS_NONE, NULL);
  g_free (page_challenge);
  g_free (page_redirect);
  page_challenge = g_strdup (g_hash_table_lookup (q, "code_challenge"));
  page_redirect = g_strdup (g_hash_table_lookup (q, "redirect_uri"));
  Visit *v = g_new0 (Visit, 1);
  v->redirect = g_strdup (page_redirect);
  v->state = g_strdup (g_hash_table_lookup (q, "state"));
  v->answer = browser_answer;
  g_thread_unref (g_thread_new ("browser", browse, v));
}

/* ---- waiting for the client --------------------------------------------------------- */

typedef struct {
  gboolean done, ok;
  char *error;
  int verified, n, each;
  GCancellable *cancel_after_first; /* cancelled when the first topic is reported */
} Result;

static void
on_signed_in (gboolean ok, const char *error, gpointer user)
{
  Result *r = user;
  r->done = TRUE;
  r->ok = ok;
  r->error = g_strdup (error);
}

static void
on_each (const JnDriveResult *r, const char *root, gpointer user)
{
  (void) root;
  Result *res = user;
  res->each += r->doc != NULL;
  if (res->cancel_after_first != NULL)
    g_cancellable_cancel (res->cancel_after_first);
}

static void
on_backed_up (const JnDriveAnswer *a, const char *error, gpointer user)
{
  Result *r = user;
  r->done = TRUE;
  r->error = g_strdup (error != NULL ? error : a->error);
  r->ok = r->error == NULL;
  for (gsize i = 0; a != NULL && i < a->n_results; i++)
    r->verified += a->results[i].verified;
  r->n = a != NULL ? (int) a->n_results : 0;
}

static void
wait_for (Result *r)
{
  gint64 until = g_get_monotonic_time () + 20 * G_USEC_PER_SEC;
  while (!r->done && g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (r->done);
}

static JnBackup *
client (const char *file)
{
  g_autofree char *token = g_strconcat (fg_base, "oauth/token", NULL);
  g_autofree char *revoke = g_strconcat (fg_base, "oauth/revoke", NULL);
  JnBackupConfig c = { helper, file, NULL, token, revoke, fg_base, { (guint16) fg_port () }, 1,
                       { store_load, store_save, store_clear, NULL }, open_page, NULL };
  return jn_google_backup_new (&c);
}

static const JnDriveItem items[] = {
  { 1, (char *) "DSA", (char *) "Prefix sum", (char *) "# Prefix sum\n\n- pre[0] = 0", NULL, NULL },
  { 2, (char *) "OS", (char *) "Paging", (char *) "# Paging\n\n- page table", NULL, NULL },
};

static void
test_client (void)
{
  JrCases c = { "drive client", 0, 0 };

  /* No client file: says where to put it, nothing happens. */
  g_autofree char *missing = g_strconcat (client_file, ".missing", NULL);
  JnBackup *b = client (missing);
  jr_case (&c, "no client file", !b->can_sign_in (b) && !b->signed_in (b), "can=%d", b->can_sign_in (b));
  Result r = { 0 };
  b->sign_in (b, NULL, on_signed_in, &r);
  wait_for (&r);
  jr_case (&c, "no client file: says where", !r.ok && r.error != NULL && strstr (r.error, "google-client.json") != NULL,
           "Got %s", r.error);
  g_free (r.error);
  b->free (b);

  /* A sign-in, with a program poking the port first. */
  b = client (client_file);
  jr_case (&c, "a client file", b->can_sign_in (b) && !b->signed_in (b), "can=%d", b->can_sign_in (b));
  browser_answer = INTRUDER_FIRST;
  r = (Result){ 0 };
  b->sign_in (b, NULL, on_signed_in, &r);
  wait_for (&r);
  g_mutex_lock (&fg_lock);
  g_autofree char *challenge = fg_last_verifier != NULL ? jn_oauth_challenge (fg_last_verifier) : NULL;
  gboolean same_redirect = g_strcmp0 (fg_last_redirect, page_redirect) == 0;
  g_mutex_unlock (&fg_lock);
  jr_case (&c, "signed in, the intruder ignored", r.ok && b->signed_in (b) && g_strcmp0 (kept, FG_REFRESH) == 0,
           "ok=%d %s kept=%s", r.ok, r.error, kept);
  jr_case (&c, "the code traded with the page's verifier", g_strcmp0 (challenge, page_challenge) == 0 && same_redirect &&
           g_str_has_prefix (page_redirect, "http://127.0.0.1:"), "challenge %s vs %s, redirect %s", challenge,
           page_challenge, page_redirect);
  g_free (r.error);

  /* A backup right away (the sign-in's own access token). */
  r = (Result){ 0 };
  b->run (b, NULL, items, G_N_ELEMENTS (items), NULL, on_each, on_backed_up, &r);
  wait_for (&r);
  jr_case (&c, "backed up after signing in", r.ok && r.n == 2 && r.verified == 2, "%s n=%d verified=%d", r.error, r.n,
           r.verified);
  g_free (r.error);
  b->free (b);

  /* A fresh start: the kept refresh token gets a new access token. */
  b = client (client_file);
  int calls = fg_token_calls;
  r = (Result){ 0 };
  b->run (b, NULL, items, G_N_ELEMENTS (items), NULL, on_each, on_backed_up, &r);
  wait_for (&r);
  jr_case (&c, "backed up after a restart", r.ok && r.verified == 2 && fg_token_calls == calls + 1, "%s calls %d",
           r.error, fg_token_calls - calls);
  g_free (r.error);

  /* Stopped half-way (Notes closing): every topic done so far was
   * reported and the helper itself stops. A request already on its way
   * may still make a Doc Notes did not hear of; backing up again finds it
   * instead of making a second one. */
  JnDriveItem many[6];
  for (int i = 0; i < 6; i++)
    many[i] = (JnDriveItem){ 10 + i, (char *) "DSA", g_strdup_printf ("Topic %d", i), (char *) "# x", NULL, NULL };
  fg_delay_ms = 60;
  GCancellable *cancel = g_cancellable_new ();
  r = (Result){ 0 };
  r.cancel_after_first = cancel;
  b->run (b, NULL, many, 6, cancel, on_each, on_backed_up, &r);
  wait_for (&r);
  g_usleep (500 * 1000); /* a request already on its way lands */
  int seen = fg_requests;
  g_usleep (1500 * 1000);
  jr_case (&c, "stopped: done topics reported, helper stopped", r.each >= 1 && r.each < 6 && r.error != NULL &&
           strstr (r.error, "Stopped") != NULL && fg_requests == seen, "each=%d %s requests %d -> %d", r.each, r.error,
           seen, fg_requests);
  fg_delay_ms = 0;
  g_free (r.error);
  r = (Result){ 0 };
  b->run (b, NULL, many, 6, NULL, on_each, on_backed_up, &r); /* Notes forgot nothing it heard; it heard little */
  wait_for (&r);
  int docs = 0;
  g_mutex_lock (&fg_lock);
  GHashTableIter fit;
  gpointer fk, fv;
  g_hash_table_iter_init (&fit, fg_files);
  while (g_hash_table_iter_next (&fit, &fk, &fv))
    docs += g_str_has_prefix (((FgFile *) fv)->name, "Topic ");
  g_mutex_unlock (&fg_lock);
  jr_case (&c, "backed up again: one Doc per topic, none twice", r.ok && r.verified == 6 && docs == 6,
           "%s verified=%d docs=%d", r.error, r.verified, docs);
  g_free (r.error);
  g_object_unref (cancel);
  for (int i = 0; i < 6; i++)
    g_free ((char *) many[i].name);

  /* Google revoked it: forgotten, Sign in offered again. */
  b->free (b);
  b = client (client_file);
  fg_refresh_revoked = TRUE;
  r = (Result){ 0 };
  b->run (b, NULL, items, G_N_ELEMENTS (items), NULL, on_each, on_backed_up, &r);
  wait_for (&r);
  jr_case (&c, "a revoked sign-in is forgotten", !r.ok && r.error != NULL && strstr (r.error, "Sign in") != NULL &&
           !b->signed_in (b) && kept == NULL && strstr (r.error, "1//") == NULL, "%s kept=%s", r.error, kept);
  g_free (r.error);
  fg_refresh_revoked = FALSE;

  /* Saying no on Google's page. */
  browser_answer = DENY;
  r = (Result){ 0 };
  b->sign_in (b, NULL, on_signed_in, &r);
  wait_for (&r);
  jr_case (&c, "said no on Google's page", !r.ok && r.error != NULL && strstr (r.error, "did not allow") != NULL &&
           !b->signed_in (b) && kept == NULL, "%s", r.error);
  g_free (r.error);

  /* Sign in again, then out: revoked at Google, forgotten here. */
  browser_answer = ALLOW;
  r = (Result){ 0 };
  b->sign_in (b, NULL, on_signed_in, &r);
  wait_for (&r);
  g_free (r.error);
  b->sign_out (b);
  gint64 until = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (fg_revoked == NULL && g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, FALSE);
  jr_case (&c, "signing out revokes and forgets", !b->signed_in (b) && kept == NULL &&
           g_strcmp0 (fg_revoked, FG_REFRESH) == 0, "revoked %s", fg_revoked);
  b->free (b);
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (argc < 2)
    {
      g_printerr ("usage: test_drive_client PATH-TO-journal-drive\n");
      return 1;
    }
  helper = argv[1];
  fg_start ();
  g_autofree char *dir = g_dir_make_tmp ("drive-client-XXXXXX", NULL);
  client_file = g_build_filename (dir, "google-client.json", NULL);
  g_assert_true (g_file_set_contents (client_file, "{\"installed\":{\"client_id\":\"123.apps.googleusercontent.com\","
                                      "\"client_secret\":\"" FG_SECRET "\"}}", -1, NULL));
  g_test_add_func ("/drive-client/sign-in-and-back-up", test_client);
  int rc = g_test_run ();
  g_remove (client_file);
  g_rmdir (dir);
  g_free (client_file);
  return rc;
}
