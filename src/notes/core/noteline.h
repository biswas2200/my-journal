/* noteline: the six kinds of line a note is made of, and how each is
 * stored.
 *
 * A note is stored as a small Markdown subset, one line per block, so an
 * export is plain Markdown and the AI reads it as is:
 *
 *   "# t" heading       "## t" subheading    "- t" bullet
 *   "  - t" sub-bullet  "→ t" arrow          anything else: text
 *
 * Reading also accepts what people type in Markdown mode: "*" or "+"
 * bullets, a tab or 2+ spaces before a sub-bullet, "->" for the arrow.
 * A text line that would read as one of those is stored with a leading
 * backslash (as Markdown escapes it), so every line comes back exactly
 * as written. */
#pragma once

#include <glib.h>

typedef enum {
  JN_TEXT,
  JN_HEADING,
  JN_SUBHEADING,
  JN_BULLET,
  JN_SUB_BULLET,
  JN_ARROW,
} JnKind;

/* The kind of a stored line; *text points into `line`, after the marker
 * (and after the escaping backslash of a text line). */
JnKind      jn_line_parse    (const char *line, const char **text);
/* The stored form of one line; newlines in `text` become spaces. */
char       *jn_line_format   (JnKind kind, const char *text);
/* Typing at the start of a line: when everything before the cursor is
 * a marker ("- ", "* ", "# ", "## ", "> ", "-> ", "→ ", "  - "), the
 * kind it turns the line into and how many bytes to remove; else TEXT/0. */
JnKind      jn_line_shortcut (const char *typed, gsize *prefix_len);
/* Tab and Shift+Tab: bullet ⇄ sub-bullet; Shift+Tab ends a bullet or
 * an arrow back into text. Other kinds stay. */
JnKind      jn_kind_indent   (JnKind kind);
JnKind      jn_kind_outdent  (JnKind kind);
/* What the editor shows before the text: "● ", "◦ ", "→ " or "". */
const char *jn_kind_glyph    (JnKind kind);
