/* text: see text.h. */
#include "text.h"

guint
jr_count_words (const char *text)
{
  if (text == NULL)
    return 0;
  guint words = 0;
  gboolean in_word = FALSE;
  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      /* Fast path for ASCII; decode only multi-byte characters. */
      guchar c = (guchar) *p;
      gboolean space = c < 0x80 ? g_ascii_isspace (c) : g_unichar_isspace (g_utf8_get_char (p));
      if (!space && !in_word)
        words++;
      in_word = !space;
    }
  return words;
}
