/* Tests for reading Markdown to show it (src/notes/core/markdown.c),
 * LeetCode style (see cases.h).
 *
 * The input is what the AI wrote (or an export): any text, often a bit
 * off (a code fence inside a list, an unclosed fence, odd indents).
 * Output: blocks (headings, paragraphs, bullets with their depth,
 * numbered items, arrows, code with its language, SVG, rules) and, inside
 * a line, spans (bold, italic, code, links, [n] citations). Nothing is
 * ever lost: every letter shows up in some block or span (markup
 * characters may go). */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "markdown.h"

static char *
blocks_str (const char *md)
{
  GPtrArray *b = jn_md_blocks (md);
  GString *s = g_string_new (NULL);
  static const char *names[] = { "p", "h", "ul", "ol", "arrow", "code", "svg", "rule", "blank" };
  for (guint i = 0; i < b->len; i++)
    {
      JnMdBlock *k = b->pdata[i];
      if (i > 0)
        g_string_append (s, " | ");
      g_string_append_printf (s, "%s%d", names[k->kind], k->level);
      if (k->lang != NULL)
        g_string_append_printf (s, "(%s)", k->lang);
      if (k->text != NULL && k->kind != JN_MD_BLANK && k->kind != JN_MD_RULE)
        g_string_append_printf (s, ":%s", k->text);
    }
  g_ptr_array_unref (b);
  return g_string_free (s, FALSE);
}

typedef struct {
  const char *name, *md, *want;
} BlockCase;

static const BlockCase block_cases[] = {
  { "paragraph",          "plain words",                       "p0:plain words" },
  { "headings",           "# A\n## B\n### C\n###### F",        "h1:A | h2:B | h3:C | h6:F" },
  { "seven hashes are text", "####### x",                       "p0:####### x" },
  { "hash without space", "#tag",                              "p0:#tag" },
  { "bullets and depth",  "- a\n  - b\n    - c\n* d\n+ e",     "ul0:a | ul1:b | ul2:c | ul0:d | ul0:e" },
  { "tab indent",         "- a\n\t- b",                        "ul0:a | ul2:b" },
  { "deep indent capped", "            - x",                   "ul4:x" },
  { "numbered",           "1. one\n2) two\n  3. three",        "ol1:one | ol2:two | ol3:three" },
  { "arrows",             "→ so\n-> also\n  → inner",          "arrow0:so | arrow0:also | arrow1:inner" },
  { "code fence",         "```python\ndef f():\n    return 1\n```", "code0(python):def f():\n    return 1" },
  { "code without language", "```\nx = 1\n```",               "code0:x = 1" },
  { "fence inside a list","- build:\n```text\npre[i] = pre[i-1] + a[i]\n```\n    [1]",
                          "ul0:build: | code0(text):pre[i] = pre[i-1] + a[i] | p0:[1]" },
  { "indented fence",     "    ```c\n    int x;\n    ```",     "code0(c):int x;" },
  { "unclosed fence",     "```\nlast\nlines",                  "code0:last\nlines" },
  { "svg block",          "```svg\n<svg width=\"10\"></svg>\n```", "svg0:<svg width=\"10\"></svg>" },
  { "tildes fence",       "~~~\ncode\n~~~",                     "code0:code" },
  { "a fence with text after it is code", "```\n```not closing\nx\n```", "code0:```not closing\nx" },
  { "a non-language info line is kept", "```see below\nx\n```",     "code0:see below\nx" },
  { "rule",               "a\n---\nb",                          "p0:a | rule0 | p0:b" },
  { "stars rule",         "***",                                "rule0" },
  { "blank lines collapse", "a\n\n\n\nb",                       "p0:a | blank0 | p0:b" },
  { "escaped heading",    "\\# not a heading",                  "p0:# not a heading" },
  { "escaped bullet",     "\\- not a bullet",                   "p0:- not a bullet" },
  { "empty",              "",                                   "" },
  { "only blanks",        "\n\n",                               "" },
  { "trailing spaces kept out", "## Idea   ",                    "h2:Idea" },
  { "windows line ends",  "# A\r\n- b\r\n",                     "h1:A | ul0:b" },
  { "broken UTF-8 replaced (fuzzing)", "- a\xf0\n\xe2\x86", "ul0:a\xef\xbf\xbd | p0:\xef\xbf\xbd\xef\xbf\xbd" },
};

static void
test_blocks (void)
{
  JrCases c = { "blocks", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (block_cases); i++)
    {
      g_autofree char *got = blocks_str (block_cases[i].md);
      jr_case (&c, block_cases[i].name, g_strcmp0 (got, block_cases[i].want) == 0, "Input \"%s\"  Expected \"%s\"  Got \"%s\"",
               block_cases[i].md, block_cases[i].want, got);
    }
  jr_cases_done (&c);
}

static char *
spans_str (const char *text)
{
  GPtrArray *sp = jn_md_spans (text);
  GString *s = g_string_new (NULL);
  static const char *names[] = { "t", "b", "i", "c", "link", "cite" };
  for (guint i = 0; i < sp->len; i++)
    {
      JnMdSpan *k = sp->pdata[i];
      if (i > 0)
        g_string_append (s, " ");
      g_string_append_printf (s, "%s[%s]", names[k->kind], k->text);
      if (k->url != NULL)
        g_string_append_printf (s, "(%s)", k->url);
    }
  g_ptr_array_unref (sp);
  return g_string_free (s, FALSE);
}

typedef struct {
  const char *name, *text, *want;
} SpanCase;

static const SpanCase span_cases[] = {
  { "plain",              "just text",                    "t[just text]" },
  { "bold",               "a **b** c",                    "t[a ] b[b] t[ c]" },
  { "bold underscores",   "__b__",                        "b[b]" },
  { "italic",             "an *i* word",                  "t[an ] i[i] t[ word]" },
  { "italic underscore",  "_i_",                          "i[i]" },
  { "snake_case stays",   "my_var_name",                  "t[my_var_name]" },
  { "inline code",        "use `pre[r+1] - pre[l]` here", "t[use ] c[pre[r+1] - pre[l]] t[ here]" },
  { "stars inside code",  "`a*b*c`",                      "c[a*b*c]" },
  { "citation",           "a fact [1]",                   "t[a fact ] cite[1]" },
  { "two citations",      "both [1, 2]",                  "t[both ] cite[1, 2]" },
  { "brackets not a citation", "arr[i] and [x]",          "t[arr[i] and [x]]" },
  { "an index is not a citation", "pre[0] = 0 and a[1]",  "t[pre[0] = 0 and a[1]]" },
  { "citation at the start", "[2] says so",               "cite[2] t[ says so]" },
  { "citation after a dot", "it is O(1).[3]",              "t[it is O(1).] cite[3]" },
  { "link",               "see [USACO](https://usaco.guide/x)", "t[see ] link[USACO](https://usaco.guide/x)" },
  { "autolink",           "<https://a.example/b>",        "link[https://a.example/b](https://a.example/b)" },
  { "javascript link is text", "[x](javascript:alert(1))", "t[[x](javascript:alert(1))]" },
  { "a link with no text shows its address (hidden test)", "see [](https://a.example/x) ok",
                          "t[see ] link[https://a.example/x](https://a.example/x) t[ ok]" },
  { "file link is text",  "[x](file:///etc/passwd)",      "t[[x](file:///etc/passwd)]" },
  { "unclosed bold",      "a **b",                        "t[a **b]" },
  { "unclosed code",      "a `b",                         "t[a `b]" },
  { "lone star",          "2 * 3 = 6",                    "t[2 * 3 = 6]" },
  { "escapes",            "\\*not italic\\*",             "t[*not italic*]" },
  { "empty",              "",                             "" },
  { "inline math",        "for $1 \\le k \\le N$",        "t[for ] c[1 ≤ k ≤ N]" },
  { "math with text and log", "$\\text{pre}[i] = O(n \\log n)$", "c[pre[i] = O(n log n)]" },
  { "math symbols",       "$a \\ne b \\cdot c \\ge 0$",  "c[a ≠ b · c ≥ 0]" },
  { "math braces",        "$x^{2} + y_{i}$",              "c[x^2 + y_i]" },
  { "prices are not math","costs $5 and $6 each",         "t[costs $5 and $6 each]" },
  { "lone dollar",        "a $ b",                        "t[a $ b]" },
  { "math of only braces (fuzzing)", "a $}}$ b",          "t[a  b]" },
  { "broken UTF-8 at the end (fuzzing)", "**\xf0",       "t[**\xef\xbf\xbd]" },
};

static void
test_spans (void)
{
  JrCases c = { "spans", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (span_cases); i++)
    {
      g_autofree char *got = spans_str (span_cases[i].text);
      jr_case (&c, span_cases[i].name, g_strcmp0 (got, span_cases[i].want) == 0, "Input \"%s\"  Expected \"%s\"  Got \"%s\"",
               span_cases[i].text, span_cases[i].want, got);
    }
  jr_cases_done (&c);
}

/* Hidden test: random Markdown-ish text never crashes, yields valid
 * UTF-8, and keeps every letter (markup characters may go). */
static void
test_hidden (void)
{
  static const char *bits[] = { "#", " ", "-", "*", "_", "`", "```", "\n", "[", "]", "(", ")", "1.", "→", "a", "é",
                                "<", ">", "\\", "~~~", "  ", "svg", "http://x" };
  JrCases c = { "markdown (hidden)", 0, 0 };
  for (int i = 0; i < 4000; i++)
    {
      GString *s = g_string_new (NULL);
      for (int j = g_test_rand_int_range (0, 30); j > 0; j--)
        g_string_append (s, bits[g_test_rand_int_range (0, G_N_ELEMENTS (bits))]);
      int letters_in = 0, letters_out = 0;
      for (const char *p = s->str; *p; p++)
        letters_in += *p == 'a';
      GPtrArray *b = jn_md_blocks (s->str);
      gboolean valid = TRUE;
      for (guint k = 0; k < b->len; k++)
        {
          JnMdBlock *blk = b->pdata[k];
          if (blk->text == NULL)
            continue;
          valid &= g_utf8_validate (blk->text, -1, NULL);
          for (const char *p = blk->lang; p != NULL && *p; p++)
            letters_out += *p == 'a'; /* shown as the code's label */
          if (blk->kind == JN_MD_CODE || blk->kind == JN_MD_SVG)
            {
              for (const char *p = blk->text; *p; p++)
                letters_out += *p == 'a';
              continue;
            }
          GPtrArray *sp = jn_md_spans (blk->text);
          for (guint m = 0; m < sp->len; m++)
            {
              JnMdSpan *span = sp->pdata[m];
              valid &= g_utf8_validate (span->text, -1, NULL);
              for (const char *p = span->text; *p; p++)
                letters_out += *p == 'a';
              for (const char *p = span->url; p != NULL && *p; p++)
                letters_out += *p == 'a'; /* a link's address: what it opens */
            }
          g_ptr_array_unref (sp);
        }
      g_ptr_array_unref (b);
      g_autofree char *shown = g_strescape (s->str, NULL);
      jr_case (&c, "random", valid && letters_out >= letters_in, "Input \"%s\"  letters %d -> %d", shown, letters_in,
               letters_out);
      g_string_free (s, TRUE);
    }
  jr_cases_done (&c);
}

/* Unclosed marks make each one look to the end of the line: the AI's
 * longest answer (about 130 KB) in one line of them still reads fast,
 * and a line over 16 KB keeps all its text, unformatted. */
static void
test_long_lines (void)
{
  static const char *marks[] = { "[x](", "[", "`a", "_a", "<https://", "**a" };
  for (gsize i = 0; i < G_N_ELEMENTS (marks); i++)
    for (gsize size = 16 * 1024 - 8; size <= 160 * 1024; size += 144 * 1024)
      {
        GString *s = g_string_new (NULL);
        while (s->len < size)
          g_string_append (s, marks[i]);
        gint64 t0 = g_get_monotonic_time ();
        g_autoptr (GPtrArray) blocks = jn_md_blocks (s->str);
        g_autoptr (GPtrArray) spans = jn_md_spans (((JnMdBlock *) blocks->pdata[0])->text);
        /* A fraction of a second here; 10 s leaves room for Valgrind and
         * still catches the square, which took seconds before the limit. */
        g_assert_cmpint (g_get_monotonic_time () - t0, <, 10 * G_USEC_PER_SEC);
        if (size > 16 * 1024)
          {
            g_assert_cmpuint (spans->len, ==, 1);
            g_assert_cmpstr (((JnMdSpan *) spans->pdata[0])->text, ==, g_strstrip (s->str));
          }
        g_string_free (s, TRUE);
      }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/markdown/blocks", test_blocks);
  g_test_add_func ("/markdown/spans", test_spans);
  g_test_add_func ("/markdown/hidden", test_hidden);
  g_test_add_func ("/markdown/long-lines", test_long_lines);
  return g_test_run ();
}
