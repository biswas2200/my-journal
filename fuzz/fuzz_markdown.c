/* Fuzz target: the Markdown reader that shows the AI's enhanced copy
 * and the topic reference (the AI read web pages, so its text is not
 * trusted). Blocks have sane kinds and levels, no blank line leads,
 * ends or repeats; spans are never empty, runs of text are one span,
 * only http(s) links come out, a citation is only numbers; all text
 * comes out valid UTF-8, whatever went in; and a line with no markup
 * in it is shown exactly as written. */
#define FUZZ_NOTES
#include <string.h>
#include "fuzz.h"
#include "markdown.h"

static void
check_spans (const char *text)
{
  g_autoptr (GPtrArray) spans = jn_md_spans (text);
  GString *all = g_string_new (NULL);
  for (guint i = 0; i < spans->len; i++)
    {
      const JnMdSpan *s = spans->pdata[i];
      FUZZ_CHECK (s->kind >= JN_SPAN_TEXT && s->kind <= JN_SPAN_CITE);
      FUZZ_CHECK (s->text != NULL && *s->text != '\0');
      FUZZ_CHECK (g_utf8_validate (s->text, -1, NULL));
      if (s->kind == JN_SPAN_TEXT && i > 0)
        FUZZ_CHECK (((const JnMdSpan *) spans->pdata[i - 1])->kind != JN_SPAN_TEXT);
      if (s->kind == JN_SPAN_LINK)
        FUZZ_CHECK (g_ascii_strncasecmp (s->url, "https://", 8) == 0 || g_ascii_strncasecmp (s->url, "http://", 7) == 0);
      else
        FUZZ_CHECK (s->url == NULL);
      if (s->kind == JN_SPAN_CITE)
        FUZZ_CHECK (strspn (s->text, "0123456789, ") == strlen (s->text));
      g_string_append (all, s->text);
    }
  if (strpbrk (text, "\\$`*_[<") == NULL)
    FUZZ_CHECK (strcmp (all->str, text) == 0);
  g_string_free (all, TRUE);
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  g_autofree char *text = fuzz_string (data, size);
  g_autoptr (GPtrArray) blocks = jn_md_blocks (text);
  for (guint i = 0; i < blocks->len; i++)
    {
      const JnMdBlock *b = blocks->pdata[i];
      FUZZ_CHECK (b->kind >= JN_MD_PARAGRAPH && b->kind <= JN_MD_BLANK);
      if (b->kind == JN_MD_BLANK)
        FUZZ_CHECK (i > 0 && i + 1 < blocks->len && ((const JnMdBlock *) blocks->pdata[i - 1])->kind != JN_MD_BLANK);
      if (b->kind == JN_MD_HEADING)
        FUZZ_CHECK (b->level >= 1 && b->level <= 6);
      else if (b->kind == JN_MD_BULLET || b->kind == JN_MD_ARROW)
        FUZZ_CHECK (b->level >= 0 && b->level <= 4);
      else if (b->kind == JN_MD_NUMBERED)
        FUZZ_CHECK (b->level >= 0);
      else
        FUZZ_CHECK (b->level == 0);
      FUZZ_CHECK (b->lang == NULL || b->kind == JN_MD_CODE);
      gboolean empty = b->kind == JN_MD_BLANK || b->kind == JN_MD_RULE;
      FUZZ_CHECK ((b->text == NULL) == empty);
      if (empty)
        continue;
      FUZZ_CHECK (g_utf8_validate (b->text, -1, NULL));
      if (b->kind == JN_MD_CODE || b->kind == JN_MD_SVG)
        continue;
      FUZZ_CHECK (strchr (b->text, '\n') == NULL);
      check_spans (b->text);
    }
  return 0;
}
