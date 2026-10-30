/* Tests for making an AI-drawn SVG safe to show (src/notes/core/svgsafe.c),
 * LeetCode style (see cases.h).
 *
 * The SVG comes from a model that read web pages, so it is untrusted.
 * Only drawing survives: an allow-list of shapes, text and their
 * geometry and paint attributes. Scripts, event handlers, links, embedded
 * documents, external images, style sheets and url() references other
 * than "#id" are removed; DOCTYPEs, entities and oversized input are
 * refused outright. What comes out is re-written XML that librsvg draws. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "svgsafe.h"

#define OK_SVG \
  "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"500\" height=\"110\" viewBox=\"0 0 500 110\">" \
  "<rect x=\"10\" y=\"15\" width=\"140\" height=\"30\" fill=\"#e2e8f0\" stroke=\"#475569\" stroke-width=\"1.5\"/>" \
  "<text x=\"80\" y=\"35\" font-family=\"sans-serif\" font-size=\"12\" text-anchor=\"middle\">prefix[L - 1]</text>" \
  "<line x1=\"10\" y1=\"60\" x2=\"370\" y2=\"60\" stroke=\"#0f172a\" marker-end=\"url(#arrow)\"/>" \
  "<defs><marker id=\"arrow\" markerWidth=\"6\" markerHeight=\"6\" refX=\"3\" refY=\"3\" orient=\"auto\">" \
  "<path d=\"M0,0 L6,3 L0,6 z\" fill=\"#0f172a\"/></marker></defs></svg>"

typedef struct {
  const char *name, *svg;
  gboolean    accepted;
  const char *must_have, *must_not; /* in the cleaned output */
} SvgCase;

static const SvgCase cases[] = {
  { "a diagram",            OK_SVG, TRUE, "prefix[L - 1]", NULL },
  { "markers by #id",       OK_SVG, TRUE, "url(#arrow)", NULL },
  { "script removed",       "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>fetch('x')</script><rect width=\"1\" height=\"1\"/></svg>",
                            TRUE, "<rect", "script" },
  { "event handler removed","<svg xmlns=\"http://www.w3.org/2000/svg\" onload=\"alert(1)\"><rect onclick=\"x()\" width=\"1\" height=\"1\"/></svg>",
                            TRUE, "<rect", "on" },
  { "link removed, text kept", "<svg xmlns=\"http://www.w3.org/2000/svg\"><a href=\"https://evil.example\"><text>hi</text></a></svg>",
                            TRUE, "hi", "evil" },
  { "external image removed", "<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"https://tracker.example/p.png\" width=\"9\" height=\"9\"/></svg>",
                            TRUE, "<svg", "tracker" },
  { "local file image removed", "<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"file:///home/me/.local/share/journal/journal.db\"/></svg>",
                            TRUE, "<svg", "journal" },
  { "foreignObject removed","<svg xmlns=\"http://www.w3.org/2000/svg\"><foreignObject><div xmlns=\"http://www.w3.org/1999/xhtml\">x</div></foreignObject></svg>",
                            TRUE, "<svg", "foreign" },
  { "use of another file removed", "<svg xmlns=\"http://www.w3.org/2000/svg\"><use href=\"other.svg#a\"/></svg>",
                            TRUE, "<svg", "other.svg" },
  { "style element removed","<svg xmlns=\"http://www.w3.org/2000/svg\"><style>@import url(https://evil.example/x.css);</style></svg>",
                            TRUE, "<svg", "import" },
  { "style attribute removed", "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect style=\"fill:url(https://evil.example/a)\" width=\"1\" height=\"1\"/></svg>",
                            TRUE, "<rect", "evil" },
  { "remote paint removed", "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect fill=\"url(https://evil.example/#g)\" width=\"1\" height=\"1\"/></svg>",
                            TRUE, "<rect", "evil" },
  { "a second url() checked too (fuzzing)", "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect fill=\"url(#g) url(https://evil.example/#x)\" width=\"1\" height=\"1\"/></svg>",
                            TRUE, "<rect", "evil" },
  { "a second root refused (fuzzing)", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\"></svg><svg width=\"99999\"></svg>",
                            FALSE, NULL, NULL },
  { "an attribute given twice kept once (fuzzing)", "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" width=\"2\" height=\"1\"/></svg>",
                            TRUE, "<rect width=\"2\" height=\"1\">", "width=\"1\"" },
  { "href and style on a kept shape removed", "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect href=\"https://evil.example\" style=\"fill:red\" width=\"1\" height=\"1\"/></svg>",
                            TRUE, "<rect width=\"1\" height=\"1\">", "style" },
  { "xlink:href removed",   "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"><use xlink:href=\"#a\"/></svg>",
                            TRUE, "<svg", "href" },
  { "text escaped again",   "<svg xmlns=\"http://www.w3.org/2000/svg\"><text>a &lt; b &amp; c</text></svg>",
                            TRUE, "a &lt; b &amp; c", NULL },
  { "comments dropped",     "<svg xmlns=\"http://www.w3.org/2000/svg\"><!-- secret --><rect width=\"1\" height=\"1\"/></svg>",
                            TRUE, "<rect", "secret" },
  { "DOCTYPE refused",      "<!DOCTYPE svg [<!ENTITY a \"aaaaaaaaaa\">]><svg xmlns=\"http://www.w3.org/2000/svg\"><text>&a;</text></svg>",
                            FALSE, NULL, NULL },
  { "unknown entity refused", "<svg xmlns=\"http://www.w3.org/2000/svg\"><text>&evil;</text></svg>", FALSE, NULL, NULL },
  { "not SVG",              "<html><body>x</body></html>", FALSE, NULL, NULL },
  { "broken XML",           "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect", FALSE, NULL, NULL },
  { "empty",                "", FALSE, NULL, NULL },
  { "huge size refused",    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100000\" height=\"100000\"></svg>", FALSE, NULL, NULL },
  { "processing instruction", "<?xml-stylesheet href=\"https://evil.example/x.css\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>",
                            TRUE, "<svg", "evil" },
};

static void
test_table (void)
{
  JrCases c = { "svgsafe", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      const SvgCase *k = &cases[i];
      g_autofree char *got = jn_svg_clean (k->svg, -1);
      gboolean ok = k->accepted ? got != NULL && (k->must_have == NULL || strstr (got, k->must_have) != NULL) &&
                                    (k->must_not == NULL || strstr (got, k->must_not) == NULL)
                                : got == NULL;
      jr_case (&c, k->name, ok, "Expected %s  Got %s", k->accepted ? "cleaned" : "refused", got ? got : "refused");
    }
  /* The cleaned SVG is itself clean (cleaning twice changes nothing). */
  g_autofree char *once = jn_svg_clean (OK_SVG, -1);
  g_autofree char *twice = jn_svg_clean (once, -1);
  jr_case (&c, "stable", g_strcmp0 (once, twice) == 0, "Expected the same  Got %s", twice);
  jr_cases_done (&c);
}

/* Limits: a billion-laughs style or deep nesting, many elements, a huge
 * input, are refused quickly. */
static void
test_limits (void)
{
  GString *deep = g_string_new ("<svg xmlns=\"http://www.w3.org/2000/svg\">");
  for (int i = 0; i < 500; i++)
    g_string_append (deep, "<g>");
  for (int i = 0; i < 500; i++)
    g_string_append (deep, "</g>");
  g_string_append (deep, "</svg>");
  g_autofree char *d = jn_svg_clean (deep->str, -1);
  g_assert_null (d);
  g_string_free (deep, TRUE);

  GString *many = g_string_new ("<svg xmlns=\"http://www.w3.org/2000/svg\">");
  for (int i = 0; i < 5000; i++)
    g_string_append (many, "<rect width=\"1\" height=\"1\"/>");
  g_string_append (many, "</svg>");
  g_autofree char *m = jn_svg_clean (many->str, -1);
  g_assert_null (m);
  g_string_free (many, TRUE);

  g_autofree char *big = g_strnfill (300 * 1024, ' ');
  memcpy (big, "<svg>", 5);
  g_assert_null (jn_svg_clean (big, -1));
}

/* Hidden: random fragments never crash, and whatever comes out has no
 * script, no handler, no href and no remote url(). */
static void
test_hidden (void)
{
  static const char *bits[] = { "<svg xmlns=\"http://www.w3.org/2000/svg\">", "</svg>", "<rect width=\"1\"/>", "<script>",
                                "</script>", " onload=\"x\"", "<a href=\"https://e\">", "</a>", "<text>", "</text>", "&amp;",
                                "<g>", "</g>", "<style>", "</style>", " fill=\"url(https://e)\"", "x", "<", ">", "\"",
                                "<image href=\"file:///etc\"/>", "<!--", "-->" };
  JrCases c = { "svgsafe (hidden)", 0, 0 };
  for (int i = 0; i < 5000; i++)
    {
      GString *s = g_string_new (NULL);
      for (int j = g_test_rand_int_range (0, 14); j > 0; j--)
        g_string_append (s, bits[g_test_rand_int_range (0, G_N_ELEMENTS (bits))]);
      g_autofree char *got = jn_svg_clean (s->str, -1);
      /* Text may say anything once escaped ("<" and "\"" become entities),
       * so look only for markup: tags and attribute values. */
      gboolean ok = got == NULL || (strstr (got, "<script") == NULL && strstr (got, "onload=\"") == NULL &&
                                    strstr (got, "href=\"") == NULL && strstr (got, "=\"url(h") == NULL &&
                                    strstr (got, "<style") == NULL && strstr (got, "style=\"") == NULL &&
                                    g_str_has_prefix (got, "<svg"));
      g_autofree char *shown = g_strescape (s->str, NULL);
      jr_case (&c, "random", ok, "Input \"%s\"  Got \"%s\"", shown, got);
      g_string_free (s, TRUE);
    }
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/svgsafe/table", test_table);
  g_test_add_func ("/svgsafe/limits", test_limits);
  g_test_add_func ("/svgsafe/hidden", test_hidden);
  return g_test_run ();
}
