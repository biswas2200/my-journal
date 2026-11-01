/* Tests for Notes export (src/notes/core/noteexport.c), LeetCode style
 * (see cases.h): the time a session took, the export file name, and a
 * topic as one Markdown file, read the same by any Markdown reader
 * (Google Docs among them): every line of a note stays its own line,
 * and nothing in it is taken for web code and dropped.
 *
 * Constraints: times are Unix seconds in the local time zone (tests run
 * in Asia/Kolkata, UTC+5:30, and in a zone with summer time); names are
 * any UTF-8, including "/" and dots. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include "cases.h"
#include "noteexport.h"
#include "noteline.h"
#include "notesdb.h"

/* 2026-10-28 14:05:00 in UTC+5:30 */
#define T1405 G_GINT64_CONSTANT (1793176500)
#define MINUTE 60

static void
test_span (void)
{
  static const struct { const char *name; gint64 started, ended; const char *want; } t[] = {
    { "a study session",         T1405, T1405 + 95 * MINUTE,  "28 Oct 2026, 14:05–15:40" },
    { "no time yet",             T1405, T1405,             "28 Oct 2026, 14:05" },
    { "under a minute",          T1405, T1405 + 40,        "28 Oct 2026, 14:05" },
    { "past midnight",           T1405 + 9 * 60 * MINUTE + 45 * MINUTE, T1405 + 10 * 60 * MINUTE + 25 * MINUTE,
                                 "28 Oct 2026, 23:50 – 29 Oct 2026, 00:30" },
    { "morning, single digits",  T1405 - 5 * 60 * MINUTE - 5 * MINUTE, T1405 - 4 * 60 * MINUTE, "28 Oct 2026, 09:00–10:05" },
    { "end before start (clock)",T1405, T1405 - 10 * MINUTE,  "28 Oct 2026, 14:05" },
  };
  JrCases c = { "span", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      g_autofree char *got = jn_format_span (t[i].started, t[i].ended);
      jr_case (&c, t[i].name, g_strcmp0 (got, t[i].want) == 0, "Expected \"%s\"  Got \"%s\"", t[i].want, got);
    }
  jr_cases_done (&c);
}

static void
test_file_name (void)
{
  static const struct { const char *name, *topic, *want; } t[] = {
    { "plain",              "Prefix sum",        "Prefix sum.md" },
    { "slash",              "TCP/IP",            "TCP-IP.md" },
    { "backslash",          "a\\b",              "a-b.md" },
    { "dots only",          "..",                "Untitled.md" },
    { "hidden file",        ".bashrc",           "bashrc.md" },
    { "empty",              "",                  "Untitled.md" },
    { "spaces trimmed",     "  Graphs  ",        "Graphs.md" },
    { "control characters", "a\tb\nc",           "a b c.md" },
    { "reserved characters",  "a:b*c?\"<>|",     "a-b-c-----.md" },
    { "unicode kept",       "टिप्पणी ✓",          "टिप्पणी ✓.md" },
    { "very long",          NULL,                NULL },
  };
  JrCases c = { "file name", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      if (t[i].topic == NULL)
        {
          g_autofree char *big = g_strnfill (1000, 'x');
          g_autofree char *got = jn_export_file_name (big);
          jr_case (&c, t[i].name, strlen (got) <= 200 && g_str_has_suffix (got, ".md"),
                   "Expected at most 200 bytes ending .md  Got %zu bytes", strlen (got));
          continue;
        }
      g_autofree char *got = jn_export_file_name (t[i].topic);
      jr_case (&c, t[i].name, g_strcmp0 (got, t[i].want) == 0, "Input \"%s\"  Expected \"%s\"  Got \"%s\"",
               t[i].topic, t[i].want, got);
    }
  /* Hidden: no name ever escapes the folder or hides itself. */
  static const char *bits[] = { "/", ".", "..", "\\", "a", " ", "\n", "\x01", "é", ":" };
  for (int i = 0; i < 5000; i++)
    {
      GString *s = g_string_new (NULL);
      for (int j = g_test_rand_int_range (0, 8); j > 0; j--)
        g_string_append (s, bits[g_test_rand_int_range (0, G_N_ELEMENTS (bits))]);
      g_autofree char *got = jn_export_file_name (s->str);
      gboolean ok = strchr (got, '/') == NULL && got[0] != '.' && g_str_has_suffix (got, ".md") &&
                    g_utf8_validate (got, -1, NULL) && strchr (got, '\n') == NULL;
      jr_case (&c, "random", ok, "Input \"%s\"  Got \"%s\"", s->str, got);
      g_string_free (s, TRUE);
    }
  jr_cases_done (&c);
}

static void
test_topic (void)
{
  JnExport *e = jn_export_new ("DSA", "Prefix sum");
  jn_export_add_session (e, T1405, T1405 + 95 * MINUTE, "https://cp-algorithms.com/prefix\n\nhttps://youtu.be/x\n",
                         "# Idea\n- pre[0] = 0\n  - build once\n→ each query is O(1)\n## Doubts\nwhat about 2D?");
  jn_export_add_session (e, T1405 + 86400, T1405 + 86400 + 30 * MINUTE, "", "- 2D: pre[i][j]");
  g_autofree char *md = jn_export_finish (e);
  static const char want[] =
    "# Prefix sum\n"
    "\n"
    "Subject: DSA\n"
    "\n"
    "## 28 Oct 2026, 14:05–15:40\n"
    "\n"
    "Sources:\n"
    "- <https://cp-algorithms.com/prefix>\n"
    "- <https://youtu.be/x>\n"
    "\n"
    "### Idea\n"           /* the note's own headings move down two levels */
    "- pre[0] = 0\n"
    "  - build once\n"
    "\n"                   /* else the arrow would join the bullet above */
    "→ each query is O(1)\n"
    "#### Doubts\n"
    "what about 2D?\n"
    "\n"
    "## 29 Oct 2026, 14:05–14:35\n"
    "\n"
    "- 2D: pre[i][j]\n";
  g_assert_cmpstr (md, ==, want);

  /* No sessions yet: just the title. */
  e = jn_export_new ("OS", "Paging");
  g_autofree char *empty = jn_export_finish (e);
  g_assert_cmpstr (empty, ==, "# Paging\n\nSubject: OS\n");

  /* Time studied with the stopwatch or timer goes under the subject;
   * none, no line. */
  e = jn_export_new ("OS", "Paging");
  jn_export_set_studied (e, 3 * 3600 + 10 * MINUTE);
  g_autofree char *timed = jn_export_finish (e);
  g_assert_cmpstr (timed, ==, "# Paging\n\nSubject: OS\n\nTime studied: 3 h 10 min\n");
  e = jn_export_new ("OS", "Paging");
  jn_export_set_studied (e, 0);
  g_autofree char *untimed = jn_export_finish (e);
  g_assert_cmpstr (untimed, ==, "# Paging\n\nSubject: OS\n");

  /* A text line that looks like a heading stays escaped. */
  e = jn_export_new ("A", "B");
  jn_export_add_session (e, T1405, T1405, "", "\\# not a heading");
  g_autofree char *esc = jn_export_finish (e);
  g_assert_nonnull (strstr (esc, "\n\\# not a heading\n"));
}

/* A topic reference: every enhanced copy of the topic, oldest first,
 * under its session's time; their titles and header lines go, their
 * headings move down a level, their sources stay with them. */
static void
test_reference (void)
{
  JnExport *e = jn_export_reference_new ("DSA", "Prefix sum");
  jn_export_reference_add (e, T1405, T1405 + 95 * MINUTE,
    "# Prefix sum (enhanced)\n\nDSA \xc2\xb7 studied 28 Oct 2026, 14:05–15:40 \xc2\xb7 written by m from your note\n\n"
    "Sources:\n1. <https://a.example/x>\n\n## Idea\n- pre[0] = 0 [1]\n\n## Gaps\n- 2D\n");
  jn_export_reference_add (e, T1405 + 86400, T1405 + 86400 + 30 * MINUTE,
    "# Prefix sum (enhanced)\n\nDSA \xc2\xb7 studied 29 Oct · written by m from your note\n\n## 2D\n- pre[i][j] [1]\n");
  g_autofree char *md = jn_export_finish (e);
  static const char want[] =
    "# Prefix sum (reference)\n"
    "\n"
    "DSA \xc2\xb7 2 sessions with an enhanced copy, written by the AI from your notes\n"
    "\n"
    "## 28 Oct 2026, 14:05–15:40\n"
    "\n"
    "Sources:\n"
    "1. <https://a.example/x>\n"
    "\n"
    "### Idea\n"
    "- pre[0] = 0 [1]\n"
    "\n"
    "### Gaps\n"
    "- 2D\n"
    "\n"
    "## 29 Oct 2026, 14:05–14:35\n"
    "\n"
    "### 2D\n"
    "- pre[i][j] [1]\n";
  g_assert_cmpstr (md, ==, want);
  JnExport *none = jn_export_reference_new ("DSA", "Empty");
  g_autofree char *empty = jn_export_finish (none);
  g_assert_cmpstr (empty, ==, "# Empty (reference)\n\nDSA \xc2\xb7 no enhanced copies yet\n");
}

/* A topic straight from the store: the same text as building it by hand
 * (what Export .md writes and the Drive backup sends). */
static void
test_from_store (void)
{
  JnDb *db = jn_db_open (":memory:", NULL);
  gint64 dsa = jn_db_add_subject (db, "DSA");
  gint64 topic = jn_db_add_topic (db, dsa, "Prefix sum");
  gint64 later = jn_db_start_session (db, topic, 1793600000);
  gint64 first = jn_db_start_session (db, topic, 1793500000);
  jn_db_save_session (db, first, "# Idea\n- pre[0] = 0", "https://a.example", 1793501500); /* within 30 min */
  jn_db_save_session (db, later, "- more", "", 1793601800);
  jn_db_add_study (db, topic, 1793500100, 1500);

  JnExport *e = jn_export_new ("DSA", "Prefix sum");
  jn_export_set_studied (e, 1500);
  jn_export_add_session (e, 1793500000, 1793501500, "https://a.example", "# Idea\n- pre[0] = 0");
  jn_export_add_session (e, 1793600000, 1793601800, "", "- more");
  g_autofree char *want = jn_export_finish (e);
  g_autofree char *got = jn_export_topic (db, topic);
  g_assert_cmpstr (got, ==, want);
  g_assert_null (jn_export_topic (db, 999));
  jn_db_close (db);
}

/* One session's note as it lands in the export: every line its own
 * line in any Markdown reader. A plain or arrow line right after a
 * bullet or another plain line would be read as part of it, so a blank
 * line goes between; "<tag" and "&name;" would be read as web code and
 * dropped, so they are escaped. Code blocks and tables are left alone.
 *
 * Constraints: a note is lines of the six kinds (noteline.h), any text. */
static void
test_markdown_lines (void)
{
  static const struct {
    const char *name, *note, *want;
  } cases[] = {
    { "arrow after a sub-bullet", "- a\n  - b\n\xe2\x86\x92 c", "- a\n  - b\n\n\xe2\x86\x92 c\n" },
    { "text after a bullet", "- a\nplain", "- a\n\nplain\n" },
    { "two text lines", "one\ntwo", "one\n\ntwo\n" },
    { "two arrows", "\xe2\x86\x92 a\n\xe2\x86\x92 b", "\xe2\x86\x92 a\n\n\xe2\x86\x92 b\n" },
    { "bullet after text", "plain\n- a", "plain\n- a\n" },
    { "heading after a list", "- a\n## Next", "- a\n#### Next\n" },
    { "text after a heading", "# Idea\nplain", "### Idea\nplain\n" },
    { "a blank line is not doubled", "- a\n\nplain", "- a\n\nplain\n" },
    { "escaped marker after a bullet", "- a\n\\# not a heading", "- a\n\n\\# not a heading\n" },
    { "code block untouched", "- a\n```\nx < y\n<b>\n\n```\nafter", "- a\n\n```\nx < y\n<b>\n\n```\n\nafter\n" },
    { "table rows stay together", "| a | b |\n|---|---|\n| 1 | 2 |", "| a | b |\n|---|---|\n| 1 | 2 |\n" },
    { "a tag is escaped", "- <tags> and <b>x</b>", "- \\<tags> and \\<b>x\\</b>\n" },
    { "a web address in <> stays a link", "see <https://a.org/x>", "see <https://a.org/x>\n" },
    { "less-than in maths stays", "- 1 < 2 and l<r", "- 1 < 2 and l<r\n" },
    { "a tag inside code stays", "- `<b>` and <i>", "- `<b>` and \\<i>\n" },
    { "an entity is escaped", "- &amp; &#169; AT&T", "- \\&amp; \\&#169; AT&T\n" },
  };
  JrCases c = { "markdown lines", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      JnExport *e = jn_export_new ("A", "B");
      jn_export_add_session (e, T1405, T1405, "", cases[i].note);
      g_autofree char *md = jn_export_finish (e);
      const char *body = strstr (md, "14:05\n\n");
      body = body != NULL ? body + strlen ("14:05\n\n") : "";
      jr_case (&c, cases[i].name, g_strcmp0 (body, cases[i].want) == 0, "Input \"%s\"  Expected \"%s\"  Got \"%s\"",
               cases[i].note, cases[i].want, body);
    }
  jr_cases_done (&c);
}

/* Lines a reader could fold into the line before: plain or arrow text
 * right after a bullet, a plain line or an arrow. */
static gboolean
foldable (const char *line)
{
  const char *text;
  JnKind k = jn_line_parse (line, &text); /* also "* a", " + a": bullets */
  return *line != '\0' && line[0] != '#' && (k == JN_TEXT || k == JN_ARROW);
}

/* Hidden: random notes. No line can fold into the one before, and taking
 * out the blank lines and escapes the export added gives the note back. */
static void
test_markdown_lines_hidden (void)
{
  static const char *const kinds[] = { "", "- ", "  - ", "\xe2\x86\x92 ", "# ", "## ", "\\# " };
  static const char *const bits[] = { "a", "b", " ", "<", "<b>", "&amp;", "&", "x<y", "`", "[1]", "*", "\xc3\xa9" };
  JrCases c = { "markdown lines (hidden)", 0, 0 };
  for (int round = 0; round < 2000; round++)
    {
      GString *note = g_string_new (NULL);
      for (int n = g_test_rand_int_range (1, 9); n > 0; n--)
        {
          if (note->len > 0)
            g_string_append_c (note, '\n');
          g_string_append (note, kinds[g_test_rand_int_range (0, G_N_ELEMENTS (kinds))]);
          for (int k = g_test_rand_int_range (0, 6); k > 0; k--)
            g_string_append (note, bits[g_test_rand_int_range (0, G_N_ELEMENTS (bits))]);
        }
      JnExport *e = jn_export_new ("A", "B");
      jn_export_add_session (e, T1405, T1405, "", note->str);
      g_autofree char *md = jn_export_finish (e);
      const char *body = strstr (md, "14:05\n\n") + strlen ("14:05\n\n");
      g_auto (GStrv) out = g_strsplit (body, "\n", -1);
      gboolean fences = strchr (note->str, '`') != NULL; /* a ``` may open a block: no rule inside */
      gboolean folds = FALSE;
      for (int i = 1; out[0] != NULL && out[i] != NULL && !fences; i++) /* an empty body has no line at all */
        folds = folds || (foldable (out[i]) && *out[i - 1] != '\0' && !g_str_has_prefix (out[i - 1], "#"));
      /* Back to the note: blank lines out, escapes out, headings up. */
      GString *back = g_string_new (NULL);
      for (int i = 0; out[i] != NULL; i++)
        {
          if (*out[i] == '\0')
            continue;
          g_autofree char *line = g_strdup (out[i]);
          if (g_str_has_prefix (line, "### ") || g_str_has_prefix (line, "#### "))
            memmove (line, line + 2, strlen (line + 2) + 1);
          GString *l = g_string_new (NULL);
          for (const char *p = line; *p != '\0'; p++)
            if (!(p[0] == '\\' && (p[1] == '<' || p[1] == '&')))
              g_string_append_c (l, *p);
          if (back->len > 0)
            g_string_append_c (back, '\n');
          g_string_append (back, l->str);
          g_string_free (l, TRUE);
        }
      GString *want = g_string_new (NULL);
      g_auto (GStrv) in = g_strsplit (note->str, "\n", -1);
      for (int i = 0; in[i] != NULL; i++)
        if (*in[i] != '\0')
          g_string_append_printf (want, "%s%s", want->len > 0 ? "\n" : "", in[i]);
      jr_case (&c, "random note", !folds && strcmp (back->str, want->str) == 0, "Input \"%s\"  Got \"%s\"  folds=%d",
               note->str, body, folds);
      g_string_free (back, TRUE);
      g_string_free (want, TRUE);
      g_string_free (note, TRUE);
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Asia/Kolkata", TRUE);
  tzset ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/noteexport/span", test_span);
  g_test_add_func ("/noteexport/file-name", test_file_name);
  g_test_add_func ("/noteexport/topic", test_topic);
  g_test_add_func ("/noteexport/markdown-lines", test_markdown_lines);
  g_test_add_func ("/noteexport/hidden/markdown-lines", test_markdown_lines_hidden);
  g_test_add_func ("/noteexport/reference", test_reference);
  g_test_add_func ("/noteexport/from-store", test_from_store);
  return g_test_run ();
}
