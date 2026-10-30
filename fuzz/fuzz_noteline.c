/* Fuzz target: how a note's lines are stored. The input is split on
 * newlines; the first byte of each line also picks a kind to store it
 * as. Every line, whatever its kind, comes back as that kind and text
 * (newlines in the text become spaces); any stored line reads as a
 * kind whose text lies inside it, and writing that out reads the same;
 * the typing shortcuts and Tab keys give real kinds. */
#define FUZZ_NOTES
#include <string.h>
#include "fuzz.h"
#include "noteline.h"

static gboolean
real_kind (JnKind k)
{
  return k >= JN_TEXT && k <= JN_ARROW;
}

static void
check_line (const char *line, JnKind pick)
{
  /* Stored as `pick`, it reads back as written. */
  g_autofree char *stored = jn_line_format (pick, line);
  FUZZ_CHECK (strchr (stored, '\n') == NULL && strchr (stored, '\r') == NULL);
  const char *text;
  FUZZ_CHECK (jn_line_parse (stored, &text) == pick);
  FUZZ_CHECK (strcmp (text, line) == 0);

  /* As found in a file, it reads as something, and stays that. */
  JnKind k = jn_line_parse (line, &text);
  FUZZ_CHECK (real_kind (k));
  FUZZ_CHECK (text >= line && text <= line + strlen (line));
  g_autofree char *again = jn_line_format (k, text);
  const char *text2;
  FUZZ_CHECK (jn_line_parse (again, &text2) == k && strcmp (text2, text) == 0);

  gsize prefix = 99;
  JnKind s = jn_line_shortcut (line, &prefix);
  FUZZ_CHECK (real_kind (s) && prefix <= strlen (line) && (s != JN_TEXT) == (prefix > 0));
  FUZZ_CHECK (real_kind (jn_kind_indent (pick)) && real_kind (jn_kind_outdent (pick)));
  FUZZ_CHECK (jn_kind_glyph (pick) != NULL);
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  g_autofree char *text = fuzz_string (data, size);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (char **l = lines; *l != NULL; l++)
    {
      for (char *p = *l; *p != '\0'; p++)
        if (*p == '\r')
          *p = ' ';
      check_line (*l, (JnKind) ((guchar) (*l)[0] % 6));
    }
  /* A newline inside the text becomes a space, nothing else changes. */
  g_autofree char *stored = jn_line_format (JN_BULLET, text);
  const char *back;
  FUZZ_CHECK (jn_line_parse (stored, &back) == JN_BULLET && strlen (back) == strlen (text));
  return 0;
}
