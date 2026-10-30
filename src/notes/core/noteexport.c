/* noteexport: see noteexport.h. */
#include "noteexport.h"

#include <string.h>
#include "noteline.h"
#include "studyclock.h"

#define MAX_NAME 200

struct JnExport {
  GString *md;
  /* A topic reference: its header is written at the end. */
  gboolean reference;
  char    *subject, *topic;
  int      copies;
};

static char *
local (gint64 t, const char *format)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  return d != NULL ? g_date_time_format (d, format) : g_strdup ("?");
}

char *
jn_format_span (gint64 started, gint64 ended)
{
  g_autofree char *day = local (started, "%-d %b %Y");
  g_autofree char *from = local (started, "%H:%M");
  if (ended - started < 60)
    return g_strdup_printf ("%s, %s", day, from);
  g_autofree char *end_day = local (ended, "%-d %b %Y");
  g_autofree char *to = local (ended, "%H:%M");
  if (strcmp (day, end_day) != 0)
    return g_strdup_printf ("%s, %s – %s, %s", day, from, end_day, to);
  return g_strdup_printf ("%s, %s–%s", day, from, to);
}

char *
jn_export_file_name (const char *topic)
{
  GString *s = g_string_new (NULL);
  for (const char *p = topic; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar ch = g_utf8_get_char_validated (p, -1);
      if (ch == (gunichar) -1 || ch == (gunichar) -2)
        break;
      if (ch < 0x20 || ch == 0x7f)
        g_string_append_c (s, ' ');
      else if (strchr ("/\\:*?\"<>|", (int) ch) != NULL && ch < 0x80)
        g_string_append_c (s, '-');
      else
        g_string_append_unichar (s, ch);
    }
  g_strstrip (s->str);
  const char *start = s->str;
  while (*start == '.' || *start == ' ')
    start++; /* no hidden files, no "..", also after spaces (". .") */
  g_autofree char *name = g_strstrip (g_strdup (start));
  g_string_free (s, TRUE);
  /* Cut at a character boundary so the name with ".md" fits. */
  if (strlen (name) > MAX_NAME - 3)
    {
      const char *end = name;
      while (*end != '\0' && (gsize) (g_utf8_next_char (end) - name) <= MAX_NAME - 3)
        end = g_utf8_next_char (end);
      name[end - name] = '\0';
    }
  if (*name == '\0')
    return g_strdup ("Untitled.md");
  return g_strconcat (name, ".md", NULL);
}

char *
jn_export_dir (void)
{
  const char *docs = g_get_user_special_dir (G_USER_DIRECTORY_DOCUMENTS);
  g_autofree char *home_docs = g_build_filename (g_get_home_dir (), "Documents", NULL);
  return g_build_filename (docs != NULL ? docs : home_docs, "Notes", NULL);
}

JnExport *
jn_export_new (const char *subject, const char *topic)
{
  JnExport *e = g_new0 (JnExport, 1);
  g_autofree char *t = jn_line_format (JN_TEXT, topic);
  g_autofree char *s = jn_line_format (JN_TEXT, subject);
  e->md = g_string_new (NULL);
  g_string_append_printf (e->md, "# %s\n\nSubject: %s\n", t, s);
  return e;
}

void
jn_export_set_studied (JnExport *e, gint64 seconds)
{
  if (seconds <= 0)
    return;
  g_autofree char *t = jn_format_studied (seconds);
  g_string_append_printf (e->md, "Time studied: %s\n", t);
}

void
jn_export_add_session (JnExport *e, gint64 started, gint64 ended, const char *sources, const char *body)
{
  g_autofree char *span = jn_format_span (started, ended);
  g_string_append_printf (e->md, "\n## %s\n\n", span);
  g_auto (GStrv) links = g_strsplit (sources != NULL ? sources : "", "\n", -1);
  gboolean any = FALSE;
  for (char **l = links; *l != NULL; l++)
    {
      g_strstrip (*l);
      if (**l == '\0')
        continue;
      if (!any)
        g_string_append (e->md, "Sources:\n");
      any = TRUE;
      g_string_append_printf (e->md, "- <%s>\n", *l);
    }
  if (any)
    g_string_append_c (e->md, '\n');
  g_auto (GStrv) lines = g_strsplit (body != NULL ? body : "", "\n", -1);
  for (char **l = lines; *l != NULL; l++)
    {
      const char *text;
      JnKind k = jn_line_parse (*l, &text);
      if (k == JN_HEADING)
        g_string_append_printf (e->md, "### %s\n", text);
      else if (k == JN_SUBHEADING)
        g_string_append_printf (e->md, "#### %s\n", text);
      else
        g_string_append_printf (e->md, "%s\n", *l);
    }
}

JnExport *
jn_export_reference_new (const char *subject, const char *topic)
{
  JnExport *e = g_new0 (JnExport, 1);
  e->md = g_string_new (NULL);
  e->reference = TRUE;
  e->subject = g_strdup (subject);
  e->topic = g_strdup (topic);
  return e;
}

void
jn_export_reference_add (JnExport *e, gint64 started, gint64 ended, const char *enhanced)
{
  g_autofree char *span = jn_format_span (started, ended);
  g_string_append_printf (e->md, "\n## %s\n\n", span);
  e->copies++;
  g_auto (GStrv) lines = g_strsplit (enhanced != NULL ? enhanced : "", "\n", -1);
  int i = 0;
  if (lines[0] != NULL && strncmp (lines[0], "# ", 2) == 0)
    i++; /* the copy's own title */
  while (lines[i] != NULL && *lines[i] == '\0')
    i++;
  if (lines[i] != NULL && strstr (lines[i], "written by") != NULL)
    {
      i++; /* its header line: the reference says it once */
      while (lines[i] != NULL && *lines[i] == '\0')
        i++;
    }
  GString *body = g_string_new (NULL);
  gboolean fence = FALSE;
  for (; lines[i] != NULL; i++)
    {
      const char *l = lines[i];
      const char *t = l;
      while (*t == ' ')
        t++;
      if (strncmp (t, "```", 3) == 0 || strncmp (t, "~~~", 3) == 0)
        fence = !fence;
      if (!fence && l[0] == '#' && strspn (l, "#") < 6 && l[strspn (l, "#")] == ' ')
        g_string_append_c (body, '#'); /* one level down */
      g_string_append (body, l);
      g_string_append_c (body, '\n');
    }
  /* One line end at the end, no trailing blank lines. */
  while (body->len >= 2 && body->str[body->len - 1] == '\n' && body->str[body->len - 2] == '\n')
    g_string_truncate (body, body->len - 1);
  g_string_append (e->md, body->str);
  g_string_free (body, TRUE);
}

char *
jn_export_finish (JnExport *e)
{
  if (e->reference)
    {
      g_autofree char *t = jn_line_format (JN_TEXT, e->topic);
      GString *head = g_string_new (NULL);
      g_string_append_printf (head, "# %s (reference)\n\n", t);
      if (e->copies == 0)
        g_string_append_printf (head, "%s \xc2\xb7 no enhanced copies yet\n", e->subject);
      else
        g_string_append_printf (head, "%s \xc2\xb7 %d %s with an enhanced copy, written by the AI from your notes\n",
                                e->subject, e->copies, e->copies == 1 ? "session" : "sessions");
      g_string_prepend (e->md, head->str);
      g_string_free (head, TRUE);
      g_free (e->subject);
      g_free (e->topic);
    }
  char *md = g_string_free (e->md, FALSE);
  g_free (e);
  return md;
}
