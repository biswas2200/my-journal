/* notebackup: daily copies of notes.db, and rescue copies of a note
 * that could not be saved.
 *
 * Backups: ~/.local/share/journal-notes/backups/notes-2026-11-01.db, one
 * per day (local date), made when Notes opens and again when it closes,
 * so the day's copy is the state at the last close. The newest
 * JN_BACKUP_KEEP days are kept; older ones are removed, and nothing else
 * in the folder is ever touched. Each copy is whole and checked before it
 * replaces anything (jn_db_copy_to). To restore one: close Notes, copy it
 * over notes.db.
 *
 * Rescue copies: when a save fails, the note as Markdown goes to
 * ~/Documents/Notes/Unsaved/"<subject> - <topic> <date> <time>.md", so
 * the text is on disk whatever happens next. */
#pragma once

#include <glib.h>
#include "notesdb.h"

#define JN_BACKUP_KEEP 14

/* "notes-2026-11-01.db" for the local day of `now` (Unix seconds). */
char     *jn_backup_name      (gint64 now);
/* Of `names` (a folder listing), the backups to remove so the newest
 * `keep` remain: only names of the form notes-YYYY-MM-DD.db, oldest
 * first. NULL-terminated; free with g_strfreev. */
char    **jn_backup_prune     (const char *const *names, guint keep);
/* Today's copy in `dir` (made 0700 if missing), then the pruning. */
gboolean  jn_backup_now       (JnDb *db, const char *dir, gint64 now, guint keep, GError **error);
/* The newest backup in `dir`, or NULL. */
char     *jn_backup_latest    (const char *dir);

/* "DSA - Prefix sum 2026-11-01 14.05.33.md" in local time, safe as a
 * file name (see jn_export_file_name). */
char     *jn_rescue_file_name (const char *subject, const char *topic, gint64 now);
/* Writes `markdown` to dir/name (dir made 0700 if missing; the file 0600,
 * written in full and synced before it appears). */
gboolean  jn_rescue_write     (const char *dir, const char *name, const char *markdown, GError **error);
