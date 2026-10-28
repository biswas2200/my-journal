/* noteexport: a topic as one Markdown file, and the small pieces it
 * needs (when a session was, a safe file name).
 *
 *   # Prefix sum
 *
 *   Subject: DSA
 *
 *   ## 28 Oct 2026, 14:05–15:40
 *
 *   Sources:
 *   - <https://cp-algorithms.com/...>
 *
 *   ### Idea          (the note's own headings, two levels down)
 *   - pre[0] = 0
 *   ...
 */
#pragma once

#include <glib.h>

typedef struct JnExport JnExport;

/* "28 Oct 2026, 14:05–15:40" in local time; one time if under a minute
 * (or the clock went back); both dates when it crossed midnight. */
char     *jn_format_span        (gint64 started, gint64 ended);
/* "<topic>.md" that stays in its folder: no "/", "\" or reserved
 * characters, no leading dot, no control characters, at most 200 bytes;
 * "Untitled.md" if nothing is left. */
char     *jn_export_file_name   (const char *topic);

/* Where exports go: ~/Documents/Notes (the Documents folder in your
 * language, from xdg-user-dirs). */
char     *jn_export_dir         (void);

JnExport *jn_export_new         (const char *subject, const char *topic);
/* Sessions in the order they should appear (oldest first). */
void      jn_export_add_session (JnExport *e, gint64 started, gint64 ended, const char *sources, const char *body);
/* The Markdown text; frees `e`. */
char     *jn_export_finish      (JnExport *e);
