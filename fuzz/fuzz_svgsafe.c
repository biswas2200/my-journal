/* Fuzz target: the cleaner for the AI's SVG diagrams (the AI read web
 * pages, so a diagram may carry anything). What comes out is checked by
 * reading it again, with rules written apart from the cleaner's own
 * allow-lists: the root is <svg> of size 4000 or less; no script,
 * style, link, image, <use> or embedded document; one root and no
 * attribute twice (XML readers refuse both); no event handler,
 * href or style attribute; url() only as url(#id); cleaning it again
 * changes nothing. */
#define FUZZ_NOTES
#include <string.h>
#include "fuzz.h"
#include "svgsafe.h"

static const char *const BANNED_ELEMENTS[] = {
  "script", "style", "a", "image", "use", "foreignObject", "iframe", "object", "embed", "animate", "set",
  "animateTransform", "animateMotion", "feImage", "pattern", "filter", NULL,
};

typedef struct {
  int depth, roots;
} Check;

static void
start (GMarkupParseContext *ctx, const char *name, const char **anames, const char **avalues, gpointer data,
       GError **error)
{
  (void) ctx; (void) error;
  Check *c = data;
  if (c->depth++ == 0)
    FUZZ_CHECK (strcmp (name, "svg") == 0 && ++c->roots == 1);
  for (const char *const *b = BANNED_ELEMENTS; *b != NULL; b++)
    FUZZ_CHECK (g_ascii_strcasecmp (name, *b) != 0);
  FUZZ_CHECK (strchr (name, ':') == NULL);
  for (int i = 0; anames[i] != NULL; i++)
    {
      const char *a = anames[i], *v = avalues[i];
      FUZZ_CHECK (!g_strv_contains (anames + i + 1, a)); /* XML readers refuse a repeat */
      FUZZ_CHECK (g_ascii_strncasecmp (a, "on", 2) != 0);
      FUZZ_CHECK (g_ascii_strcasecmp (a, "style") != 0 && g_ascii_strcasecmp (a, "href") != 0);
      FUZZ_CHECK (strchr (a, ':') == NULL); /* no xlink:href, no other namespace */
      FUZZ_CHECK (strstr (v, "javascript:") == NULL);
      for (const char *u = strstr (v, "url("); u != NULL; u = strstr (u + 4, "url("))
        FUZZ_CHECK (u[4] == '#');
      if (c->depth == 1 && (strcmp (a, "width") == 0 || strcmp (a, "height") == 0))
        FUZZ_CHECK (!(g_ascii_strtod (v, NULL) > 4000.0));
    }
}

static void
end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  (void) ctx; (void) name; (void) error;
  ((Check *) data)->depth--;
}

static void
passthrough (GMarkupParseContext *ctx, const char *t, gsize len, gpointer data, GError **error)
{
  (void) ctx; (void) t; (void) len; (void) data; (void) error;
  FUZZ_CHECK (!"the cleaned SVG has no comments, DOCTYPE or processing instructions");
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  g_autofree char *svg = jn_svg_clean ((const char *) data, (gssize) size);
  if (svg == NULL)
    return 0;
  FUZZ_CHECK (g_str_has_prefix (svg, "<svg"));
  FUZZ_CHECK (g_utf8_validate (svg, -1, NULL));
  static const GMarkupParser parser = { start, end, NULL, passthrough, NULL };
  Check c = { 0 };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &c, NULL);
  GError *error = NULL;
  gboolean ok = g_markup_parse_context_parse (ctx, svg, -1, &error) && g_markup_parse_context_end_parse (ctx, &error);
  if (!ok)
    fprintf (stderr, "cleaned SVG does not read back: %s\n", error->message);
  FUZZ_CHECK (ok);
  g_markup_parse_context_free (ctx);
  g_autofree char *again = jn_svg_clean (svg, -1);
  FUZZ_CHECK (again != NULL && strcmp (again, svg) == 0);
  return 0;
}
