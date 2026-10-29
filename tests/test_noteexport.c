/* Tests for Notes export (src/notes/core/noteexport.c), LeetCode style
 * (see cases.h): the time a session took, the export file name, and a
 * topic as one Markdown file.
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

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Asia/Kolkata", TRUE);
  tzset ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/noteexport/span", test_span);
  g_test_add_func ("/noteexport/file-name", test_file_name);
  g_test_add_func ("/noteexport/topic", test_topic);
  g_test_add_func ("/noteexport/reference", test_reference);
  return g_test_run ();
}
