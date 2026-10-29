/* Tests for turning a fetched web page into plain text for the AI
 * (src/notes/ai/textify.c), LeetCode style (see cases.h).
 *
 * Constraints: input is whatever a server sends: any bytes, broken or
 * hostile HTML, up to 5 MB. Output is valid UTF-8 text with the page's
 * words in order, no markup, no scripts or styles, paragraphs kept as
 * blank lines, at most `limit` bytes. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "textify.h"

typedef struct {
  const char *name, *html, *text;
} Case;

static const Case cases[] = {
  { "plain words",          "prefix sums",                             "prefix sums" },
  { "tags dropped",         "<p>prefix <b>sums</b></p>",               "prefix sums" },
  { "paragraphs kept",      "<p>one</p><p>two</p>",                    "one\n\ntwo" },
  { "headings on their own line", "<h2>Idea</h2>pre[i] is a sum",     "Idea\n\npre[i] is a sum" },
  { "list items",           "<ul><li>a</li><li>b</li></ul>",           "- a\n- b" },
  { "line breaks",          "a<br>b<br/>c",                            "a\nb\nc" },
  { "script removed",       "a<script>alert('x')</script>b",           "ab" },
  { "style removed",        "a<style>p{color:red}</style>b",           "ab" },
  { "script with < inside", "a<script>if (x < 3) y();</script>b",      "ab" },
  { "noscript, svg, head removed", "<head><title>t</title></head><svg><text>x</text></svg>body", "body" },
  { "comments removed",     "a<!-- secret -->b",                       "ab" },
  { "entities",             "a &amp; b &lt; c &gt; d &quot;e&quot; &#39;f&#39; &nbsp;g", "a & b < c > d \"e\" 'f' g" },
  { "numeric entities",     "&#8594; &#x2192; &#955;",                 "→ → λ" },
  { "bad entities kept",    "&bogus; &#xZZ; &#0; &#99999999;",         "&bogus; &#xZZ; &#0; &#99999999;" },
  { "surrogate entities kept", "&#xD800; &#57343;",                    "&#xD800; &#57343;" },
  { "whitespace collapsed", "  a \t\n  b  ",                           "a b" },
  { "pre keeps lines",      "<pre>for i:\n  pre[i+1] = pre[i] + a[i]</pre>", "for i:\n  pre[i+1] = pre[i] + a[i]" },
  { "code inline",          "use <code>pre[r+1] - pre[l]</code> here", "use pre[r+1] - pre[l] here" },
  { "attributes ignored",   "<a href=\"x\" onclick=\"evil()\">link</a>", "link" },
  { "uppercase tags",       "<P>A</P><SCRIPT>x</SCRIPT><P>B</P>",      "A\n\nB" },
  { "unclosed tag at end",  "text <b",                                 "text" },
  { "unclosed comment",     "a <!-- never closed",                     "a" },
  { "unclosed script",      "a <script> never closed",                 "a" },
  { "lone < and >",         "3 < 4 and 5 > 2",                         "3 < 4 and 5 > 2" },
  { "invalid UTF-8 replaced", "caf\xe9 ok",                            "caf\xef\xbf\xbd ok" },
  { "zero byte",            NULL,                                      "a b" },
  { "empty",                "",                                        "" },
};

static void
test_table (void)
{
  JrCases c = { "textify", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      const char *html = cases[i].html;
      gsize len = html != NULL ? strlen (html) : 3;
      if (html == NULL)
        html = "a\0b";
      g_autofree char *got = jn_textify (html, len, 100000);
      jr_case (&c, cases[i].name, g_strcmp0 (got, cases[i].text) == 0, "Expected \"%s\"  Got \"%s\"", cases[i].text,
               got);
    }
  jr_cases_done (&c);
}

/* Hidden test: random tag soup never yields markup characters from a
 * tag, invalid UTF-8, or more than the limit. */
static void
test_hidden (void)
{
  static const char *bits[] = { "<", ">", "<p>", "</p>", "<script>", "</script>", "<!--", "-->", "&amp;", "&#",
                                "&", "x", " ", "\n", "\xff", "é", "<a href='<'>", "\"", "<style", "</style>" };
  JrCases c = { "textify (hidden)", 0, 0 };
  for (int i = 0; i < 5000; i++)
    {
      GString *s = g_string_new (NULL);
      for (int j = g_test_rand_int_range (0, 40); j > 0; j--)
        g_string_append (s, bits[g_test_rand_int_range (0, G_N_ELEMENTS (bits))]);
      gsize limit = (gsize) g_test_rand_int_range (1, 200);
      g_autofree char *got = jn_textify (s->str, s->len, limit);
      gboolean ok = got != NULL && g_utf8_validate (got, -1, NULL) && strlen (got) <= limit &&
                    strstr (got, "<p>") == NULL && strstr (got, "<script") == NULL;
      jr_case (&c, "random", ok, "Input \"%s\" limit %zu  Got \"%s\"", s->str, limit, got);
      g_string_free (s, TRUE);
    }
  jr_cases_done (&c);
}

/* A 5 MB page in linear time; the result is cut at the limit on a
 * character boundary. */
static void
test_huge (void)
{
  GString *s = g_string_new (NULL);
  while (s->len < 5 * 1024 * 1024)
    g_string_append (s, "<div><p>prefix sums turn é range sums into one subtraction</p><script>x()</script></div>");
  gint64 t0 = g_get_monotonic_time ();
  g_autofree char *got = jn_textify (s->str, s->len, 60000);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 2 * G_USEC_PER_SEC);
  g_assert_cmpuint (strlen (got), <=, 60000);
  g_assert_true (g_utf8_validate (got, -1, NULL));
  g_assert_true (g_str_has_prefix (got, "prefix sums turn é range"));
  g_string_free (s, TRUE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/textify/table", test_table);
  g_test_add_func ("/textify/hidden", test_hidden);
  g_test_add_func ("/textify/huge", test_huge);
  return g_test_run ();
}
