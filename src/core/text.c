/* text: see text.h. */
#include "text.h"

guint
jr_count_words (const char *text)
{
  if (text == NULL)
    return 0;
  guint words = 0;
  gboolean in_word = FALSE;
  for (const char *p = text; *p != '\0';)
    {
      /* Fast path for ASCII; decode only multi-byte characters. A broken
       * sequence counts as one non-space byte, so a lead byte just before
       * the end can never step past it. */
      guchar c = (guchar) *p;
      gboolean space;
      if (c < 0x80)
        {
          space = g_ascii_isspace (c);
          p++;
        }
      else
        {
          gunichar u = g_utf8_get_char_validated (p, -1);
          if (u == (gunichar) -1 || u == (gunichar) -2)
            {
              space = FALSE;
              p++;
            }
          else
            {
              space = g_unichar_isspace (u);
              p = g_utf8_next_char (p);
            }
        }
      if (!space && !in_word)
        words++;
      in_word = !space;
    }
  return words;
}
