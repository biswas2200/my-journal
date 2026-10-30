/* noteexport: a topic as one Markdown file, and the small pieces it
 * needs (when a session was, a safe file name).
 *
 *   # Prefix sum
 *
 *   Subject: DSA
 *   Time studied: 3 h 10 min   (stopwatch and timer, when there is any)
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
/* "Time studied: 3 h 10 min" under the subject; before any session.
 * Nothing for no time. */
void      jn_export_set_studied (JnExport *e, gint64 seconds);
/* Sessions in the order they should appear (oldest first). */
void      jn_export_add_session (JnExport *e, gint64 started, gint64 ended, const char *sources, const char *body);
/* A topic reference instead: every enhanced copy of the topic, oldest
 * first, each under its session's time, its title and header line left
 * out, its headings one level down, its sources kept with it. Finished
 * with jn_export_finish like a topic. */
JnExport *jn_export_reference_new (const char *subject, const char *topic);
void      jn_export_reference_add (JnExport *e, gint64 started, gint64 ended, const char *enhanced);
/* The Markdown text; frees `e`. */
char     *jn_export_finish      (JnExport *e);
