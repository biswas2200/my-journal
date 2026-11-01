/* report: see report.h. */
#include "report.h"

#include <string.h>
#include <sys/utsname.h>

static void
param (GString *s, const char *key, const char *value)
{
  g_autofree char *v = g_uri_escape_string (value != NULL ? value : "", NULL, FALSE);
  g_string_append_printf (s, "%s%s=%s", s->len > 0 ? "&" : "", key, v);
}

char *
jr_report_url (JrReportKind kind, const char *part, const char *version, const char *system)
{
  static const char *const forms[] = { "bug_report.yml", "ui_bug.yml", "improvement.yml" };
  GString *q = g_string_new (NULL);
  param (q, "template", forms[kind]);
  g_autofree char *title = part != NULL ? g_strdup_printf ("[%s] ", part) : g_strdup ("");
  param (q, "title", title);
  param (q, "version", version);
  param (q, "system", system);
  g_autofree char *query = g_string_free (q, FALSE);
  return g_strconcat (JR_REPORT_REPO "/issues/new?", query, NULL);
}

/* PRETTY_NAME from one os-release file's text ("Ubuntu 26.04 LTS"), or
 * NULL. */
static char *
pretty_name (const char *text)
{
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  char *v = NULL;
  for (char **l = lines; *l != NULL && v == NULL; l++)
    if (g_str_has_prefix (*l, "PRETTY_NAME="))
      v = g_strstrip (g_strdup (*l + strlen ("PRETTY_NAME=")));
  gsize n = v != NULL ? strlen (v) : 0;
  if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0])
    {
      memmove (v, v + 1, n - 2);
      v[n - 2] = '\0';
    }
  return v;
}

/* The distribution's name, or NULL. (No cleanup attributes in the loop:
 * GCC 13's analyzer misreads them.) */
static char *
distribution (void)
{
  static const char *const files[] = { "/etc/os-release", "/usr/lib/os-release" };
  char *name = NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (files) && name == NULL; i++)
    {
      char *text = NULL;
      if (g_file_get_contents (files[i], &text, NULL, NULL))
        name = pretty_name (text);
      g_free (text);
    }
  return name;
}

char *
jr_report_system (const char *extra)
{
  GString *s = g_string_new (NULL);
  g_autofree char *distro = distribution ();
  if (distro != NULL && g_utf8_validate (distro, -1, NULL))
    g_string_append (s, distro);
  struct utsname u;
  if (uname (&u) == 0)
    g_string_append_printf (s, "%sLinux %s", s->len > 0 ? " \xc2\xb7 " : "", u.release);
  if (extra != NULL)
    g_string_append_printf (s, "%s%s", s->len > 0 ? " \xc2\xb7 " : "", extra);
  return g_string_free (s, FALSE);
}
