/* Fuzz target: a fetched web page as plain text for the AI. The first
 * two bytes pick the length limit, the rest is the page (any bytes).
 * The text is valid UTF-8 with no control characters but newline and
 * tab; uncut, it does not end in white space; a limit gives the start of
 * the same text, cut on the last character boundary that fits. */
#define FUZZ_NOTES
#include <string.h>
#include "fuzz.h"
#include "textify.h"

static void
check_text (const char *t, gsize limit)
{
  gsize n = strlen (t);
  FUZZ_CHECK (n <= limit);
  FUZZ_CHECK (g_utf8_validate (t, -1, NULL));
  for (const char *p = t; *p != '\0'; p++)
    FUZZ_CHECK ((guchar) *p >= 0x20 || *p == '\n' || *p == '\t');
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  if (size < 2)
    return 0;
  gsize limit = (gsize) data[0] << 8 | data[1];
  const char *html = (const char *) data + 2;
  gsize len = size - 2;
  g_autofree char *full = jn_textify (html, len, G_MAXSIZE);
  g_autofree char *cut = jn_textify (html, len, limit);
  gsize n = strlen (cut), whole = strlen (full);
  check_text (full, G_MAXSIZE);
  FUZZ_CHECK (whole == 0 || !g_ascii_isspace (full[whole - 1]));
  check_text (cut, limit);
  if (whole <= limit)
    FUZZ_CHECK (strcmp (full, cut) == 0);
  else
    {
      FUZZ_CHECK (strncmp (full, cut, n) == 0);
      FUZZ_CHECK (n + 4 > limit && ((guchar) full[n] & 0xC0) != 0x80); /* a character is 4 bytes at most */
    }
  return 0;
}
