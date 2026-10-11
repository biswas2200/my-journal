/* db: see db.h. */
#include "db.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#define JR_DB_ERROR (g_quark_from_static_string ("jr-db-error"))
#define JR_DB_VERSION 1 /* PRAGMA user_version; SCHEMA sets it */

/* Every statement is prepared once at open and reused, so steady-state
 * use allocates nothing. Order must match SQL[] below. */
enum {
  ST_GET_SETTING,
  ST_SET_SETTING,
  ST_DEL_SETTING,
  ST_INSERT_ENTRY,
  ST_UPDATE_ENTRY,
  ST_DELETE_ENTRY,
  ST_DAY_ENTRIES,
  ST_ADD_SECONDS,
  ST_GET_SECONDS,
  ST_DAY_SUMMARY,
  ST_MONTHS,
  ST_COUNT
};

static const char *const SQL[ST_COUNT] = {
  [ST_GET_SETTING] = "SELECT value FROM settings WHERE key = ?1",
  [ST_SET_SETTING] = "INSERT INTO settings(key, value) VALUES(?1, ?2) "
                     "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
  [ST_DEL_SETTING] = "DELETE FROM settings WHERE key = ?1",
  [ST_INSERT_ENTRY] = "INSERT INTO entries(day, stamped_at, body, updated_at) "
                      "VALUES(?1, ?2, ?3, ?4)",
  [ST_UPDATE_ENTRY] = "UPDATE entries SET body = ?2, updated_at = ?3 WHERE id = ?1",
  [ST_DELETE_ENTRY] = "DELETE FROM entries WHERE id = ?1",
  [ST_DAY_ENTRIES] = "SELECT id, stamped_at, body FROM entries WHERE day = ?1 "
                     "ORDER BY stamped_at, id",
  [ST_ADD_SECONDS] = "INSERT INTO day_time(day, active_seconds) VALUES(?1, ?2) "
                     "ON CONFLICT(day) DO UPDATE SET "
                     "active_seconds = active_seconds + excluded.active_seconds",
  [ST_GET_SECONDS] = "SELECT active_seconds FROM day_time WHERE day = ?1",
  [ST_DAY_SUMMARY] = "SELECT day, SUM(n), SUM(s) FROM ("
                     "  SELECT day, COUNT(*) AS n, 0 AS s FROM entries"
                     "   WHERE day BETWEEN ?1 AND ?2 GROUP BY day"
                     "  UNION ALL"
                     "  SELECT day, 0, active_seconds FROM day_time"
                     "   WHERE day BETWEEN ?1 AND ?2"
                     ") GROUP BY day ORDER BY day",
  [ST_MONTHS] = "SELECT substr(day, 1, 7) AS m, COUNT(DISTINCT day) FROM entries "
                "WHERE day < ?1 GROUP BY m ORDER BY m DESC",
};

static const char SCHEMA[] =
  "CREATE TABLE IF NOT EXISTS entries ("
  "  id          INTEGER PRIMARY KEY,"
  "  day         TEXT NOT NULL,"
  "  stamped_at  INTEGER NOT NULL,"
  "  body        TEXT NOT NULL,"
  "  updated_at  INTEGER NOT NULL"
  ");"
  "CREATE INDEX IF NOT EXISTS entries_day ON entries(day);"
  "CREATE TABLE IF NOT EXISTS day_time ("
  "  day            TEXT PRIMARY KEY,"
  "  active_seconds INTEGER NOT NULL DEFAULT 0"
  ");"
  "CREATE TABLE IF NOT EXISTS settings ("
  "  key   TEXT PRIMARY KEY,"
  "  value TEXT NOT NULL"
  ");"
  "PRAGMA user_version = 1;";

/* secure_delete overwrites deleted rows on disk; the small cache keeps
 * memory low (the whole journal is tiny anyway). */
static const char PRAGMAS[] =
  "PRAGMA secure_delete = ON;"
  "PRAGMA cache_size = -256;"
  "PRAGMA temp_store = MEMORY;"
  "PRAGMA synchronous = FULL;";

struct JrDb {
  sqlite3      *handle;
  sqlite3_stmt *st[ST_COUNT];
};

/* Creates the parent folder (0700) and an empty file (0600) so SQLite
 * never creates the journal world-readable under a lax umask. A file
 * that others can read (made by hand, copied or restored) is made
 * private: it holds dates, times and settings in the clear. */
static gboolean
prepare_file (const char *path, GError **error)
{
  g_autofree char *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      int e = errno;
      g_set_error (error, JR_DB_ERROR, 1, "Cannot create %s: %s", dir, g_strerror (e));
      return FALSE;
    }
  int fd = g_open (path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      int e = errno;
      g_set_error (error, JR_DB_ERROR, 1, "Cannot open %s: %s", path, g_strerror (e));
      return FALSE;
    }
  struct stat st;
  if (fstat (fd, &st) == 0 && (st.st_mode & 077) != 0 && fchmod (fd, 0600) != 0)
    {
      int e = errno;
      g_set_error (error, JR_DB_ERROR, 1, "Cannot make %s private: %s", path, g_strerror (e));
      close (fd);
      return FALSE;
    }
  close (fd);
  return TRUE;
}

/* PRAGMA user_version of the file; 0 for a new one. */
static int
read_version (sqlite3 *handle, int *version)
{
  sqlite3_stmt *s;
  int rc = sqlite3_prepare_v2 (handle, "PRAGMA user_version", -1, &s, NULL);
  if (rc != SQLITE_OK)
    return rc;
  rc = sqlite3_step (s);
  if (rc == SQLITE_ROW)
    {
      *version = sqlite3_column_int (s, 0);
      rc = SQLITE_OK;
    }
  sqlite3_finalize (s);
  return rc;
}

JrDb *
jr_db_open (const char *path, GError **error)
{
  gboolean in_memory = g_strcmp0 (path, ":memory:") == 0;
  if (!in_memory && !prepare_file (path, error))
    return NULL;

  JrDb *db = g_new0 (JrDb, 1);
  int rc = sqlite3_open_v2 (path, &db->handle,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                            NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_exec (db->handle, PRAGMAS, NULL, NULL, NULL);
  int version = 0;
  if (rc == SQLITE_OK)
    rc = read_version (db->handle, &version);
  if (rc == SQLITE_OK && version > JR_DB_VERSION)
    {
      /* Never rewrite a file this version does not understand. */
      g_set_error (error, JR_DB_ERROR, 2,
                   "This journal was made by a newer version of Journal (file version %d; "
                   "this one reads up to %d). Install the newer version to open it.",
                   version, JR_DB_VERSION);
      jr_db_close (db);
      return NULL;
    }
  if (rc == SQLITE_OK)
    rc = sqlite3_exec (db->handle, SCHEMA, NULL, NULL, NULL);
  for (int i = 0; rc == SQLITE_OK && i < ST_COUNT; i++)
    rc = sqlite3_prepare_v3 (db->handle, SQL[i], -1, SQLITE_PREPARE_PERSISTENT,
                             &db->st[i], NULL);
  if (rc != SQLITE_OK)
    {
      g_set_error (error, JR_DB_ERROR, rc, "Cannot open journal database: %s",
                   db->handle ? sqlite3_errmsg (db->handle) : sqlite3_errstr (rc));
      jr_db_close (db);
      return NULL;
    }
  return db;
}

void
jr_db_close (JrDb *db)
{
  if (db == NULL)
    return;
  for (int i = 0; i < ST_COUNT; i++)
    sqlite3_finalize (db->st[i]); /* NULL is a no-op */
  sqlite3_close (db->handle);
  g_free (db);
}

/* Resets and clears bindings so the next user starts clean and no
 * pointer to caller memory is kept between calls. */
static sqlite3_stmt *
stmt (JrDb *db, int which)
{
  sqlite3_stmt *s = db->st[which];
  sqlite3_reset (s);
  sqlite3_clear_bindings (s);
  return s;
}

/* For the foreach functions. A callback may run the same query again;
 * the shared statement is still stepping then, so that call gets a
 * statement of its own (resetting the shared one would restart the
 * outer loop). */
static sqlite3_stmt *
query (JrDb *db, int which, sqlite3_stmt **own)
{
  *own = NULL;
  if (!sqlite3_stmt_busy (db->st[which]))
    return stmt (db, which);
  if (sqlite3_prepare_v2 (db->handle, SQL[which], -1, own, NULL) != SQLITE_OK)
    return NULL;
  return *own;
}

/* Ends a query: SQLITE_DONE if every row was read. */
static int
query_done (sqlite3_stmt *s, sqlite3_stmt *own, int rc)
{
  if (own != NULL)
    sqlite3_finalize (own);
  else
    {
      sqlite3_reset (s);
      sqlite3_clear_bindings (s);
    }
  return rc;
}

/* A NULL pointer would bind SQL NULL, which the table refuses; an empty
 * body is a zero-length blob. */
static void
bind_body (sqlite3_stmt *s, int col, const void *body, gsize len)
{
  sqlite3_bind_blob64 (s, col, body != NULL ? body : "", body != NULL ? len : 0, SQLITE_STATIC);
}

static gboolean
done (JrDb *db, sqlite3_stmt *s)
{
  int rc = sqlite3_step (s);
  sqlite3_reset (s);
  sqlite3_clear_bindings (s);
  if (rc != SQLITE_DONE)
    {
      g_warning ("SQLite: %s", sqlite3_errmsg (db->handle));
      return FALSE;
    }
  return TRUE;
}

static gboolean
exec (JrDb *db, const char *sql)
{
  return sqlite3_exec (db->handle, sql, NULL, NULL, NULL) == SQLITE_OK;
}

gboolean jr_db_begin (JrDb *db) { return exec (db, "BEGIN IMMEDIATE"); }
gboolean jr_db_commit (JrDb *db) { return exec (db, "COMMIT"); }
void jr_db_rollback (JrDb *db) { exec (db, "ROLLBACK"); }

char *
jr_db_get_setting (JrDb *db, const char *key)
{
  sqlite3_stmt *s = stmt (db, ST_GET_SETTING);
  sqlite3_bind_text (s, 1, key, -1, SQLITE_STATIC);
  char *value = NULL;
  if (sqlite3_step (s) == SQLITE_ROW)
    value = g_strdup ((const char *) sqlite3_column_text (s, 0));
  sqlite3_reset (s);
  return value;
}

gboolean
jr_db_set_setting (JrDb *db, const char *key, const char *value)
{
  sqlite3_stmt *s = stmt (db, ST_SET_SETTING);
  sqlite3_bind_text (s, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text (s, 2, value, -1, SQLITE_STATIC);
  return done (db, s);
}

gboolean
jr_db_delete_setting (JrDb *db, const char *key)
{
  sqlite3_stmt *s = stmt (db, ST_DEL_SETTING);
  sqlite3_bind_text (s, 1, key, -1, SQLITE_STATIC);
  return done (db, s);
}

gint64
jr_db_insert_entry (JrDb *db, const char *day, gint64 stamped_at,
                    const void *body, gsize len, gint64 now)
{
  sqlite3_stmt *s = stmt (db, ST_INSERT_ENTRY);
  sqlite3_bind_text (s, 1, day, -1, SQLITE_STATIC);
  sqlite3_bind_int64 (s, 2, stamped_at);
  bind_body (s, 3, body, len);
  sqlite3_bind_int64 (s, 4, now);
  if (!done (db, s))
    return 0;
  return sqlite3_last_insert_rowid (db->handle);
}

gboolean
jr_db_update_entry (JrDb *db, gint64 id, const void *body, gsize len, gint64 now)
{
  sqlite3_stmt *s = stmt (db, ST_UPDATE_ENTRY);
  sqlite3_bind_int64 (s, 1, id);
  bind_body (s, 2, body, len);
  sqlite3_bind_int64 (s, 3, now);
  return done (db, s) && sqlite3_changes (db->handle) == 1;
}

gboolean
jr_db_delete_entry (JrDb *db, gint64 id)
{
  sqlite3_stmt *s = stmt (db, ST_DELETE_ENTRY);
  sqlite3_bind_int64 (s, 1, id);
  return done (db, s);
}

gboolean
jr_db_foreach_entry (JrDb *db, const char *day, JrDbEntryFunc fn, gpointer user)
{
  sqlite3_stmt *own, *s = query (db, ST_DAY_ENTRIES, &own);
  if (s == NULL)
    return FALSE;
  sqlite3_bind_text (s, 1, day, -1, SQLITE_STATIC);
  int rc;
  while ((rc = sqlite3_step (s)) == SQLITE_ROW)
    fn (sqlite3_column_int64 (s, 0), sqlite3_column_int64 (s, 1),
        sqlite3_column_blob (s, 2), (gsize) sqlite3_column_bytes (s, 2), user);
  return query_done (s, own, rc) == SQLITE_DONE;
}

gboolean
jr_db_add_active_seconds (JrDb *db, const char *day, gint64 seconds)
{
  if (seconds <= 0) /* nothing to add; never take time away */
    return TRUE;
  sqlite3_stmt *s = stmt (db, ST_ADD_SECONDS);
  sqlite3_bind_text (s, 1, day, -1, SQLITE_STATIC);
  sqlite3_bind_int64 (s, 2, seconds);
  return done (db, s);
}

gint64
jr_db_active_seconds (JrDb *db, const char *day)
{
  sqlite3_stmt *s = stmt (db, ST_GET_SECONDS);
  sqlite3_bind_text (s, 1, day, -1, SQLITE_STATIC);
  gint64 v = sqlite3_step (s) == SQLITE_ROW ? sqlite3_column_int64 (s, 0) : 0;
  sqlite3_reset (s);
  return v;
}

gboolean
jr_db_foreach_day (JrDb *db, const char *from, const char *to, JrDbDayFunc fn, gpointer user)
{
  sqlite3_stmt *own, *s = query (db, ST_DAY_SUMMARY, &own);
  if (s == NULL)
    return FALSE;
  sqlite3_bind_text (s, 1, from, -1, SQLITE_STATIC);
  sqlite3_bind_text (s, 2, to, -1, SQLITE_STATIC);
  int rc;
  while ((rc = sqlite3_step (s)) == SQLITE_ROW)
    {
      /* A damaged index can lead to a row with no day: skip it. */
      const char *day = (const char *) sqlite3_column_text (s, 0);
      if (day != NULL)
        fn (day, sqlite3_column_int (s, 1), sqlite3_column_int64 (s, 2), user);
    }
  return query_done (s, own, rc) == SQLITE_DONE;
}

gboolean
jr_db_foreach_month (JrDb *db, const char *before_month, JrDbMonthFunc fn, gpointer user)
{
  sqlite3_stmt *own, *s = query (db, ST_MONTHS, &own);
  if (s == NULL)
    return FALSE;
  /* "YYYY-MM" sorts before every "YYYY-MM-DD" of that month. */
  sqlite3_bind_text (s, 1, before_month, -1, SQLITE_STATIC);
  int rc;
  while ((rc = sqlite3_step (s)) == SQLITE_ROW)
    {
      const char *month = (const char *) sqlite3_column_text (s, 0);
      if (month != NULL) /* as for days: never hand on a missing one */
        fn (month, sqlite3_column_int (s, 1), user);
    }
  return query_done (s, own, rc) == SQLITE_DONE;
}
