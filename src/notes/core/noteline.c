/* noteline: see noteline.h. */
#include "noteline.h"

#include <string.h>

#define ARROW "\xe2\x86\x92" /* → */

/* A bullet marker ("- ", "* ", "+ ") at s: its length, else 0. */
static gsize
bullet_marker (const char *s)
{
  return (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ' ? 2 : 0;
}

JnKind
jn_line_parse (const char *line, const char **text)
{
  if (line[0] == '\\')
    {
      *text = line + 1;
      return JN_TEXT;
    }
  if (g_str_has_prefix (line, "## "))
    {
      *text = line + 3;
      return JN_SUBHEADING;
    }
  if (g_str_has_prefix (line, "# "))
    {
      *text = line + 2;
      return JN_HEADING;
    }
  if (g_str_has_prefix (line, ARROW " "))
    {
      *text = line + strlen (ARROW " ");
      return JN_ARROW;
    }
  if (g_str_has_prefix (line, "-> "))
    {
      *text = line + 3;
      return JN_ARROW;
    }
  /* Bullets: up to one space of indent is a bullet, more (or a tab) a
   * sub-bullet. A tab counts as four spaces. */
  gsize indent = 0, i = 0;
  for (; line[i] == ' ' || line[i] == '\t'; i++)
    indent += line[i] == '\t' ? 4 : 1;
  gsize m = bullet_marker (line + i);
  if (m > 0)
    {
      *text = line + i + m;
      return indent >= 2 ? JN_SUB_BULLET : JN_BULLET;
    }
  *text = line;
  return JN_TEXT;
}

char *
jn_line_format (JnKind kind, const char *text)
{
  g_autofree char *flat = g_strdup (text);
  for (char *p = flat; *p != '\0'; p++)
    if (*p == '\n' || *p == '\r')
      *p = ' ';
  switch (kind)
    {
    case JN_HEADING:    return g_strconcat ("# ", flat, NULL);
    case JN_SUBHEADING: return g_strconcat ("## ", flat, NULL);
    case JN_BULLET:     return g_strconcat ("- ", flat, NULL);
    case JN_SUB_BULLET: return g_strconcat ("  - ", flat, NULL);
    case JN_ARROW:      return g_strconcat (ARROW " ", flat, NULL);
    case JN_TEXT:
    default:
      {
        const char *rest;
        /* Escape text that would read back as something else. */
        if (flat[0] == '\\' || jn_line_parse (flat, &rest) != JN_TEXT)
          return g_strconcat ("\\", flat, NULL);
        return g_steal_pointer (&flat);
      }
    }
}

JnKind
jn_line_shortcut (const char *typed, gsize *prefix_len)
{
  static const struct { const char *marker; JnKind kind; } shortcuts[] = {
    { "- ", JN_BULLET }, { "* ", JN_BULLET }, { "+ ", JN_BULLET },
    { "# ", JN_HEADING }, { "## ", JN_SUBHEADING },
    { "> ", JN_ARROW }, { "-> ", JN_ARROW }, { ARROW " ", JN_ARROW },
    { "  - ", JN_SUB_BULLET }, { "  * ", JN_SUB_BULLET }, { "\t- ", JN_SUB_BULLET },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (shortcuts); i++)
    if (strcmp (typed, shortcuts[i].marker) == 0)
      {
        *prefix_len = strlen (shortcuts[i].marker);
        return shortcuts[i].kind;
      }
  *prefix_len = 0;
  return JN_TEXT;
}

JnKind
jn_kind_indent (JnKind kind)
{
  return kind == JN_BULLET ? JN_SUB_BULLET : kind;
}

JnKind
jn_kind_outdent (JnKind kind)
{
  switch (kind)
    {
    case JN_SUB_BULLET: return JN_BULLET;
    case JN_BULLET:
    case JN_ARROW:      return JN_TEXT;
    default:            return kind;
    }
}

const char *
jn_kind_glyph (JnKind kind)
{
  switch (kind)
    {
    case JN_BULLET:     return "\xe2\x97\x8f ";  /* ● */
    case JN_SUB_BULLET: return "\xe2\x97\xa6 ";  /* ◦ */
    case JN_ARROW:      return ARROW " ";
    default:            return "";
    }
}
