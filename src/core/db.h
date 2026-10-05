/* db: the SQLite file. Only SQL lives here; no crypto, no policy.
 *
 * Rows are handed to callbacks one at a time straight from SQLite's
 * buffers, so nothing is copied into lists and nothing lingers in memory.
 */
#pragma once

#include <glib.h>

#define JR_DB_DAY_LEN 11

typedef struct JrDb JrDb;

/* Opens (creating if needed) the database at `path`, or ":memory:".
 * A new file gets mode 0600 and a new parent folder 0700. */
JrDb    *jr_db_open             (const char *path, GError **error);
void     jr_db_close            (JrDb *db);

gboolean jr_db_begin            (JrDb *db);
gboolean jr_db_commit           (JrDb *db);
void     jr_db_rollback         (JrDb *db);

/* Returns a newly allocated value (g_free) or NULL if missing. */
char    *jr_db_get_setting      (JrDb *db, const char *key);
gboolean jr_db_set_setting      (JrDb *db, const char *key, const char *value);
gboolean jr_db_delete_setting   (JrDb *db, const char *key);

/* Returns the new row id, or 0 on failure. `body` may hold any bytes. */
gint64   jr_db_insert_entry     (JrDb *db, const char *day, gint64 stamped_at,
                                 const void *body, gsize len, gint64 now);
gboolean jr_db_update_entry     (JrDb *db, gint64 id, const void *body, gsize len, gint64 now);
gboolean jr_db_delete_entry     (JrDb *db, gint64 id);

/* `body` is only valid during the call. */
typedef void (*JrDbEntryFunc) (gint64 id, gint64 stamped_at, const void *body, gsize len,
                               gpointer user);
/* Entries of one day, oldest stamp first. */
gboolean jr_db_foreach_entry    (JrDb *db, const char *day, JrDbEntryFunc fn, gpointer user);

gboolean jr_db_add_active_seconds (JrDb *db, const char *day, gint64 seconds);
gint64   jr_db_active_seconds     (JrDb *db, const char *day);

/* Days in [from, to] that have entries or counted time, ascending. */
typedef void (*JrDbDayFunc) (const char *day, int entries, gint64 seconds, gpointer user);
gboolean jr_db_foreach_day      (JrDb *db, const char *from, const char *to,
                                 JrDbDayFunc fn, gpointer user);

/* Months ("YYYY-MM") strictly before `before_month` with at least one entry,
 * newest first, with the number of distinct days written. */
typedef void (*JrDbMonthFunc) (const char *month, int days_written, gpointer user);
gboolean jr_db_foreach_month    (JrDb *db, const char *before_month,
                                 JrDbMonthFunc fn, gpointer user);
