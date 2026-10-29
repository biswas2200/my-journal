/* svgsafe: see svgsafe.h. */
#include "svgsafe.h"

#include <string.h>

#define SVG_NS "http://www.w3.org/2000/svg"
#define MAX_ELEMENTS 2000
#define MAX_DEPTH 64
#define MAX_SIZE 4000.0

static const char *const ELEMENTS[] = {
  "svg", "g", "defs", "marker", "rect", "circle", "ellipse", "line", "polyline", "polygon", "path", "text", "tspan",
  "title", "desc", "linearGradient", "radialGradient", "stop", "clipPath", NULL,
};

/* Dropped, but what is inside stays (a link's text). */
static const char *const UNWRAP[] = { "a", NULL };

static const char *const ATTRIBUTES[] = {
  "xmlns", "version", "width", "height", "viewBox", "preserveAspectRatio", "x", "y", "x1", "y1", "x2", "y2", "cx", "cy",
  "r", "rx", "ry", "dx", "dy", "d", "points", "transform", "fill", "fill-opacity", "fill-rule", "stroke",
  "stroke-width", "stroke-opacity", "stroke-dasharray", "stroke-dashoffset", "stroke-linecap", "stroke-linejoin",
  "stroke-miterlimit", "opacity", "font-family", "font-size", "font-weight", "font-style", "text-anchor",
  "dominant-baseline", "alignment-baseline", "letter-spacing", "id", "marker-start", "marker-mid", "marker-end",
  "markerWidth", "markerHeight", "refX", "refY", "orient", "markerUnits", "offset", "stop-color", "stop-opacity",
  "gradientUnits", "gradientTransform", "clip-path", "clipPathUnits", "rotate", "textLength", "lengthAdjust",
  "visibility", "display", NULL,
};

typedef struct {
  GString *out;
  int      depth, elements;
  int      skip;      /* inside a dropped element: its depth, else 0 */
  int      unwrapped; /* bitmask-free: depths of unwrapped elements */
  GArray  *kept;      /* per depth: was the element written */
  gboolean root_seen;
} Clean;

static gboolean
value_safe (const char *name, const char *value)
{
  if (strlen (value) > 10000)
    return FALSE;
  if (g_strrstr (value, "javascript:") != NULL)
    return FALSE;
  const char *u = strstr (value, "url(");
  if (u != NULL)
    {
      /* Only a reference inside this drawing: url(#id). */
      if (u[4] != '#')
        return FALSE;
      for (const char *p = u + 5; *p != ')'; p++)
        if (*p == '\0' || !(g_ascii_isalnum (*p) || *p == '-' || *p == '_'))
          return FALSE;
    }
  if (strcmp (name, "xmlns") == 0)
    return strcmp (value, SVG_NS) == 0;
  return TRUE;
}

static double
size_of (const char *v)
{
  return g_ascii_strtod (v, NULL);
}

static void
start (GMarkupParseContext *ctx, const char *name, const char **anames, const char **avalues, gpointer data,
       GError **error)
{
  (void) ctx;
  Clean *c = data;
  c->depth++;
  gboolean written = FALSE;
  if (++c->elements > MAX_ELEMENTS || c->depth > MAX_DEPTH)
    {
      g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "too large");
      return;
    }
  if (!c->root_seen)
    {
      c->root_seen = TRUE;
      if (strcmp (name, "svg") != 0)
        {
          g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "not svg");
          return;
        }
      for (int i = 0; anames[i] != NULL; i++)
        if ((strcmp (anames[i], "width") == 0 || strcmp (anames[i], "height") == 0) &&
            size_of (avalues[i]) > MAX_SIZE)
          {
            g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "too big");
            return;
          }
    }
  if (c->skip == 0 && g_strv_contains (ELEMENTS, name))
    {
      g_string_append_printf (c->out, "<%s", name);
      for (int i = 0; anames[i] != NULL; i++)
        if (g_strv_contains (ATTRIBUTES, anames[i]) && value_safe (anames[i], avalues[i]))
          {
            g_autofree char *v = g_markup_escape_text (avalues[i], -1);
            g_string_append_printf (c->out, " %s=\"%s\"", anames[i], v);
          }
      g_string_append_c (c->out, '>');
      written = TRUE;
    }
  else if (c->skip == 0 && !g_strv_contains (UNWRAP, name))
    c->skip = c->depth; /* drop it and everything inside */
  g_array_append_val (c->kept, written);
}

static void
end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  (void) ctx; (void) error;
  Clean *c = data;
  gboolean written = g_array_index (c->kept, gboolean, c->kept->len - 1);
  g_array_set_size (c->kept, c->kept->len - 1);
  if (written)
    g_string_append_printf (c->out, "</%s>", name);
  if (c->skip == c->depth)
    c->skip = 0;
  c->depth--;
}

static void
text (GMarkupParseContext *ctx, const char *t, gsize len, gpointer data, GError **error)
{
  (void) ctx; (void) error;
  Clean *c = data;
  if (c->skip != 0 || c->depth == 0)
    return;
  g_autofree char *e = g_markup_escape_text (t, (gssize) len);
  g_string_append (c->out, e);
}

static void
passthrough (GMarkupParseContext *ctx, const char *t, gsize len, gpointer data, GError **error)
{
  (void) ctx; (void) data;
  /* Comments and processing instructions are dropped; a DOCTYPE (where
   * entities would be declared) is refused. */
  if (len >= 9 && g_ascii_strncasecmp (t, "<!DOCTYPE", 9) == 0)
    g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "doctype");
}

char *
jn_svg_clean (const char *svg, gssize len)
{
  gsize n = len < 0 ? strlen (svg) : (gsize) len;
  if (n == 0 || n > JN_SVG_MAX_BYTES)
    return NULL;
  static const GMarkupParser parser = { start, end, text, passthrough, NULL };
  Clean c = { g_string_new (NULL), 0, 0, 0, 0, g_array_new (FALSE, FALSE, sizeof (gboolean)), FALSE };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &c, NULL);
  gboolean ok = g_markup_parse_context_parse (ctx, svg, (gssize) n, NULL) &&
                g_markup_parse_context_end_parse (ctx, NULL) && c.root_seen;
  g_markup_parse_context_free (ctx);
  g_array_unref (c.kept);
  if (!ok)
    {
      g_string_free (c.out, TRUE);
      return NULL;
    }
  return g_string_free (c.out, FALSE);
}
