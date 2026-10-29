/* textify: a fetched web page as plain text for the AI to read.
 *
 * Keeps the words in order; drops tags, comments, and everything inside
 * script, style, head, svg, noscript, template, iframe, object and canvas;
 * decodes entities; turns blocks into blank lines, list items into "- "
 * lines and <br> into a newline; keeps <pre> as it is; collapses other
 * whitespace. Any input bytes are fine: invalid UTF-8 becomes U+FFFD and
 * control characters become spaces. One pass, linear time. */
#pragma once

#include <glib.h>

/* At most `limit` bytes, cut on a character boundary. */
char *jn_textify (const char *html, gsize len, gsize limit);
