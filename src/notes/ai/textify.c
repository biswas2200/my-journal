/* textify: see textify.h. */
#include "textify.h"

#include <string.h>

typedef struct {
  GString *out;
  int      breaks;  /* newlines owed before the next text (0, 1 or 2) */
  gboolean space;   /* a space owed */
  gboolean bullet;  /* "- " owed */
  int      pre;     /* inside <pre> */
} Out;

static void
owe_break (Out *o, int n)
{
  o->breaks = MAX (o->breaks, n);
  o->space = FALSE;
}

/* Pays what is owed, then writes one character. */
static void
put (Out *o, gunichar c)
{
  if (o->out->len > 0)
    {
      if (o->breaks > 0)
        {
          while (o->out->len > 0 && o->out->str[o->out->len - 1] == ' ')
            g_string_truncate (o->out, o->out->len - 1);
          for (int i = 0; i < o->breaks; i++)
            g_string_append_c (o->out, '\n');
        }
      else if (o->space && o->out->str[o->out->len - 1] != '\n')
        g_string_append_c (o->out, ' ');
    }
  if (o->bullet)
    g_string_append (o->out, "- ");
  o->breaks = 0;
  o->space = FALSE;
  o->bullet = FALSE;
  g_string_append_unichar (o->out, c);
}

static void
text_char (Out *o, gunichar c)
{
  if (c < 0x20 && c != '\n' && c != '\t')
    c = ' ';
  if (c == 0xA0)
    c = ' ';
  if (o->pre > 0)
    {
      if (c == '\n')
        {
          if (o->out->len > 0 && o->breaks == 0)
            g_string_append_c (o->out, '\n');
          return;
        }
      put (o, c);
      return;
    }
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
    o->space = TRUE;
  else
    put (o, c);
}

static const struct { const char *name; gunichar c; } ENTITIES[] = {
  { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
  { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "copy", 0xA9 }, { "rsquo", 0x2019 },
  { "lsquo", 0x2018 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "times", 0xD7 }, { "rarr", 0x2192 },
  { "larr", 0x2190 }, { "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 }, { "middot", 0xB7 },
};

/* An entity at p ("&...;"); its character and length, or 0. */
static gsize
entity (const char *p, const char *end, gunichar *c)
{
  const char *semi = memchr (p, ';', MIN ((gsize) (end - p), (gsize) 12));
  if (semi == NULL)
    return 0;
  gsize n = (gsize) (semi - p - 1);
  if (n >= 2 && p[1] == '#')
    {
      gboolean hex = p[2] == 'x' || p[2] == 'X';
      const char *digits = p + (hex ? 3 : 2);
      if (digits >= semi)
        return 0;
      guint64 v = 0;
      for (const char *d = digits; d < semi; d++)
        {
          int x = hex ? g_ascii_xdigit_value (*d) : g_ascii_digit_value (*d);
          if (x < 0)
            return 0;
          v = v * (hex ? 16 : 10) + (guint64) x;
          if (v > 0x10FFFF)
            return 0;
        }
      if (v == 0 || (v >= 0xD800 && v <= 0xDFFF))
        return 0;
      *c = (gunichar) v;
      return (gsize) (semi - p + 1);
    }
  for (gsize i = 0; i < G_N_ELEMENTS (ENTITIES); i++)
    if (strlen (ENTITIES[i].name) == n && strncmp (p + 1, ENTITIES[i].name, n) == 0)
      {
        *c = ENTITIES[i].c;
        return (gsize) (semi - p + 1);
      }
  return 0;
}

static gboolean
name_is (const char *name, gsize n, const char *const *list)
{
  for (const char *const *l = list; *l != NULL; l++)
    if (strlen (*l) == n && g_ascii_strncasecmp (name, *l, n) == 0)
      return TRUE;
  return FALSE;
}

static const char *const SKIP[] = { "script", "style", "head", "svg", "noscript", "template", "iframe",
                                    "object", "canvas", NULL };
static const char *const BLOCK[] = { "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "section", "article",
                                     "header", "footer", "table", "tr", "blockquote", "ul", "ol", "main", "nav",
                                     "aside", "figure", "dl", "dt", "dd", "hr", NULL };

/* Finds "</name" (any case) from p; returns where it ends, or end. */
static const char *
skip_element (const char *p, const char *end, const char *name, gsize n)
{
  for (const char *q = p; q + n + 2 <= end; q++)
    if (q[0] == '<' && q[1] == '/' && g_ascii_strncasecmp (q + 2, name, n) == 0)
      {
        const char *gt = memchr (q, '>', (gsize) (end - q));
        return gt != NULL ? gt + 1 : end;
      }
  return end;
}

char *
jn_textify (const char *html, gsize len, gsize limit)
{
  /* Zero bytes become spaces first, then invalid UTF-8 becomes U+FFFD:
   * from here on the text is one valid, zero-terminated string. */
  g_autofree char *flat = g_memdup2 (html, len + 1);
  flat[len] = '\0';
  for (gsize i = 0; i < len; i++)
    if (flat[i] == '\0')
      flat[i] = ' ';
  g_autofree char *valid = g_utf8_make_valid (flat, (gssize) len);
  gsize vlen = strlen (valid);
  Out o = { g_string_sized_new (MIN (vlen, limit) + 16), 0, FALSE, FALSE, 0 };
  const char *p = valid, *end = valid + vlen;
  while (p < end)
    {
      if (*p == '<' && p + 1 < end && (g_ascii_isalpha (p[1]) || p[1] == '/' || p[1] == '!' || p[1] == '?'))
        {
          if (p + 3 < end && strncmp (p, "<!--", 4) == 0)
            {
              const char *close = g_strstr_len (p + 4, (gssize) (end - p - 4), "-->");
              p = close != NULL ? close + 3 : end;
              continue;
            }
          const char *gt = memchr (p, '>', (gsize) (end - p));
          if (gt == NULL)
            break; /* an unclosed tag at the end: drop it */
          gboolean closing = p[1] == '/';
          const char *name = p + (closing ? 2 : 1);
          gsize n = 0;
          while (name + n < gt && (g_ascii_isalnum (name[n]) || name[n] == '-'))
            n++;
          p = gt + 1;
          if (!closing && name_is (name, n, SKIP))
            {
              g_autofree char *nm = g_strndup (name, n);
              p = skip_element (p, end, nm, n);
              continue;
            }
          if (n == 2 && g_ascii_strncasecmp (name, "br", 2) == 0)
            owe_break (&o, 1);
          else if (n == 2 && g_ascii_strncasecmp (name, "li", 2) == 0)
            {
              owe_break (&o, 1);
              o.bullet = !closing;
            }
          else if (n == 3 && g_ascii_strncasecmp (name, "pre", 3) == 0)
            {
              owe_break (&o, 2);
              o.pre += closing ? (o.pre > 0 ? -1 : 0) : 1;
            }
          else if (n == 2 && (g_ascii_strncasecmp (name, "td", 2) == 0 || g_ascii_strncasecmp (name, "th", 2) == 0))
            o.space = TRUE;
          else if (name_is (name, n, BLOCK))
            owe_break (&o, 2);
          continue;
        }
      gunichar c = 0;
      gsize m = *p == '&' ? entity (p, end, &c) : 0;
      if (m > 0)
        {
          text_char (&o, c);
          p += m;
          continue;
        }
      c = g_utf8_get_char (p);
      text_char (&o, c);
      p = g_utf8_next_char (p);
    }
  /* Trim the end, then fit the limit on a character boundary. */
  while (o.out->len > 0 && g_ascii_isspace (o.out->str[o.out->len - 1]))
    g_string_truncate (o.out, o.out->len - 1);
  if (o.out->len > limit)
    {
      /* Back up to the start of the character that would cross it. */
      gsize cut = limit;
      while (cut > 0 && ((guchar) o.out->str[cut] & 0xC0) == 0x80)
        cut--;
      g_string_truncate (o.out, cut);
    }
  return g_string_free (o.out, FALSE);
}
