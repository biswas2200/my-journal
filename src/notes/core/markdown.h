/* markdown: reading Markdown (the AI's enhanced copy, a topic reference)
 * into blocks and inline spans, to show it formatted.
 *
 * Forgiving by design: a code fence inside a list item, an unclosed
 * fence, odd indents or stray markup never fail; whatever is not markup
 * comes out as text, so nothing written is ever lost. Only http and
 * https addresses become links; any other link shows as written. Any
 * bytes are fine: broken UTF-8 comes out as U+FFFD. Spans of a text
 * over 16 KB are one plain text span, so reading takes little time
 * whatever the AI wrote. */
#pragma once

#include <glib.h>

typedef enum {
  JN_MD_PARAGRAPH,
  JN_MD_HEADING,   /* level 1-6 */
  JN_MD_BULLET,    /* level = depth 0-4 */
  JN_MD_NUMBERED,  /* level = the number */
  JN_MD_ARROW,     /* level = depth 0-4 */
  JN_MD_CODE,      /* lang may be NULL */
  JN_MD_SVG,       /* a ```svg block: text is the SVG (show only after svgsafe.h) */
  JN_MD_RULE,
  JN_MD_BLANK,
} JnMdKind;

typedef struct {
  JnMdKind kind;
  int      level;
  char    *lang;
  char    *text;
} JnMdBlock;

typedef enum {
  JN_SPAN_TEXT,
  JN_SPAN_BOLD,
  JN_SPAN_ITALIC,
  JN_SPAN_CODE,
  JN_SPAN_LINK,  /* url set */
  JN_SPAN_CITE,  /* "1" or "1, 2" from [1] or [1, 2] */
} JnMdSpanKind;

typedef struct {
  JnMdSpanKind kind;
  char        *text;
  char        *url;
} JnMdSpan;

/* Arrays of JnMdBlock* / JnMdSpan*, freed with g_ptr_array_unref. */
GPtrArray *jn_md_blocks (const char *markdown);
GPtrArray *jn_md_spans  (const char *text);
