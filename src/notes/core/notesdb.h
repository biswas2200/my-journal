/* notesdb: the Notes store, one SQLite file, separate from the Journal.
 *
 * Subjects (DSA) hold topics (Prefix sum), which hold study sessions:
 * one note each, with its start and end time and the links studied
 * from. Notes are study material, so the file is not encrypted; it is
 * private to you (0600 in a 0700 folder) like the journal.
 *
 * Names are trimmed, not empty, and unique ignoring case (any script):
 * subjects among subjects, topics within their subject. Ids count from 1
 * per table and are never reused. Deleting a subject or topic deletes
 * everything in it. */
#pragma once

#include <glib.h>

typedef struct JnDb JnDb;

typedef struct {
  gint64 id, topic, started, ended;
  char  *body;    /* stored lines (noteline.h) */
  char  *sources; /* one link per line */
} JnSession;

typedef void (*JnNameFunc)    (gint64 id, const char *name, gpointer user);
typedef void (*JnSessionFunc) (gint64 id, gint64 started, gint64 ended, gpointer user);

/* Opens or creates the file; ":memory:" for a scratch store. Refuses a
 * file made by a newer version, leaving it untouched. */
JnDb    *jn_db_open             (const char *path, GError **error);
void     jn_db_close            (JnDb *db);

/* 0 / FALSE when the name is empty or taken, or the parent is missing. */
gint64   jn_db_add_subject      (JnDb *db, const char *name);
gboolean jn_db_rename_subject   (JnDb *db, gint64 id, const char *name);
gboolean jn_db_delete_subject   (JnDb *db, gint64 id);
gint64   jn_db_add_topic        (JnDb *db, gint64 subject, const char *name);
gboolean jn_db_rename_topic     (JnDb *db, gint64 id, const char *name);
gboolean jn_db_delete_topic     (JnDb *db, gint64 id);

/* A topic's subject id and both names (free them); FALSE if missing. */
gboolean jn_db_get_topic        (JnDb *db, gint64 id, gint64 *subject, char **subject_name, char **topic_name);

/* A new, empty session that starts (and for now ends) at `now`. */
gint64   jn_db_start_session    (JnDb *db, gint64 topic, gint64 now);
/* Saves body and sources; the end time follows jn_session_extend. */
gboolean jn_db_save_session     (JnDb *db, gint64 id, const char *body, const char *sources, gint64 now);
gboolean jn_db_get_session      (JnDb *db, gint64 id, JnSession *out);
void     jn_session_clear       (JnSession *s);
/* Removes the session if nothing was written in it (blank body and no
 * links); TRUE if it was removed. */
gboolean jn_db_drop_if_empty    (JnDb *db, gint64 id);
gboolean jn_db_delete_session   (JnDb *db, gint64 id);

/* The AI's enhanced copy of a session (one per session; enhancing again
 * replaces it; it goes when the session goes). Never the note itself. */
gboolean jn_db_set_enhanced     (JnDb *db, gint64 session, const char *markdown, const char *model, gint64 now);
/* NULL if there is none; *model and *created are filled in when found. */
char    *jn_db_get_enhanced     (JnDb *db, gint64 session, char **model, gint64 *created);

/* A topic's enhanced copies, oldest session first. */
typedef void (*JnCopyFunc) (gint64 session, gint64 started, gint64 ended, const char *markdown, gpointer user);
void     jn_db_foreach_enhanced (JnDb *db, gint64 topic, JnCopyFunc fn, gpointer user);

/* Small settings (e.g. "ai_consent"). */
gboolean jn_db_set_setting      (JnDb *db, const char *key, const char *value);
char    *jn_db_get_setting      (JnDb *db, const char *key);

/* Up to `n` recent, non-empty notes from topics other than `topic`,
 * newest first, each cut to `max_bytes`: to show the AI how this person
 * writes. NULL-terminated. */
char   **jn_db_style_samples    (JnDb *db, gint64 topic, int n, gsize max_bytes);

/* Sorted by name ignoring case; sessions newest first. */
void     jn_db_foreach_subject  (JnDb *db, JnNameFunc fn, gpointer user);
void     jn_db_foreach_topic    (JnDb *db, gint64 subject, JnNameFunc fn, gpointer user);
void     jn_db_foreach_session  (JnDb *db, gint64 topic, JnSessionFunc fn, gpointer user);

/* The end of a session after an edit at `now`: while you are studying
 * (the edit is within 30 minutes after the current end) it follows the
 * edit; a later edit (fixing a typo next week) or a clock that went back
 * leaves it, so the session keeps the time you actually studied. */
gint64   jn_session_extend      (gint64 started, gint64 ended, gint64 now);
