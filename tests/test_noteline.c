/* Tests for the Notes line formats (src/notes/core/noteline.c),
 * LeetCode style (see cases.h).
 *
 * A note is stored as a small Markdown subset, one line per block:
 *   "# t" heading      "## t" subheading     "- t" bullet
 *   "  - t" sub-bullet "→ t" arrow           anything else: text
 * A text line that would read as one of those is stored with a leading
 * backslash, so every line comes back exactly as it was written.
 *
 * Constraints: a line is any UTF-8 text without a newline, 0..10^5
 * bytes; Markdown typed by hand may use "*" or "+" for bullets, tabs or
 * 2+ spaces to indent, and "->" for the arrow. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "noteline.h"

static const char *
kind_name (JnKind k)
{
  switch (k)
    {
    case JN_TEXT:        return "text";
    case JN_HEADING:     return "heading";
    case JN_SUBHEADING:  return "subheading";
    case JN_BULLET:      return "bullet";
    case JN_SUB_BULLET:  return "sub-bullet";
    case JN_ARROW:       return "arrow";
    }
  return "?";
}

/* ---- reading a stored (or hand-typed Markdown) line ---------------- */

typedef struct {
  const char *name, *line;
  JnKind      kind;
  const char *text;
} ParseCase;

static const ParseCase parse_cases[] = {
  { "plain text",                 "prefix sums are cumulative", JN_TEXT, "prefix sums are cumulative" },
  { "empty line",                 "",              JN_TEXT,       "" },
  { "heading",                    "# Prefix sum",  JN_HEADING,    "Prefix sum" },
  { "subheading",                 "## Idea",       JN_SUBHEADING, "Idea" },
  { "empty heading",              "# ",            JN_HEADING,    "" },
  { "hash without space is text", "#hashtag",      JN_TEXT,       "#hashtag" },
  { "lone hash is text",          "#",             JN_TEXT,       "#" },
  { "three hashes are text",      "### deep",      JN_TEXT,       "### deep" },
  { "indented hash is text",      "  # not a heading", JN_TEXT,   "  # not a heading" },
  { "bullet",                     "- pre[0] = 0",  JN_BULLET,     "pre[0] = 0" },
  { "star bullet (Markdown)",     "* O(n) build",  JN_BULLET,     "O(n) build" },
  { "plus bullet (Markdown)",     "+ O(1) query",  JN_BULLET,     "O(1) query" },
  { "one space indent: bullet",   " - item",       JN_BULLET,     "item" },
  { "sub-bullet, 2 spaces",       "  - inner",     JN_SUB_BULLET, "inner" },
  { "sub-bullet, 4 spaces",       "    - inner",   JN_SUB_BULLET, "inner" },
  { "sub-bullet, tab",            "\t* inner",     JN_SUB_BULLET, "inner" },
  { "dash without space is text", "-5 degrees",    JN_TEXT,       "-5 degrees" },
  { "lone dash is text",          "-",             JN_TEXT,       "-" },
  { "arrow glyph",                "→ each query is O(1)", JN_ARROW, "each query is O(1)" },
  { "typed arrow",                "-> so it is O(1)", JN_ARROW,   "so it is O(1)" },
  { "arrow without space is text", "->x",          JN_TEXT,       "->x" },
  { "quote stays text",           "> a quote",     JN_TEXT,       "> a quote" },
  { "escaped heading",            "\\# not a heading", JN_TEXT,   "# not a heading" },
  { "escaped bullet",             "\\- not a bullet", JN_TEXT,    "- not a bullet" },
  { "escaped backslash",          "\\\\server",    JN_TEXT,       "\\server" },
  { "bullet of a bullet",         "- - x",         JN_BULLET,     "- x" },
  { "heading keeps inner hashes", "# # x",         JN_HEADING,    "# x" },
  { "unicode text",               "- टिप्पणी ✓",   JN_BULLET,     "टिप्पणी ✓" },
};

static void
test_parse (void)
{
  JrCases c = { "parse", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (parse_cases); i++)
    {
      const ParseCase *k = &parse_cases[i];
      const char *text = NULL;
      JnKind got = jn_line_parse (k->line, &text);
      gboolean ok = got == k->kind && g_strcmp0 (text, k->text) == 0;
      jr_case (&c, k->name, ok, "Input \"%s\"  Expected %s \"%s\"  Got %s \"%s\"", k->line, kind_name (k->kind),
               k->text, kind_name (got), text);
    }
  jr_cases_done (&c);
}

/* ---- writing a line ------------------------------------------------- */

typedef struct {
  const char *name;
  JnKind      kind;
  const char *text, *line;
} FormatCase;

static const FormatCase format_cases[] = {
  { "text",                       JN_TEXT,       "plain",            "plain" },
  { "heading",                    JN_HEADING,    "Prefix sum",       "# Prefix sum" },
  { "subheading",                 JN_SUBHEADING, "Idea",             "## Idea" },
  { "bullet",                     JN_BULLET,     "pre[0] = 0",       "- pre[0] = 0" },
  { "sub-bullet",                 JN_SUB_BULLET, "inner",            "  - inner" },
  { "arrow",                      JN_ARROW,      "O(1)",             "→ O(1)" },
  { "empty bullet",               JN_BULLET,     "",                 "- " },
  { "text that looks like a heading", JN_TEXT,   "# not a heading",  "\\# not a heading" },
  { "text that looks like a bullet",  JN_TEXT,   "* star",           "\\* star" },
  { "text that looks indented",   JN_TEXT,       "  - x",            "\\  - x" },
  { "text that looks like an arrow", JN_TEXT,    "-> x",             "\\-> x" },
  { "text starting with backslash", JN_TEXT,     "\\n is newline",   "\\\\n is newline" },
  { "hashtag needs no escape",    JN_TEXT,       "#tag",             "#tag" },
  { "newline becomes a space",    JN_BULLET,     "two\nlines",       "- two lines" },
};

static void
test_format (void)
{
  JrCases c = { "format", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (format_cases); i++)
    {
      const FormatCase *k = &format_cases[i];
      g_autofree char *got = jn_line_format (k->kind, k->text);
      jr_case (&c, k->name, g_strcmp0 (got, k->line) == 0, "Input %s \"%s\"  Expected \"%s\"  Got \"%s\"",
               kind_name (k->kind), k->text, k->line, got);
    }
  jr_cases_done (&c);
}

/* Hidden test: any kind and any text, built from the characters that
 * matter, comes back the same after format then parse. */
static void
test_round_trip_hidden (void)
{
  static const char *bits[] = { "#", " ", "-", "*", "+", "\\", "→", ">", "\t", "a", "é", "##", "->" };
  JrCases c = { "round trip (hidden)", 0, 0 };
  for (int i = 0; i < 20000; i++)
    {
      JnKind kind = (JnKind) g_test_rand_int_range (JN_TEXT, JN_ARROW + 1);
      GString *t = g_string_new (NULL);
      int n = g_test_rand_int_range (0, 7);
      for (int j = 0; j < n; j++)
        g_string_append (t, bits[g_test_rand_int_range (0, G_N_ELEMENTS (bits))]);
      g_autofree char *line = jn_line_format (kind, t->str);
      const char *back = NULL;
      JnKind got = jn_line_parse (line, &back);
      jr_case (&c, "random", got == kind && g_strcmp0 (back, t->str) == 0,
               "Input %s \"%s\"  Stored \"%s\"  Got %s \"%s\"", kind_name (kind), t->str, line, kind_name (got), back);
      g_string_free (t, TRUE);
    }
  jr_cases_done (&c);
}

/* A 100 000-byte line, in every kind, in linear time. */
static void
test_huge_line (void)
{
  g_autofree char *big = g_strnfill (100000, 'x');
  gint64 t0 = g_get_monotonic_time ();
  for (JnKind k = JN_TEXT; k <= JN_ARROW; k++)
    {
      g_autofree char *line = jn_line_format (k, big);
      const char *back = NULL;
      g_assert_cmpint (jn_line_parse (line, &back), ==, k);
      g_assert_cmpstr (back, ==, big);
    }
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 200000); /* well under 0.2 s */
}

/* ---- typing shortcuts at the start of a line ----------------------- */

typedef struct {
  const char *name, *typed;
  JnKind      kind;
  gsize       prefix;
} ShortcutCase;

static const ShortcutCase shortcut_cases[] = {
  { "dash space",      "- ",   JN_BULLET,     2 },
  { "star space",      "* ",   JN_BULLET,     2 },
  { "hash space",      "# ",   JN_HEADING,    2 },
  { "two hashes",      "## ",  JN_SUBHEADING, 3 },
  { "greater-than",    "> ",   JN_ARROW,      2 },
  { "dash arrow",      "-> ",  JN_ARROW,      3 },
  { "arrow glyph",     "→ ",   JN_ARROW,      strlen ("→ ") },
  { "indented dash",   "  - ", JN_SUB_BULLET, 4 },
  { "no space yet",    "-",    JN_TEXT,       0 },
  { "words first",     "a - ", JN_TEXT,       0 },
  { "three hashes",    "### ", JN_TEXT,       0 },
  { "empty",           "",     JN_TEXT,       0 },
  { "space only",      " ",    JN_TEXT,       0 },
};

static void
test_shortcuts (void)
{
  JrCases c = { "shortcut", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (shortcut_cases); i++)
    {
      const ShortcutCase *k = &shortcut_cases[i];
      gsize prefix = 99;
      JnKind got = jn_line_shortcut (k->typed, &prefix);
      jr_case (&c, k->name, got == k->kind && prefix == k->prefix, "Input \"%s\"  Expected %s/%zu  Got %s/%zu",
               k->typed, kind_name (k->kind), k->prefix, kind_name (got), prefix);
    }
  jr_cases_done (&c);
}

/* ---- Tab and Shift+Tab, and what the editor shows ------------------- */

static void
test_indent_and_glyphs (void)
{
  JrCases c = { "indent", 0, 0 };
  static const struct { JnKind from, in, out; const char *glyph; } t[] = {
    { JN_TEXT,       JN_TEXT,       JN_TEXT,   "" },
    { JN_HEADING,    JN_HEADING,    JN_HEADING, "" },
    { JN_SUBHEADING, JN_SUBHEADING, JN_SUBHEADING, "" },
    { JN_BULLET,     JN_SUB_BULLET, JN_TEXT,   "● " },
    { JN_SUB_BULLET, JN_SUB_BULLET, JN_BULLET, "◦ " },
    { JN_ARROW,      JN_ARROW,      JN_TEXT,   "→ " },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      jr_case (&c, kind_name (t[i].from), jn_kind_indent (t[i].from) == t[i].in, "Tab  Expected %s  Got %s",
               kind_name (t[i].in), kind_name (jn_kind_indent (t[i].from)));
      jr_case (&c, kind_name (t[i].from), jn_kind_outdent (t[i].from) == t[i].out, "Shift+Tab  Expected %s  Got %s",
               kind_name (t[i].out), kind_name (jn_kind_outdent (t[i].from)));
      jr_case (&c, kind_name (t[i].from), g_strcmp0 (jn_kind_glyph (t[i].from), t[i].glyph) == 0,
               "glyph  Expected \"%s\"  Got \"%s\"", t[i].glyph, jn_kind_glyph (t[i].from));
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/noteline/parse", test_parse);
  g_test_add_func ("/noteline/format", test_format);
  g_test_add_func ("/noteline/round-trip-hidden", test_round_trip_hidden);
  g_test_add_func ("/noteline/huge-line", test_huge_line);
  g_test_add_func ("/noteline/shortcuts", test_shortcuts);
  g_test_add_func ("/noteline/indent-and-glyphs", test_indent_and_glyphs);
  return g_test_run ();
}
