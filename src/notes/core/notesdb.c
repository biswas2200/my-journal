/* notesdb: see notesdb.h. */
#include "notesdb.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#define VERSION 1 /* PRAGMA user_version */
#define STUDY_GAP (30 * 60)

struct JnDb {
  sqlite3 *h;
};

static const char SCHEMA[] =
  "CREATE TABLE IF NOT EXISTS subjects ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  name TEXT NOT NULL,"
  "  key TEXT NOT NULL UNIQUE,"        /* the name, case-folded */
  "  created INTEGER NOT NULL);"
  "CREATE TABLE IF NOT EXISTS topics ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  subject INTEGER NOT NULL REFERENCES subjects(id) ON DELETE CASCADE,"
  "  name TEXT NOT NULL,"
  "  key TEXT NOT NULL,"
  "  created INTEGER NOT NULL,"
  "  UNIQUE (subject, key));"
  "CREATE TABLE IF NOT EXISTS sessions ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  topic INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE,"
  "  started INTEGER NOT NULL,"
  "  ended INTEGER NOT NULL,"
  "  body TEXT NOT NULL DEFAULT '',"
  "  sources TEXT NOT NULL DEFAULT '');"
  "CREATE INDEX IF NOT EXISTS sessions_by_topic ON sessions (topic, started);"
  "CREATE TABLE IF NOT EXISTS enhanced ("
  "  session INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
  "  markdown TEXT NOT NULL,"
  "  model TEXT NOT NULL,"
  "  created INTEGER NOT NULL);"
  /* Study time from the stopwatch and timer: one row per stretch logged.
   * Added in place (older files gain it when opened), so still version 1. */
  "CREATE TABLE IF NOT EXISTS study_log ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  topic INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE,"
  "  at INTEGER NOT NULL,"
  "  seconds INTEGER NOT NULL CHECK (seconds > 0));"
  "CREATE INDEX IF NOT EXISTS study_by_topic ON study_log (topic, at);"
  "CREATE TABLE IF NOT EXISTS settings ("
  "  key TEXT PRIMARY KEY,"
  "  value TEXT NOT NULL);"
  "PRAGMA user_version = 1;";

static const char PRAGMAS[] =
  "PRAGMA foreign_keys = ON;"
  "PRAGMA synchronous = FULL;"
  "PRAGMA temp_store = MEMORY;";

static void
warn (JnDb *db)
{
  g_warning ("SQLite: %s", sqlite3_errmsg (db->h));
}

/* The folder (0700) and an empty file (0600) first, so SQLite never
 * creates it readable by others; an existing open file is made private. */
static gboolean
prepare_file (const char *path, GError **error)
{
  g_autofree char *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      int e = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e), "Cannot create %s: %s", dir, g_strerror (e));
      return FALSE;
    }
  int fd = g_open (path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      int e = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e), "Cannot open %s: %s", path, g_strerror (e));
      return FALSE;
    }
  struct stat st;
  gboolean ok = fstat (fd, &st) == 0 && ((st.st_mode & 077) == 0 || fchmod (fd, 0600) == 0);
  int e = errno;
  close (fd);
  if (!ok)
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e), "Cannot make %s private: %s", path,
                 g_strerror (e));
  return ok;
}

JnDb *
jn_db_open (const char *path, GError **error)
{
  gboolean memory = g_strcmp0 (path, ":memory:") == 0;
  if (!memory && !prepare_file (path, error))
    return NULL;
  JnDb *db = g_new0 (JnDb, 1);
  int rc = sqlite3_open_v2 (path, &db->h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_exec (db->h, PRAGMAS, NULL, NULL, NULL);
  int version = 0;
  sqlite3_stmt *s = NULL;
  if (rc == SQLITE_OK)
    rc = sqlite3_prepare_v2 (db->h, "PRAGMA user_version", -1, &s, NULL);
  if (rc == SQLITE_OK)
    {
      rc = sqlite3_step (s);
      if (rc == SQLITE_ROW)
        {
          version = sqlite3_column_int (s, 0);
          rc = SQLITE_OK;
        }
    }
  sqlite3_finalize (s);
  if (rc == SQLITE_OK && version > VERSION)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "These notes were made by a newer version of Notes (file version %d; this one reads up to "
                   "%d). Install the newer version to open them.", version, VERSION);
      jn_db_close (db);
      return NULL;
    }
  if (rc == SQLITE_OK)
    rc = sqlite3_exec (db->h, SCHEMA, NULL, NULL, NULL);
  if (rc != SQLITE_OK)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Cannot open the notes: %s",
                   db->h != NULL ? sqlite3_errmsg (db->h) : sqlite3_errstr (rc));
      jn_db_close (db);
      return NULL;
    }
  return db;
}

void
jn_db_close (JnDb *db)
{
  if (db == NULL)
    return;
  sqlite3_close (db->h);
  g_free (db);
}

/* ---- statements ------------------------------------------------------ */

static sqlite3_stmt *
prepare (JnDb *db, const char *sql)
{
  sqlite3_stmt *s = NULL;
  if (sqlite3_prepare_v2 (db->h, sql, -1, &s, NULL) != SQLITE_OK)
    warn (db);
  return s;
}

/* Steps a write. A broken uniqueness or parent rule is an expected
 * "no" (taken name, missing parent), not a warning. */
static gboolean
done (JnDb *db, sqlite3_stmt *s)
{
  int rc = s != NULL ? sqlite3_step (s) : SQLITE_ERROR;
  if (rc != SQLITE_DONE && (rc & 0xff) != SQLITE_CONSTRAINT && s != NULL)
    warn (db);
  sqlite3_finalize (s);
  return rc == SQLITE_DONE;
}

static void
bind_text (sqlite3_stmt *s, int i, const char *text)
{
  if (s != NULL)
    sqlite3_bind_text (s, i, text != NULL ? text : "", -1, SQLITE_TRANSIENT);
}

static void
bind_int (sqlite3_stmt *s, int i, gint64 v)
{
  if (s != NULL)
    sqlite3_bind_int64 (s, i, v);
}

/* Trimmed name and its case-folded key; FALSE if the name is empty. */
static gboolean
name_and_key (const char *raw, char **name, char **key)
{
  *name = g_strstrip (g_strdup (raw != NULL ? raw : ""));
  if (**name == '\0' || !g_utf8_validate (*name, -1, NULL))
    {
      g_clear_pointer (name, g_free);
      return FALSE;
    }
  g_autofree char *norm = g_utf8_normalize (*name, -1, G_NORMALIZE_NFKC);
  *key = g_utf8_casefold (norm, -1);
  return TRUE;
}

/* ---- subjects and topics --------------------------------------------- */

gint64
jn_db_add_subject (JnDb *db, const char *name)
{
  g_autofree char *n = NULL, *k = NULL;
  if (!name_and_key (name, &n, &k))
    return 0;
  sqlite3_stmt *s = prepare (db, "INSERT INTO subjects (name, key, created) VALUES (?, ?, ?)");
  bind_text (s, 1, n);
  bind_text (s, 2, k);
  bind_int (s, 3, g_get_real_time () / G_USEC_PER_SEC);
  return done (db, s) ? sqlite3_last_insert_rowid (db->h) : 0;
}

static gboolean
rename_in (JnDb *db, const char *sql, gint64 id, const char *name)
{
  g_autofree char *n = NULL, *k = NULL;
  if (!name_and_key (name, &n, &k))
    return FALSE;
  sqlite3_stmt *s = prepare (db, sql);
  bind_text (s, 1, n);
  bind_text (s, 2, k);
  bind_int (s, 3, id);
  return done (db, s) && sqlite3_changes (db->h) > 0;
}

static gboolean
delete_in (JnDb *db, const char *sql, gint64 id)
{
  sqlite3_stmt *s = prepare (db, sql);
  bind_int (s, 1, id);
  return done (db, s) && sqlite3_changes (db->h) > 0;
}

gboolean
jn_db_rename_subject (JnDb *db, gint64 id, const char *name)
{
  return rename_in (db, "UPDATE subjects SET name = ?, key = ? WHERE id = ?", id, name);
}

gboolean
jn_db_delete_subject (JnDb *db, gint64 id)
{
  return delete_in (db, "DELETE FROM subjects WHERE id = ?", id);
}

gint64
jn_db_add_topic (JnDb *db, gint64 subject, const char *name)
{
  g_autofree char *n = NULL, *k = NULL;
  if (!name_and_key (name, &n, &k))
    return 0;
  sqlite3_stmt *s = prepare (db, "INSERT INTO topics (subject, name, key, created) VALUES (?, ?, ?, ?)");
  bind_int (s, 1, subject);
  bind_text (s, 2, n);
  bind_text (s, 3, k);
  bind_int (s, 4, g_get_real_time () / G_USEC_PER_SEC);
  return done (db, s) ? sqlite3_last_insert_rowid (db->h) : 0;
}

gboolean
jn_db_rename_topic (JnDb *db, gint64 id, const char *name)
{
  return rename_in (db, "UPDATE topics SET name = ?, key = ? WHERE id = ?", id, name);
}

gboolean
jn_db_delete_topic (JnDb *db, gint64 id)
{
  return delete_in (db, "DELETE FROM topics WHERE id = ?", id);
}

gboolean
jn_db_get_topic (JnDb *db, gint64 id, gint64 *subject, char **subject_name, char **topic_name)
{
  sqlite3_stmt *s = prepare (db, "SELECT s.id, s.name, t.name FROM topics t JOIN subjects s ON s.id = t.subject"
                                 " WHERE t.id = ?");
  bind_int (s, 1, id);
  gboolean found = s != NULL && sqlite3_step (s) == SQLITE_ROW;
  if (found)
    {
      *subject = sqlite3_column_int64 (s, 0);
      *subject_name = g_strdup ((const char *) sqlite3_column_text (s, 1));
      *topic_name = g_strdup ((const char *) sqlite3_column_text (s, 2));
    }
  sqlite3_finalize (s);
  return found;
}

/* ---- sessions ---------------------------------------------------------- */

gint64
jn_session_extend (gint64 started, gint64 ended, gint64 now)
{
  (void) started;
  return now > ended && now - ended <= STUDY_GAP ? now : ended;
}

gint64
jn_db_start_session (JnDb *db, gint64 topic, gint64 now)
{
  sqlite3_stmt *s = prepare (db, "INSERT INTO sessions (topic, started, ended) VALUES (?, ?, ?)");
  bind_int (s, 1, topic);
  bind_int (s, 2, now);
  bind_int (s, 3, now);
  return done (db, s) ? sqlite3_last_insert_rowid (db->h) : 0;
}

gboolean
jn_db_get_session (JnDb *db, gint64 id, JnSession *out)
{
  sqlite3_stmt *s = prepare (db, "SELECT topic, started, ended, body, sources FROM sessions WHERE id = ?");
  bind_int (s, 1, id);
  gboolean found = s != NULL && sqlite3_step (s) == SQLITE_ROW;
  if (found)
    {
      out->id = id;
      out->topic = sqlite3_column_int64 (s, 0);
      out->started = sqlite3_column_int64 (s, 1);
      out->ended = sqlite3_column_int64 (s, 2);
      out->body = g_strdup ((const char *) sqlite3_column_text (s, 3));
      out->sources = g_strdup ((const char *) sqlite3_column_text (s, 4));
    }
  sqlite3_finalize (s);
  return found;
}

void
jn_session_clear (JnSession *s)
{
  g_clear_pointer (&s->body, g_free);
  g_clear_pointer (&s->sources, g_free);
}

gboolean
jn_db_save_session (JnDb *db, gint64 id, const char *body, const char *sources, gint64 now)
{
  JnSession cur = { 0 };
  if (!jn_db_get_session (db, id, &cur))
    return FALSE;
  jn_session_clear (&cur);
  sqlite3_stmt *s = prepare (db, "UPDATE sessions SET body = ?, sources = ?, ended = ? WHERE id = ?");
  bind_text (s, 1, body);
  bind_text (s, 2, sources);
  bind_int (s, 3, jn_session_extend (cur.started, cur.ended, now));
  bind_int (s, 4, id);
  return done (db, s) && sqlite3_changes (db->h) > 0;
}

gboolean
jn_db_drop_if_empty (JnDb *db, gint64 id)
{
  return delete_in (db, "DELETE FROM sessions WHERE id = ? AND trim(body, ' ' || char(9, 10, 13)) = ''"
                        " AND trim(sources, ' ' || char(9, 10, 13)) = ''", id);
}

gboolean
jn_db_delete_session (JnDb *db, gint64 id)
{
  return delete_in (db, "DELETE FROM sessions WHERE id = ?", id);
}

/* ---- enhanced copies, settings, style ------------------------------------ */

gboolean
jn_db_set_enhanced (JnDb *db, gint64 session, const char *markdown, const char *model, gint64 now)
{
  sqlite3_stmt *s = prepare (db, "INSERT INTO enhanced (session, markdown, model, created) VALUES (?, ?, ?, ?)"
                                 " ON CONFLICT (session) DO UPDATE SET markdown = excluded.markdown,"
                                 " model = excluded.model, created = excluded.created");
  bind_int (s, 1, session);
  bind_text (s, 2, markdown);
  bind_text (s, 3, model);
  bind_int (s, 4, now);
  return done (db, s); /* a missing session breaks the parent rule: FALSE */
}

char *
jn_db_get_enhanced (JnDb *db, gint64 session, char **model, gint64 *created)
{
  sqlite3_stmt *s = prepare (db, "SELECT markdown, model, created FROM enhanced WHERE session = ?");
  bind_int (s, 1, session);
  char *md = NULL;
  if (s != NULL && sqlite3_step (s) == SQLITE_ROW)
    {
      md = g_strdup ((const char *) sqlite3_column_text (s, 0));
      if (model != NULL)
        *model = g_strdup ((const char *) sqlite3_column_text (s, 1));
      if (created != NULL)
        *created = sqlite3_column_int64 (s, 2);
    }
  sqlite3_finalize (s);
  return md;
}

void
jn_db_foreach_enhanced (JnDb *db, gint64 topic, JnCopyFunc fn, gpointer user)
{
  sqlite3_stmt *s = prepare (db, "SELECT s.id, s.started, s.ended, e.markdown FROM enhanced e"
                                 " JOIN sessions s ON s.id = e.session WHERE s.topic = ? ORDER BY s.started, s.id");
  bind_int (s, 1, topic);
  while (s != NULL && sqlite3_step (s) == SQLITE_ROW)
    fn (sqlite3_column_int64 (s, 0), sqlite3_column_int64 (s, 1), sqlite3_column_int64 (s, 2),
        (const char *) sqlite3_column_text (s, 3), user);
  sqlite3_finalize (s);
}

gboolean
jn_db_set_setting (JnDb *db, const char *key, const char *value)
{
  sqlite3_stmt *s = prepare (db, "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)");
  bind_text (s, 1, key);
  bind_text (s, 2, value);
  return done (db, s);
}

char *
jn_db_get_setting (JnDb *db, const char *key)
{
  sqlite3_stmt *s = prepare (db, "SELECT value FROM settings WHERE key = ?");
  bind_text (s, 1, key);
  char *v = s != NULL && sqlite3_step (s) == SQLITE_ROW ? g_strdup ((const char *) sqlite3_column_text (s, 0)) : NULL;
  sqlite3_finalize (s);
  return v;
}

gboolean
jn_db_add_study (JnDb *db, gint64 topic, gint64 at, gint64 seconds)
{
  if (seconds <= 0 || seconds > JN_STUDY_MAX_RUN)
    return FALSE;
  sqlite3_stmt *s = prepare (db, "INSERT INTO study_log (topic, at, seconds) VALUES (?, ?, ?)");
  bind_int (s, 1, topic);
  bind_int (s, 2, at);
  bind_int (s, 3, seconds);
  return done (db, s);
}

gint64
jn_db_studied (JnDb *db, gint64 topic, gint64 since)
{
  sqlite3_stmt *s = prepare (db, "SELECT total(seconds) FROM study_log WHERE topic = ? AND at >= ?");
  bind_int (s, 1, topic);
  bind_int (s, 2, since);
  gint64 total = s != NULL && sqlite3_step (s) == SQLITE_ROW ? (gint64) sqlite3_column_double (s, 0) : 0;
  sqlite3_finalize (s);
  return total;
}

char **
jn_db_style_samples (JnDb *db, gint64 topic, int n, gsize max_bytes)
{
  GPtrArray *out = g_ptr_array_new ();
  sqlite3_stmt *s = prepare (db, "SELECT body FROM sessions WHERE topic != ? AND trim(body, ' ' || char(9, 10, 13)) != ''"
                                 " ORDER BY ended DESC, id DESC LIMIT ?");
  bind_int (s, 1, topic);
  bind_int (s, 2, MAX (n, 0));
  while (s != NULL && sqlite3_step (s) == SQLITE_ROW)
    {
      const char *body = (const char *) sqlite3_column_text (s, 0);
      gsize len = strlen (body);
      if (len > max_bytes)
        {
          len = max_bytes;
          while (len > 0 && ((guchar) body[len] & 0xC0) == 0x80)
            len--; /* on a character boundary */
        }
      g_ptr_array_add (out, g_strndup (body, len));
    }
  sqlite3_finalize (s);
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}

/* ---- lists -------------------------------------------------------------- */

static void
foreach_name (JnDb *db, const char *sql, gint64 parent, gboolean bind, JnNameFunc fn, gpointer user)
{
  sqlite3_stmt *s = prepare (db, sql);
  if (bind)
    bind_int (s, 1, parent);
  while (s != NULL && sqlite3_step (s) == SQLITE_ROW)
    fn (sqlite3_column_int64 (s, 0), (const char *) sqlite3_column_text (s, 1), user);
  sqlite3_finalize (s);
}

void
jn_db_foreach_subject (JnDb *db, JnNameFunc fn, gpointer user)
{
  foreach_name (db, "SELECT id, name FROM subjects ORDER BY key, id", 0, FALSE, fn, user);
}

void
jn_db_foreach_topic (JnDb *db, gint64 subject, JnNameFunc fn, gpointer user)
{
  foreach_name (db, "SELECT id, name FROM topics WHERE subject = ? ORDER BY key, id", subject, TRUE, fn, user);
}

void
jn_db_foreach_session (JnDb *db, gint64 topic, JnSessionFunc fn, gpointer user)
{
  sqlite3_stmt *s = prepare (db, "SELECT id, started, ended FROM sessions WHERE topic = ? ORDER BY started DESC, id DESC");
  bind_int (s, 1, topic);
  while (s != NULL && sqlite3_step (s) == SQLITE_ROW)
    fn (sqlite3_column_int64 (s, 0), sqlite3_column_int64 (s, 1), sqlite3_column_int64 (s, 2), user);
  sqlite3_finalize (s);
}
