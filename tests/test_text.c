/* Tests for word counting (src/core/text.c), LeetCode style (see
 * cases.h): a table of edge cases, hidden tests against a reference that
 * knows which pieces are gaps, broken bytes, and huge inputs. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "text.h"

#ifdef __SANITIZE_ADDRESS__
#define INSTRUMENTED 1
#else
#define INSTRUMENTED 0
#endif

/* Constraints: UTF-8 text from the editor, or NULL. A word is a run of
 * characters that are not white space (Unicode white space included).
 * Punctuation on its own counts; scripts written without spaces count
 * one word per run. */
static void
test_count_words (void)
{
  static const struct {
    const char *name;
    const char *input;
    guint expected;
  } cases[] = {
    { "NULL", NULL, 0 },
    { "empty", "", 0 },
    { "only white space", "   \n\t \r\n ", 0 },
    { "one letter", "a", 1 },
    { "one word", "one", 1 },
    { "spaces around and between", "  two  words  ", 2 },
    { "lines", "Slow start today.\nTwo chapters done", 6 },
    { "Windows line ends", "a\r\nb\r\n", 2 },
    { "tabs", "a\tb\tc", 3 },
    { "hyphenated", "what-ifs again", 2 },
    { "apostrophe", "don't stop", 2 },
    { "punctuation on its own", "wait \xe2\x80\x94 what ?", 4 },
    { "accented letters", "na\xc3\xafve caf\xc3\xa9", 2 },
    { "no-break space", "a\xc2\xa0" "b", 2 },
    { "em space", "a\xe2\x80\x83" "b", 2 },
    { "ideographic space", "\xe6\x97\xa5\xe6\x9c\xac\xe3\x80\x80\xe8\xaa\x9e", 2 },
    { "line separator", "a\xe2\x80\xa8" "b", 2 },
    { "zero-width space is not a gap", "a\xe2\x80\x8b" "b", 1 },
    { "Japanese without spaces", "\xe4\xbb\x8a\xe6\x97\xa5\xe3\x81\xaf\xe8\x89\xaf\xe3\x81\x84", 1 },
    { "emoji between words", "I \xe2\x9d\xa4\xef\xb8\x8f tea", 3 },
    { "family emoji (joined)", "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", 1 },
    { "4-byte letter at the end", "x \xf0\x9d\x90\x80", 2 },
    /* Broken bytes (never from the editor): counted, never read past the end. */
    { "lone 4-byte lead at the end", "a \xf0", 2 },
    { "lone 3-byte lead at the end", "\xe2", 1 },
    { "cut 4-byte letter", "a \xf0\x9d\x90", 2 },
    { "stray continuation byte", "\x80 \x80", 2 },
    { "broken byte inside a word", "ab\xffz", 1 },
  };
  JrCases c = { .table = "count-words" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      guint got = jr_count_words (cases[i].input);
      g_autofree char *shown = cases[i].input ? g_strescape (cases[i].input, NULL) : g_strdup ("(null)");
      jr_case (&c, cases[i].name, got == cases[i].expected, "Input \"%s\"  Expected %u  Got %u", shown,
               cases[i].expected, got);
    }
  jr_cases_done (&c);
}

/* Pieces with a known answer: is this piece a gap between words? */
static const struct {
  const char *text;
  gboolean gap;
} PIECES[] = {
  { "a", FALSE },
  { "Word", FALSE },
  { "na\xc3\xafve", FALSE },
  { "\xe6\x97\xa5\xe6\x9c\xac", FALSE },
  { "\xe2\x9d\xa4\xef\xb8\x8f", FALSE },
  { "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9", FALSE },
  { "\xe2\x80\x94", FALSE },
  { "!", FALSE },
  { "\xe2\x80\x8b", FALSE }, /* zero-width space */
  { " ", TRUE },
  { "\t", TRUE },
  { "\n", TRUE },
  { "\r\n", TRUE },
  { "\xc2\xa0", TRUE },     /* no-break space */
  { "\xe2\x80\x83", TRUE }, /* em space */
  { "\xe3\x80\x80", TRUE }, /* ideographic space */
  { "\xe2\x80\xa8", TRUE }, /* line separator */
};

/* Hidden test: random texts built from the pieces above. The reference
 * counts each word piece that starts the text or follows a gap. */
static void
test_hidden_random_text (void)
{
  JrCases c = { .table = "hidden: random text" };
  GString *text = g_string_new (NULL);
  for (int n = 0; n < 20000; n++)
    {
      g_string_truncate (text, 0);
      guint want = 0;
      gboolean after_gap = TRUE;
      int pieces = g_test_rand_int_range (0, 60);
      for (int k = 0; k < pieces; k++)
        {
          int p = g_test_rand_int_range (0, G_N_ELEMENTS (PIECES));
          g_string_append (text, PIECES[p].text);
          if (!PIECES[p].gap && after_gap)
            want++;
          after_gap = PIECES[p].gap;
        }
      guint got = jr_count_words (text->str);
      g_autofree char *shown = g_strescape (text->str, NULL);
      jr_case (&c, "random text", got == want, "Input \"%s\"  Expected %u  Got %u", shown, want, got);
    }
  g_string_free (text, TRUE);
  jr_cases_done (&c);
}

/* Hidden test: random bytes, valid UTF-8 or not. Nothing is read past
 * the end (the sanitizer build checks), a word needs at least one byte,
 * and a gap appended at the end changes nothing. */
static void
test_hidden_random_bytes (void)
{
  JrCases c = { .table = "hidden: random bytes" };
  for (int n = 0; n < 20000; n++)
    {
      int len = g_test_rand_int_range (1, 24);
      char *bytes = g_malloc (len + 2); /* exact size, so the sanitizer sees any over-read */
      for (int k = 0; k < len; k++)
        bytes[k] = (char) g_test_rand_int_range (1, 256);
      bytes[len] = '\0';
      guint got = jr_count_words (bytes);
      bytes[len] = ' ';
      bytes[len + 1] = '\0';
      guint padded = jr_count_words (bytes);
      bytes[len] = '\0';
      g_autofree char *shown = g_strescape (bytes, NULL);
      jr_case (&c, "random bytes", got <= (guint) len && padded == got,
               "Input \"%s\" (%d bytes)  Expected at most %d words, same with a space after  Got %u, %u",
               shown, len, len, got, padded);
      g_free (bytes);
    }
  jr_cases_done (&c);
}

/* Max size: a 10 MB entry is counted exactly and quickly (the count runs
 * as the user types). The time limit is generous and skipped under
 * Valgrind and the sanitizers, which are many times slower. */
static void
test_huge_input (void)
{
  JrCases c = { .table = "count-words, huge input" };
  const int words = 2 * 1000 * 1000;
  GString *text = g_string_sized_new (words * 5 + 1);
  for (int k = 0; k < words; k++)
    g_string_append (text, k % 2 ? "word " : "na\xc3\xafve\n");
  gint64 start = g_get_monotonic_time ();
  guint got = jr_count_words (text->str);
  gint64 ms = (g_get_monotonic_time () - start) / 1000;
  jr_case (&c, "2,000,000 words", got == (guint) words, "Input %" G_GSIZE_FORMAT " bytes  Expected %d  Got %u",
           text->len, words, got);
  if (!INSTRUMENTED && g_getenv ("JR_TEST_WRAPPER") == NULL)
    jr_case (&c, "time limit", ms < 1000, "Input %" G_GSIZE_FORMAT " bytes  Expected under 1000 ms  Got %" G_GINT64_FORMAT " ms",
             text->len, ms);
  g_test_message ("counted %" G_GSIZE_FORMAT " bytes in %" G_GINT64_FORMAT " ms", text->len, ms);
  g_string_free (text, TRUE);
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/text/count-words", test_count_words);
  g_test_add_func ("/text/hidden/random-text", test_hidden_random_text);
  g_test_add_func ("/text/hidden/random-bytes", test_hidden_random_bytes);
  g_test_add_func ("/text/huge-input", test_huge_input);
  return g_test_run ();
}
