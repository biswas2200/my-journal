/* Fuzz target: word counting over any bytes, valid UTF-8 or not. The
 * count never exceeds the length, a space at the end changes nothing,
 * and a word and a space in front add exactly one. */
#include <string.h>
#include "fuzz.h"
#include "text.h"

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  g_autofree char *text = fuzz_string (data, size);
  guint n = jr_count_words (text);
  FUZZ_CHECK (n <= strlen (text));
  g_autofree char *spaced = g_strconcat (text, " ", NULL);
  FUZZ_CHECK (jr_count_words (spaced) == n);
  g_autofree char *prefixed = g_strconcat ("x ", text, NULL);
  FUZZ_CHECK (jr_count_words (prefixed) == n + 1);
  return 0;
}
