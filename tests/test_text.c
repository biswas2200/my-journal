/* Tests for word counting (src/core/text.c). */
#include <glib.h>
#include "text.h"

static void
test_count_words (void)
{
  g_assert_cmpuint (jr_count_words (NULL), ==, 0);
  g_assert_cmpuint (jr_count_words (""), ==, 0);
  g_assert_cmpuint (jr_count_words ("   \n\t "), ==, 0);
  g_assert_cmpuint (jr_count_words ("one"), ==, 1);
  g_assert_cmpuint (jr_count_words ("  two  words  "), ==, 2);
  g_assert_cmpuint (jr_count_words ("Slow start today.\nTwo chapters done"), ==, 6);
  g_assert_cmpuint (jr_count_words ("what-ifs again"), ==, 2);
  /* Unicode letters and non-breaking spaces. */
  g_assert_cmpuint (jr_count_words ("naïve café"), ==, 2);
  g_assert_cmpuint (jr_count_words ("a\xc2\xa0" "b"), ==, 2);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/text/count-words", test_count_words);
  return g_test_run ();
}
