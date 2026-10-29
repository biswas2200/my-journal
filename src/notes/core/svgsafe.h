/* svgsafe: an AI-drawn SVG made safe to show.
 *
 * The model read web pages, so its SVG is untrusted. Only drawing is
 * kept, from allow-lists: shapes, paths, text, groups, markers and
 * gradients, with their geometry and paint attributes. Removed: scripts,
 * event handlers, links (their text stays), images, embedded documents,
 * <use>, style sheets and style attributes, url() other than "#id".
 * Refused outright: a DOCTYPE or unknown entity, a root that is not
 * <svg>, a size over 4000, more than 2000 elements, nesting over 64 deep,
 * input over 256 KB. The result is re-written, escaped XML. */
#pragma once

#include <glib.h>

#define JN_SVG_MAX_BYTES (256 * 1024)

/* The cleaned SVG, or NULL if it was refused. */
char *jn_svg_clean (const char *svg, gssize len);
