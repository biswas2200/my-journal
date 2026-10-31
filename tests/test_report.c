/* Tests for the report addresses (src/shared/report.c), LeetCode style
 * (see cases.h).
 *
 * Constraints: each kind opens its own GitHub form; the version, the
 * system and the part arrive as they were given, whatever characters
 * they hold (URL-escaped, read back by the same rules GitHub uses); the
 * address never carries anything but those. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "report.h"

static void
test_urls (void)
{
  static const struct {
    const char *name;
    JrReportKind kind;
    const char *part, *version, *system;
    const char *template, *title;
  } t[] = {
    { "a bug in Notes",      JR_REPORT_BUG,  "Notes", "0.1.6", "Ubuntu 26.04 LTS · GTK 4.22.1", "bug_report.yml", "[Notes] " },
    { "a UI bug, no part",   JR_REPORT_UI,   NULL, "0.1.6", "Fedora 43", "ui_bug.yml", "" },
    { "an idea",             JR_REPORT_IDEA, "Start screen", "0.1.6", "Debian", "improvement.yml", "[Start screen] " },
    { "odd characters",      JR_REPORT_BUG,  "Notes & AI", "0.1.6+git#1", "a=b&c d/é?", "bug_report.yml", "[Notes & AI] " },
  };
  JrCases c = { "report url", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      g_autofree char *url = jr_report_url (t[i].kind, t[i].part, t[i].version, t[i].system);
      g_autoptr (GUri) u = g_uri_parse (url, G_URI_FLAGS_ENCODED_QUERY, NULL);
      g_autoptr (GHashTable) q = u != NULL ? g_uri_parse_params (g_uri_get_query (u), -1, "&", G_URI_PARAMS_NONE, NULL)
                                           : NULL;
      gboolean ok = u != NULL && g_str_has_prefix (url, JR_REPORT_REPO "/issues/new?") &&
                    g_strcmp0 (g_hash_table_lookup (q, "template"), t[i].template) == 0 &&
                    g_strcmp0 (g_hash_table_lookup (q, "version"), t[i].version) == 0 &&
                    g_strcmp0 (g_hash_table_lookup (q, "system"), t[i].system) == 0 &&
                    g_strcmp0 (g_hash_table_lookup (q, "title"), t[i].title) == 0 &&
                    g_hash_table_size (q) == 4;
      jr_case (&c, t[i].name, ok, "Got %s", url);
    }
  jr_cases_done (&c);
}

static void
test_system (void)
{
  g_autofree char *plain = jr_report_system (NULL);
  g_autofree char *with = jr_report_system ("GTK 4.22.1");
  g_assert_true (strstr (plain, "Linux ") != NULL);
  g_assert_true (g_str_has_suffix (with, " \xc2\xb7 GTK 4.22.1"));
  g_assert_true (g_utf8_validate (plain, -1, NULL));
  /* Nothing about the person: not the user name, not the home folder. */
  g_assert_null (strstr (plain, g_get_user_name ()));
  g_assert_null (strstr (plain, g_get_home_dir ()));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/report/urls", test_urls);
  g_test_add_func ("/report/system", test_system);
  return g_test_run ();
}
