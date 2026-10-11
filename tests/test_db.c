/* Tests for the SQLite layer (src/core/db.c), LeetCode style (see
 * cases.h). Most cases are written like LeetCode's "design a class"
 * problems: a list of operations, each with the output expected.
 *
 *   set K V          -> true             get K -> V, or null
 *   unset K          -> true
 *   add DAY STAMP B  -> id               (that entry is #1, #2, ... below)
 *   update #N B      -> true / false     delete #N -> true
 *   entries DAY      -> "stamp:body ..." oldest stamp first
 *   time+ DAY S      -> true             time DAY -> seconds
 *   days FROM TO     -> "day:entries:seconds ..."
 *   months BEFORE    -> "YYYY-MM:days ..." newest first
 *   begin, commit    -> true / false     rollback -> ok
 *   reopen           -> ok               (the app quits and starts again)
 *
 * Days may be short: "10-06" is 2026-10-06. '' is an empty string, NULL
 * a NULL body, and \0 a zero byte in a body. #9 is an entry that never
 * existed. A failed SQLite call shows its warning after the result. */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "cases.h"
#include "db.h"

#ifdef __SANITIZE_ADDRESS__
#define INSTRUMENTED 1
#else
#define INSTRUMENTED 0
#endif

/* Under Valgrind or the sanitizers: smaller inputs, no time limits. */
static gboolean
slow_run (void)
{
  return INSTRUMENTED || g_getenv ("JR_TEST_WRAPPER") != NULL;
}

/* ---- SQLite warnings: recorded, not fatal --------------------------- */

static char *last_warning;

/* SQLite failures are logged as warnings by db.c; the case shows them
 * next to the result. Any other warning still fails the test. */
static void
note_warning (const char *domain, GLogLevelFlags level, const char *message, gpointer user)
{
  (void) domain; (void) level; (void) user;
  if (g_str_has_prefix (message, "SQLite: "))
    {
      g_free (last_warning);
      last_warning = g_strdup (message);
    }
  else
    g_test_fail_printf ("unexpected warning: %s", message);
}

/* ---- Helpers -------------------------------------------------------- */

static void
remove_tree (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  if (dir != NULL)
    {
      const char *name;
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, name, NULL);
          remove_tree (child);
        }
      g_dir_close (dir);
      g_rmdir (path);
    }
  else
    g_remove (path);
}

static JrDb *
open_or_fail (const char *path)
{
  GError *error = NULL;
  JrDb *db = jr_db_open (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (db);
  return db;
}

/* "10-06" -> "2026-10-06", "09" -> "2026-09", '' -> "". */
static char *
arg (const char *w)
{
  if (strcmp (w, "''") == 0)
    return g_strdup ("");
  if (strlen (w) == 5 && w[2] == '-')
    return g_strconcat ("2026-", w, NULL);
  if (strlen (w) == 2 && g_ascii_isdigit (w[0]))
    return g_strconcat ("2026-", w, NULL);
  return g_strdup (w);
}

/* Bytes of a body word; \0 is a zero byte. NULL for "NULL". */
static GBytes *
body_arg (const char *w)
{
  if (strcmp (w, "NULL") == 0)
    return NULL;
  if (strcmp (w, "''") == 0)
    return g_bytes_new ("", 0);
  GByteArray *b = g_byte_array_new ();
  for (const char *p = w; *p; p++)
    {
      guint8 c = (guint8) *p;
      if (p[0] == '\\' && p[1] == '0')
        {
          c = 0;
          p++;
        }
      g_byte_array_append (b, &c, 1);
    }
  return g_byte_array_free_to_bytes (b);
}

static void
show_bytes (GString *out, const void *data, gsize len)
{
  const guint8 *b = data;
  if (len == 0)
    g_string_append (out, "''");
  for (gsize i = 0; i < len; i++)
    if (b[i] == 0)
      g_string_append (out, "\\0");
    else if (b[i] < 0x20 || b[i] >= 0x7f || b[i] == '\\')
      g_string_append_printf (out, "\\x%02x", b[i]);
    else
      g_string_append_c (out, (char) b[i]);
}

static void
sep (GString *s)
{
  if (s->len > 0)
    g_string_append_c (s, ' ');
}

static void
list_entry (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  (void) id;
  GString *s = user;
  sep (s);
  g_string_append_printf (s, "%" G_GINT64_FORMAT ":", stamped_at);
  show_bytes (s, body, len);
}

static void
list_day (const char *day, int entries, gint64 seconds, gpointer user)
{
  GString *s = user;
  sep (s);
  g_string_append_printf (s, "%s:%d:%" G_GINT64_FORMAT, day, entries, seconds);
}

static void
list_month (const char *month, int days, gpointer user)
{
  GString *s = user;
  sep (s);
  g_string_append_printf (s, "%s:%d", month, days);
}

/* ---- The operation table -------------------------------------------- */

typedef struct {
  const char *op;
  const char *expected;
} Step;

#define MAX_STEPS 10
#define MAX_IDS 8

typedef struct {
  JrDb *db;
  char *path;
  gint64 ids[MAX_IDS];
  gboolean alive[MAX_IDS];
  int n_ids;
} Ctx;

static gint64
id_arg (Ctx *x, const char *w)
{
  int n = (int) g_ascii_strtoll (w + 1, NULL, 10);
  return n >= 1 && n <= x->n_ids ? x->ids[n - 1] : G_GINT64_CONSTANT (999999);
}

static char *
yes (gboolean ok)
{
  return g_strdup (ok ? "true" : "false");
}

/* Runs one operation; returns what it gave, as text. */
static char *
run_op (Ctx *x, const char *line)
{
  g_auto (GStrv) w = g_strsplit (line, " ", 0);
  const char *op = w[0];
  guint n = g_strv_length (w);
  if (strcmp (op, "set") == 0 && n == 3)
    {
      g_autofree char *k = arg (w[1]), *v = arg (w[2]);
      return yes (jr_db_set_setting (x->db, k, v));
    }
  if (strcmp (op, "get") == 0 && n == 2)
    {
      g_autofree char *k = arg (w[1]);
      char *v = jr_db_get_setting (x->db, k);
      if (v == NULL)
        return g_strdup ("null");
      if (*v == '\0')
        {
          g_free (v);
          return g_strdup ("''");
        }
      return v;
    }
  if (strcmp (op, "unset") == 0 && n == 2)
    {
      g_autofree char *k = arg (w[1]);
      return yes (jr_db_delete_setting (x->db, k));
    }
  if (strcmp (op, "add") == 0 && n == 4)
    {
      g_autofree char *day = arg (w[1]);
      g_autoptr (GBytes) body = body_arg (w[3]);
      gsize len = 0;
      const void *data = body ? g_bytes_get_data (body, &len) : NULL;
      gint64 id = jr_db_insert_entry (x->db, day, g_ascii_strtoll (w[2], NULL, 10), data, len, 1);
      gboolean fresh = id > 0;
      for (int i = 0; i < x->n_ids; i++)
        fresh = fresh && !(x->alive[i] && x->ids[i] == id);
      g_assert_cmpint (x->n_ids, <, MAX_IDS);
      x->ids[x->n_ids] = id;
      x->alive[x->n_ids++] = TRUE;
      return fresh ? g_strdup ("id") : g_strdup_printf ("%" G_GINT64_FORMAT " (not a new id)", id);
    }
  if (strcmp (op, "update") == 0 && n == 3)
    {
      g_autoptr (GBytes) body = body_arg (w[2]);
      gsize len = 0;
      const void *data = body ? g_bytes_get_data (body, &len) : NULL;
      return yes (jr_db_update_entry (x->db, id_arg (x, w[1]), data, len, 2));
    }
  if (strcmp (op, "delete") == 0 && n == 2)
    {
      gint64 id = id_arg (x, w[1]);
      for (int i = 0; i < x->n_ids; i++)
        x->alive[i] = x->alive[i] && x->ids[i] != id;
      return yes (jr_db_delete_entry (x->db, id));
    }
  if (strcmp (op, "entries") == 0 && n == 2)
    {
      g_autofree char *day = arg (w[1]);
      GString *s = g_string_new (NULL);
      gboolean ok = jr_db_foreach_entry (x->db, day, list_entry, s);
      if (!ok)
        g_string_append (s, " (failed)");
      return g_string_free (s, FALSE);
    }
  if (strcmp (op, "time+") == 0 && n == 3)
    {
      g_autofree char *day = arg (w[1]);
      return yes (jr_db_add_active_seconds (x->db, day, g_ascii_strtoll (w[2], NULL, 10)));
    }
  if (strcmp (op, "time") == 0 && n == 2)
    {
      g_autofree char *day = arg (w[1]);
      return g_strdup_printf ("%" G_GINT64_FORMAT, jr_db_active_seconds (x->db, day));
    }
  if (strcmp (op, "days") == 0 && n == 3)
    {
      g_autofree char *from = arg (w[1]), *to = arg (w[2]);
      GString *s = g_string_new (NULL);
      if (!jr_db_foreach_day (x->db, from, to, list_day, s))
        g_string_append (s, " (failed)");
      return g_string_free (s, FALSE);
    }
  if (strcmp (op, "months") == 0 && n == 2)
    {
      g_autofree char *before = arg (w[1]);
      GString *s = g_string_new (NULL);
      if (!jr_db_foreach_month (x->db, before, list_month, s))
        g_string_append (s, " (failed)");
      return g_string_free (s, FALSE);
    }
  if (strcmp (op, "begin") == 0)
    return yes (jr_db_begin (x->db));
  if (strcmp (op, "commit") == 0)
    return yes (jr_db_commit (x->db));
  if (strcmp (op, "rollback") == 0)
    {
      jr_db_rollback (x->db);
      return g_strdup ("ok");
    }
  if (strcmp (op, "reopen") == 0)
    {
      jr_db_close (x->db);
      GError *error = NULL;
      x->db = jr_db_open (x->path, &error);
      if (x->db == NULL)
        {
          char *msg = g_strdup (error->message);
          g_error_free (error);
          x->db = jr_db_open (":memory:", NULL); /* keep going */
          return msg;
        }
      return g_strdup ("ok");
    }
  g_printerr ("bad step \"%s\"\n", line);
  g_assert_not_reached ();
}

#define END { NULL, NULL }

/* Constraints: days are "YYYY-MM-DD", months "YYYY-MM"; keys and values
 * are UTF-8 text; a body is any bytes (or NULL with length 0). */
static void
test_operations (void)
{
  static const struct {
    const char *name;
    Step steps[MAX_STEPS];
  } cases[] = {
    /* Settings. */
    { "a missing setting", { { "get lock_kind", "null" }, END } },
    { "set then get", { { "set a 1", "true" }, { "get a", "1" }, END } },
    { "set again replaces", { { "set a 1", "true" }, { "set a 2", "true" }, { "get a", "2" }, END } },
    { "unset", { { "set a 1", "true" }, { "unset a", "true" }, { "get a", "null" }, END } },
    { "unset a missing one", { { "unset a", "true" }, END } },
    { "empty value", { { "set a ''", "true" }, { "get a", "''" }, END } },
    { "empty key", { { "set '' x", "true" }, { "get ''", "x" }, END } },
    { "SQL in a key and value is just text",
      { { "set a');DROP_TABLE_settings;-- '1'or'1'='1'", "true" },
        { "get a');DROP_TABLE_settings;--", "'1'or'1'='1'" },
        { "set b 2", "true" }, { "get b", "2" }, END } },
    { "keys are exact", { { "set Key 1", "true" }, { "get key", "null" }, { "get Key", "1" }, END } },
    { "unicode", { { "set \xd0\xba\xd0\xbb\xd1\x8e\xd1\x87 \xe6\x97\xa5\xe6\x9c\xac", "true" },
                   { "get \xd0\xba\xd0\xbb\xd1\x8e\xd1\x87", "\xe6\x97\xa5\xe6\x9c\xac" }, END } },
    /* Entries. */
    { "no entries", { { "entries 10-06", "" }, END } },
    { "oldest stamp first, not the order added",
      { { "add 10-06 2000 evening", "id" }, { "add 10-06 1000 morning", "id" },
        { "add 10-05 500 other", "id" }, { "entries 10-06", "1000:morning 2000:evening" },
        { "entries 10-05", "500:other" }, END } },
    { "same stamp: first added first",
      { { "add 10-06 1000 a", "id" }, { "add 10-06 1000 b", "id" }, { "entries 10-06", "1000:a 1000:b" }, END } },
    { "update keeps the stamp and day",
      { { "add 10-06 1000 morning", "id" }, { "update #1 morning!", "true" },
        { "entries 10-06", "1000:morning!" }, END } },
    { "update a missing entry", { { "update #9 x", "false" }, END } },
    { "update a deleted entry",
      { { "add 10-06 1 a", "id" }, { "delete #1", "true" }, { "update #1 b", "false" },
        { "entries 10-06", "" }, END } },
    { "delete one of two",
      { { "add 10-06 1 a", "id" }, { "add 10-06 2 b", "id" }, { "delete #1", "true" },
        { "entries 10-06", "2:b" }, END } },
    { "delete a missing entry: already gone", { { "delete #9", "true" }, END } },
    { "a body with zero bytes", { { "add 10-06 1 a\\0b\\0c", "id" }, { "entries 10-06", "1:a\\0b\\0c" }, END } },
    { "a body that is all zero bytes", { { "add 10-06 1 \\0\\0", "id" }, { "entries 10-06", "1:\\0\\0" }, END } },
    { "an empty body", { { "add 10-06 1 ''", "id" }, { "entries 10-06", "1:''" }, END } },
    { "a NULL body is stored empty", { { "add 10-06 1 NULL", "id" }, { "entries 10-06", "1:''" }, END } },
    { "updated to a NULL body", { { "add 10-06 1 a", "id" }, { "update #1 NULL", "true" },
                                  { "entries 10-06", "1:''" }, END } },
    { "days are matched exactly",
      { { "add 10-06 1 a", "id" }, { "entries 2026-10-6", "" }, { "entries 2026-10-06", "1:a" }, END } },
    { "stamps before 1970 and far ahead",
      { { "add 10-06 -86400 old", "id" }, { "add 10-06 253402300799 far", "id" },
        { "entries 10-06", "-86400:old 253402300799:far" }, END } },
    /* Writing time. */
    { "no time", { { "time 10-06", "0" }, END } },
    { "time adds up", { { "time+ 10-06 15", "true" }, { "time+ 10-06 15", "true" }, { "time 10-06", "30" }, END } },
    { "time is per day", { { "time+ 10-06 15", "true" }, { "time 10-07", "0" }, END } },
    { "zero or negative time is ignored",
      { { "time+ 10-06 15", "true" }, { "time+ 10-06 0", "true" }, { "time+ 10-06 -5", "true" },
        { "time 10-06", "15" }, { "time+ 10-07 0", "true" }, { "days 10-07 10-07", "" }, END } },
    /* Days. */
    { "a month with entries and time",
      { { "add 10-01 1 x", "id" }, { "add 10-01 2 y", "id" }, { "time+ 10-01 720", "true" },
        { "time+ 10-03 60", "true" }, { "add 10-04 3 z", "id" }, { "add 09-30 4 w", "id" },
        { "days 10-01 10-31", "2026-10-01:2:720 2026-10-03:0:60 2026-10-04:1:0" }, END } },
    { "both ends are included",
      { { "add 10-01 1 a", "id" }, { "add 10-31 1 b", "id" },
        { "days 10-01 10-31", "2026-10-01:1:0 2026-10-31:1:0" }, END } },
    { "a one-day range", { { "add 10-06 1 a", "id" }, { "add 10-07 1 b", "id" },
                           { "days 10-06 10-06", "2026-10-06:1:0" }, END } },
    { "from after to: nothing", { { "add 10-06 1 a", "id" }, { "days 10-31 10-01", "" }, END } },
    { "across a year end",
      { { "add 2027-01-01 1 b", "id" }, { "add 12-31 1 a", "id" },
        { "days 12-01 2027-01-31", "2026-12-31:1:0 2027-01-01:1:0" }, END } },
    /* Months. */
    { "months before this one, newest first",
      { { "add 08-01 1 a", "id" }, { "add 09-01 1 a", "id" }, { "add 09-01 2 b", "id" },
        { "add 09-15 1 c", "id" }, { "add 10-02 1 d", "id" }, { "months 10", "2026-09:2 2026-08:1" }, END } },
    { "this month is left out", { { "add 10-01 1 a", "id" }, { "months 10", "" }, END } },
    { "across a year end",
      { { "add 2025-12-31 1 a", "id" }, { "add 01-01 1 b", "id" },
        { "months 02", "2026-01:1 2025-12:1" }, END } },
    { "time alone does not make a month written",
      { { "time+ 09-01 60", "true" }, { "months 10", "" }, END } },
    /* Transactions. */
    { "rollback undoes", { { "begin", "true" }, { "set k v", "true" }, { "rollback", "ok" },
                           { "get k", "null" }, END } },
    { "commit keeps", { { "begin", "true" }, { "set k v", "true" }, { "commit", "true" }, { "get k", "v" }, END } },
    { "rollback undoes entries and time",
      { { "begin", "true" }, { "add 10-06 1 a", "id" }, { "time+ 10-06 15", "true" }, { "rollback", "ok" },
        { "entries 10-06", "" }, { "time 10-06", "0" }, END } },
    { "begin twice: the first one goes on",
      { { "begin", "true" }, { "begin", "false" }, { "set k v", "true" }, { "commit", "true" },
        { "get k", "v" }, END } },
    { "commit with nothing begun", { { "commit", "false" }, END } },
    { "rollback with nothing begun", { { "rollback", "ok" }, { "set k v", "true" }, { "get k", "v" }, END } },
    /* Restarts. */
    { "a restart keeps what was saved",
      { { "set k v", "true" }, { "add 10-06 1 x", "id" }, { "time+ 10-06 30", "true" }, { "reopen", "ok" },
        { "get k", "v" }, { "entries 10-06", "1:x" }, { "time 10-06", "30" }, END } },
    { "a restart drops what was not committed",
      { { "begin", "true" }, { "set k v", "true" }, { "reopen", "ok" }, { "get k", "null" }, END } },
    { "a new entry after a restart gets a new id",
      { { "add 10-06 1 a", "id" }, { "add 10-06 2 b", "id" }, { "delete #2", "true" }, { "reopen", "ok" },
        { "add 10-06 3 c", "id" }, { "entries 10-06", "1:a 3:c" }, END } },
  };
  JrCases c = { .table = "operations" };
  g_autofree char *dir = g_dir_make_tmp ("journal-db-XXXXXX", NULL);
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      Ctx x = { 0 };
      x.path = g_build_filename (dir, "journal.db", NULL);
      x.db = open_or_fail (x.path);
      int step = 0;
      g_autofree char *got = NULL;
      for (; step < MAX_STEPS && cases[i].steps[step].op != NULL; step++)
        {
          g_clear_pointer (&last_warning, g_free);
          g_free (got);
          got = run_op (&x, cases[i].steps[step].op);
          if (last_warning != NULL)
            {
              char *with = g_strdup_printf ("%s (%s)", got, last_warning);
              g_free (got);
              got = with;
            }
          if (strcmp (got, cases[i].steps[step].expected) != 0)
            break;
        }
      gboolean ok = step == MAX_STEPS || cases[i].steps[step].op == NULL;
      jr_case (&c, cases[i].name, ok, "Step %d \"%s\"  Expected \"%s\"  Got \"%s\"", step + 1,
               ok ? "" : cases[i].steps[step].op, ok ? "" : cases[i].steps[step].expected, got);
      jr_db_close (x.db);
      remove_tree (x.path);
      g_free (x.path);
    }
  remove_tree (dir);
  jr_cases_done (&c);
}

/* ---- Opening files ---------------------------------------------------- */

typedef enum {
  NEW_FOLDER,      /* sub/journal.db, neither exists */
  PRIVATE_FILE,    /* a journal from an earlier run, 0600 */
  SHARED_FILE,     /* a journal others can read, 0644 */
  EMPTY_FILE,      /* 0 bytes */
  TEXT_FILE,       /* not a database */
  FOLDER_IN_WAY,   /* a folder where the file should be */
  PARENT_IS_FILE,  /* file/journal.db */
  READ_ONLY,       /* 0400 */
  NEWER_VERSION,   /* user_version 2 */
  OLDER_VERSION,   /* a database with no tables yet */
  NO_FOLDER_ACCESS /* /proc/no-such-dir/journal.db */
} Setup;

static void
make_sqlite (const char *path, const char *sql)
{
  sqlite3 *h;
  g_assert_cmpint (sqlite3_open (path, &h), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (h, sql, NULL, NULL, NULL), ==, SQLITE_OK);
  sqlite3_close (h);
}

static int
user_version (const char *path)
{
  sqlite3 *h;
  sqlite3_stmt *st;
  int v = -1;
  if (sqlite3_open_v2 (path, &h, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
      sqlite3_prepare_v2 (h, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK)
    {
      if (sqlite3_step (st) == SQLITE_ROW)
        v = sqlite3_column_int (st, 0);
      sqlite3_finalize (st);
    }
  sqlite3_close (h);
  return v;
}

/* Constraints: any path. A journal opens, or the error says why and the
 * file is left exactly as it was. The journal file ends up readable by
 * its owner only, its new folder too. */
static void
test_open (void)
{
  static const struct {
    const char *name;
    Setup setup;
    gboolean opens;
    const char *error_has; /* part of the error message */
  } cases[] = {
    { "a new file in a new folder", NEW_FOLDER, TRUE, NULL },
    { "a journal from an earlier run", PRIVATE_FILE, TRUE, NULL },
    { "a journal others could read is made private", SHARED_FILE, TRUE, NULL },
    { "an empty file", EMPTY_FILE, TRUE, NULL },
    { "a database with no tables yet", OLDER_VERSION, TRUE, NULL },
    { "not a database", TEXT_FILE, FALSE, "not a database" },
    { "a folder in the way", FOLDER_IN_WAY, FALSE, "Cannot open" },
    { "the folder is a file", PARENT_IS_FILE, FALSE, "Cannot create" },
    { "a read-only file", READ_ONLY, FALSE, "Cannot open" },
    { "made by a newer version", NEWER_VERSION, FALSE, "newer version" },
    { "a folder that cannot be made", NO_FOLDER_ACCESS, FALSE, "Cannot create" },
  };
  JrCases c = { .table = "open" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      if (cases[i].setup == READ_ONLY && geteuid () == 0)
        continue; /* root ignores file modes */
      g_autofree char *dir = g_dir_make_tmp ("journal-open-XXXXXX", NULL);
      g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
      g_autofree char *before = NULL; /* file contents before, if kept */
      gsize before_len = 0;
      switch (cases[i].setup)
        {
        case NEW_FOLDER:
          g_free (path);
          path = g_build_filename (dir, "sub", "journal.db", NULL);
          break;
        case PRIVATE_FILE:
        case SHARED_FILE:
          {
            JrDb *db = open_or_fail (path);
            jr_db_set_setting (db, "kept", "yes");
            jr_db_close (db);
            g_chmod (path, cases[i].setup == SHARED_FILE ? 0644 : 0600);
            break;
          }
        case EMPTY_FILE:
          g_assert_true (g_file_set_contents (path, "", 0, NULL));
          break;
        case TEXT_FILE:
          g_assert_true (g_file_set_contents (path, "dear diary, this is not SQLite at all\n", -1, NULL));
          break;
        case FOLDER_IN_WAY:
          g_mkdir (path, 0700);
          break;
        case PARENT_IS_FILE:
          {
            g_autofree char *file = g_build_filename (dir, "file", NULL);
            g_assert_true (g_file_set_contents (file, "x", -1, NULL));
            g_free (path);
            path = g_build_filename (file, "journal.db", NULL);
            break;
          }
        case READ_ONLY:
          make_sqlite (path, "CREATE TABLE t(x)");
          g_chmod (path, 0400);
          break;
        case NEWER_VERSION:
          make_sqlite (path, "CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                             "CREATE TABLE future(x); PRAGMA user_version = 2;");
          break;
        case OLDER_VERSION:
          make_sqlite (path, "CREATE TABLE unrelated(x)");
          break;
        case NO_FOLDER_ACCESS:
          g_free (path);
          path = g_strdup ("/proc/no-such-dir/journal.db");
          break;
        }
      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        g_file_get_contents (path, &before, &before_len, NULL);

      GError *error = NULL;
      JrDb *db = jr_db_open (path, &error);
      GString *got = g_string_new (db ? "opens" : "refused");
      gboolean ok = (db != NULL) == cases[i].opens;
      if (db == NULL)
        {
          g_string_append_printf (got, " (%s)", error ? error->message : "no error message");
          ok = ok && error != NULL && strstr (error->message, cases[i].error_has) != NULL;
          /* Refused: the file is exactly as it was. */
          g_autofree char *after = NULL;
          gsize after_len = 0;
          if (before != NULL)
            {
              gboolean same = g_file_get_contents (path, &after, &after_len, NULL) &&
                              after_len == before_len && memcmp (after, before, before_len) == 0;
              if (!same)
                g_string_append (got, ", file changed");
              ok = ok && same;
            }
        }
      else
        {
          /* Opened: usable, earlier data kept, private. */
          gint64 id = jr_db_insert_entry (db, "2026-10-06", 1, "x", 1, 1);
          g_autofree char *kept = jr_db_get_setting (db, "kept");
          if (id <= 0)
            g_string_append (got, ", cannot write");
          gboolean had_data = cases[i].setup == PRIVATE_FILE || cases[i].setup == SHARED_FILE;
          if (had_data && g_strcmp0 (kept, "yes") != 0)
            g_string_append (got, ", earlier data lost");
          jr_db_close (db);
          struct stat st;
          if (g_stat (path, &st) != 0 || (st.st_mode & 0777) != 0600)
            g_string_append_printf (got, ", file mode %o", st.st_mode & 0777);
          g_autofree char *folder = g_path_get_dirname (path);
          if (cases[i].setup == NEW_FOLDER && (g_stat (folder, &st) != 0 || (st.st_mode & 0777) != 0700))
            g_string_append_printf (got, ", folder mode %o", st.st_mode & 0777);
          ok = ok && id > 0 && (!had_data || g_strcmp0 (kept, "yes") == 0) && strchr (got->str, ',') == NULL;
        }
      if (cases[i].setup == NEWER_VERSION && user_version (path) != 2)
        {
          g_string_append (got, ", version marker changed");
          ok = FALSE;
        }
      jr_case (&c, cases[i].name, ok, "Expected %s%s%s  Got %s", cases[i].opens ? "opens" : "refused",
               cases[i].error_has ? " with " : "", cases[i].error_has ? cases[i].error_has : "", got->str);
      g_string_free (got, TRUE);
      g_clear_error (&error);
      g_chmod (path, 0600);
      remove_tree (dir);
    }
  jr_cases_done (&c);
}

/* ---- Callbacks that query again ------------------------------------- */

typedef struct {
  JrDb *db;
  int outer, inner;
  gboolean nested;
  GString *seen;
} Again;

static void
count_entry (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  (void) id; (void) stamped_at; (void) body; (void) len;
  ((Again *) user)->inner++;
}

static void
entry_again (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  (void) body; (void) len;
  Again *a = user;
  a->outer++;
  g_string_append_printf (a->seen, "%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT " ", id, stamped_at);
  if (a->outer <= 10) /* a query that restarts would otherwise loop forever */
    a->nested = jr_db_foreach_entry (a->db, "2026-10-06", count_entry, a) && a->nested;
}

static void
count_day (const char *day, int entries, gint64 seconds, gpointer user)
{
  (void) day; (void) entries; (void) seconds;
  ((Again *) user)->inner++;
}

static void
day_again (const char *day, int entries, gint64 seconds, gpointer user)
{
  (void) entries; (void) seconds;
  Again *a = user;
  a->outer++;
  g_string_append_printf (a->seen, "%s ", day);
  if (a->outer <= 10)
    a->nested = jr_db_foreach_day (a->db, "2026-10-01", "2026-10-31", count_day, a) && a->nested;
}

static void
count_month (const char *month, int days, gpointer user)
{
  (void) month; (void) days;
  ((Again *) user)->inner++;
}

static void
month_again (const char *month, int days, gpointer user)
{
  (void) days;
  Again *a = user;
  a->outer++;
  g_string_append_printf (a->seen, "%s ", month);
  if (a->outer <= 10)
    a->nested = jr_db_foreach_month (a->db, "2026-11", count_month, a) && a->nested;
}

static void
entry_updates_itself (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  (void) stamped_at; (void) body; (void) len;
  Again *a = user;
  a->outer++;
  a->nested = jr_db_update_entry (a->db, id, "edited", 6, 9) && a->nested;
}

/* Constraints: a callback may call any db function, the same query
 * included; every row is still seen once. */
static void
test_callbacks (void)
{
  JrCases c = { .table = "callbacks" };
  JrDb *db = open_or_fail (":memory:");
  jr_db_insert_entry (db, "2026-10-06", 2, "b", 1, 1);
  jr_db_insert_entry (db, "2026-10-06", 1, "a", 1, 1);
  jr_db_insert_entry (db, "2026-09-01", 1, "c", 1, 1);
  jr_db_add_active_seconds (db, "2026-10-07", 60);

  Again a = { db, 0, 0, TRUE, g_string_new (NULL) };
  gboolean ok = jr_db_foreach_entry (db, "2026-10-06", entry_again, &a);
  jr_case (&c, "entries: the callback lists the same day again",
           ok && a.nested && a.outer == 2 && a.inner == 4 && strcmp (a.seen->str, "2:1 1:2 ") == 0,
           "Expected outer 2 rows (2:1 1:2), inner 4  Got outer %d (%s), inner %d%s", a.outer, a.seen->str,
           a.inner, ok && a.nested ? "" : ", a call failed");

  a = (Again){ db, 0, 0, TRUE, g_string_truncate (a.seen, 0) };
  ok = jr_db_foreach_day (db, "2026-10-01", "2026-10-31", day_again, &a);
  jr_case (&c, "days: the callback asks for the same days again",
           ok && a.nested && a.outer == 2 && a.inner == 4 &&
             strcmp (a.seen->str, "2026-10-06 2026-10-07 ") == 0,
           "Expected outer 2 days, inner 4  Got outer %d (%s), inner %d%s", a.outer, a.seen->str, a.inner,
           ok && a.nested ? "" : ", a call failed");

  a = (Again){ db, 0, 0, TRUE, g_string_truncate (a.seen, 0) };
  ok = jr_db_foreach_month (db, "2026-11", month_again, &a);
  jr_case (&c, "months: the callback asks for the months again",
           ok && a.nested && a.outer == 2 && a.inner == 4 && strcmp (a.seen->str, "2026-10 2026-09 ") == 0,
           "Expected outer 2 months, inner 4  Got outer %d (%s), inner %d%s", a.outer, a.seen->str, a.inner,
           ok && a.nested ? "" : ", a call failed");

  a = (Again){ db, 0, 0, TRUE, g_string_truncate (a.seen, 0) };
  ok = jr_db_foreach_entry (db, "2026-10-06", entry_updates_itself, &a);
  GString *after = g_string_new (NULL);
  jr_db_foreach_entry (db, "2026-10-06", list_entry, after);
  jr_case (&c, "entries: the callback edits each entry it is given",
           ok && a.nested && a.outer == 2 && strcmp (after->str, "1:edited 2:edited") == 0,
           "Expected 2 rows, then \"1:edited 2:edited\"  Got %d rows, then \"%s\"", a.outer, after->str);
  g_string_free (after, TRUE);
  g_string_free (a.seen, TRUE);
  jr_db_close (db);
  jr_cases_done (&c);
}

/* ---- Hidden tests --------------------------------------------------- */

static const char *const DAYS[] = {
  "2025-12-31", "2026-01-01", "2026-02-28", "2026-03-01", "2026-09-30",
  "2026-10-01", "2026-10-06", "2026-10-31", "2026-11-01", "2027-01-01",
};
static const char *const BOUNDS[] = {
  "0000-00-00", "2025-12-31", "2026-01-01", "2026-03-01", "2026-10-01",
  "2026-10-06", "2026-10-31", "2027-01-01", "9999-12-31",
};
static const char *const MONTHS[] = {
  "0000-00", "2025-12", "2026-01", "2026-02", "2026-03", "2026-10", "2026-11", "2027-01", "2027-02", "9999-12",
};
static const char *const KEYS[] = { "a", "b", "lock_kind", "key_pin", "" };

typedef struct {
  gint64 id;
  const char *day; /* one of DAYS */
  gint64 stamp;
  GBytes *body;
} MEntry;

/* The reference: plain lists, searched from start to end every time. */
typedef struct {
  GPtrArray *entries;   /* MEntry */
  gint64 seconds[G_N_ELEMENTS (DAYS)];
  gboolean has_time[G_N_ELEMENTS (DAYS)];
  char *settings[G_N_ELEMENTS (KEYS)];
} Model;

static void
mentry_free (gpointer p)
{
  MEntry *e = p;
  g_bytes_unref (e->body);
  g_free (e);
}

static Model *
model_new (void)
{
  Model *m = g_new0 (Model, 1);
  m->entries = g_ptr_array_new_with_free_func (mentry_free);
  return m;
}

static Model *
model_copy (const Model *m)
{
  Model *c = model_new ();
  for (guint i = 0; i < m->entries->len; i++)
    {
      MEntry *e = g_memdup2 (g_ptr_array_index (m->entries, i), sizeof (MEntry));
      g_bytes_ref (e->body);
      g_ptr_array_add (c->entries, e);
    }
  memcpy (c->seconds, m->seconds, sizeof c->seconds);
  memcpy (c->has_time, m->has_time, sizeof c->has_time);
  for (gsize k = 0; k < G_N_ELEMENTS (KEYS); k++)
    c->settings[k] = g_strdup (m->settings[k]);
  return c;
}

static void
model_free (Model *m)
{
  if (m == NULL)
    return;
  g_ptr_array_unref (m->entries);
  for (gsize k = 0; k < G_N_ELEMENTS (KEYS); k++)
    g_free (m->settings[k]);
  g_free (m);
}

static MEntry *
model_find (const Model *m, gint64 id)
{
  for (guint i = 0; i < m->entries->len; i++)
    if (((MEntry *) g_ptr_array_index (m->entries, i))->id == id)
      return g_ptr_array_index (m->entries, i);
  return NULL;
}

/* A day's entries the slow way: pick the smallest (stamp, id) left,
 * again and again. */
static char *
model_entries (const Model *m, const char *day)
{
  GString *s = g_string_new (NULL);
  gint64 last_stamp = G_MININT64, last_id = G_MININT64;
  for (;;)
    {
      MEntry *best = NULL;
      for (guint i = 0; i < m->entries->len; i++)
        {
          MEntry *e = g_ptr_array_index (m->entries, i);
          gboolean after = e->stamp > last_stamp || (e->stamp == last_stamp && e->id > last_id);
          if (strcmp (e->day, day) == 0 && after &&
              (best == NULL || e->stamp < best->stamp || (e->stamp == best->stamp && e->id < best->id)))
            best = e;
        }
      if (best == NULL)
        break;
      sep (s);
      gsize len;
      const void *data = g_bytes_get_data (best->body, &len);
      g_string_append_printf (s, "%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":", best->id, best->stamp);
      show_bytes (s, data, len);
      last_stamp = best->stamp;
      last_id = best->id;
    }
  return g_string_free (s, FALSE);
}

static void
list_entry_with_id (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  GString *s = user;
  sep (s);
  g_string_append_printf (s, "%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":", id, stamped_at);
  show_bytes (s, body, len);
}

/* DAYS is sorted, so walking it gives days in order. */
static char *
model_days (const Model *m, const char *from, const char *to)
{
  GString *s = g_string_new (NULL);
  for (gsize d = 0; d < G_N_ELEMENTS (DAYS); d++)
    {
      if (strcmp (DAYS[d], from) < 0 || strcmp (DAYS[d], to) > 0)
        continue;
      int n = 0;
      for (guint i = 0; i < m->entries->len; i++)
        n += strcmp (((MEntry *) g_ptr_array_index (m->entries, i))->day, DAYS[d]) == 0;
      if (n > 0 || m->has_time[d])
        {
          sep (s);
          g_string_append_printf (s, "%s:%d:%" G_GINT64_FORMAT, DAYS[d], n, m->seconds[d]);
        }
    }
  return g_string_free (s, FALSE);
}

static char *
model_months (const Model *m, const char *before)
{
  GString *s = g_string_new (NULL);
  for (gsize d = G_N_ELEMENTS (DAYS); d-- > 0;) /* newest first */
    {
      char month[8];
      g_strlcpy (month, DAYS[d], sizeof month);
      if (strcmp (month, before) >= 0 || (d + 1 < G_N_ELEMENTS (DAYS) && strncmp (DAYS[d + 1], month, 7) == 0))
        continue; /* not before, or this month was already listed */
      int days = 0;
      for (gsize e = 0; e < G_N_ELEMENTS (DAYS); e++)
        {
          gboolean written = FALSE;
          for (guint i = 0; i < m->entries->len && !written; i++)
            written = strcmp (((MEntry *) g_ptr_array_index (m->entries, i))->day, DAYS[e]) == 0;
          days += written && strncmp (DAYS[e], month, 7) == 0;
        }
      if (days > 0)
        {
          sep (s);
          g_string_append_printf (s, "%s:%d", month, days);
        }
    }
  return g_string_free (s, FALSE);
}

/* Everything the db answers matches the reference. */
static gboolean
same_as_model (JrDb *db, const Model *m, GString *why)
{
  for (gsize d = 0; d < G_N_ELEMENTS (DAYS); d++)
    {
      g_autofree char *want = model_entries (m, DAYS[d]);
      GString *got = g_string_new (NULL);
      jr_db_foreach_entry (db, DAYS[d], list_entry_with_id, got);
      gboolean same = strcmp (want, got->str) == 0;
      if (!same)
        g_string_printf (why, "entries %s: expected \"%s\", got \"%s\"", DAYS[d], want, got->str);
      g_string_free (got, TRUE);
      if (!same)
        return FALSE;
      gint64 t = jr_db_active_seconds (db, DAYS[d]);
      if (t != m->seconds[d])
        {
          g_string_printf (why, "time %s: expected %" G_GINT64_FORMAT ", got %" G_GINT64_FORMAT, DAYS[d],
                           m->seconds[d], t);
          return FALSE;
        }
    }
  for (gsize k = 0; k < G_N_ELEMENTS (KEYS); k++)
    {
      g_autofree char *v = jr_db_get_setting (db, KEYS[k]);
      if (g_strcmp0 (v, m->settings[k]) != 0)
        {
          g_string_printf (why, "get \"%s\": expected %s, got %s", KEYS[k],
                           m->settings[k] ? m->settings[k] : "null", v ? v : "null");
          return FALSE;
        }
    }
  const char *from = BOUNDS[g_test_rand_int_range (0, G_N_ELEMENTS (BOUNDS))];
  const char *to = BOUNDS[g_test_rand_int_range (0, G_N_ELEMENTS (BOUNDS))];
  g_autofree char *want_days = model_days (m, from, to);
  GString *got = g_string_new (NULL);
  jr_db_foreach_day (db, from, to, list_day, got);
  gboolean same = strcmp (want_days, got->str) == 0;
  if (!same)
    g_string_printf (why, "days %s %s: expected \"%s\", got \"%s\"", from, to, want_days, got->str);
  const char *before = MONTHS[g_test_rand_int_range (0, G_N_ELEMENTS (MONTHS))];
  g_autofree char *want_months = model_months (m, before);
  g_string_truncate (got, 0);
  jr_db_foreach_month (db, before, list_month, got);
  if (same && strcmp (want_months, got->str) != 0)
    {
      g_string_printf (why, "months %s: expected \"%s\", got \"%s\"", before, want_months, got->str);
      same = FALSE;
    }
  g_string_free (got, TRUE);
  return same;
}

#define BODY_MAX 12

/* Fills `out` with 0 to BODY_MAX - 1 random bytes; returns how many. */
static int
random_body (guint8 out[BODY_MAX])
{
  int len = g_test_rand_int_range (0, BODY_MAX);
  for (int i = 0; i < len && i < BODY_MAX; i++)
    out[i] = (guint8) g_test_rand_int_range (0, 256);
  return len;
}

/* One random script of `steps` operations; with `path`, restarts too.
 * Returns FALSE (and the script so far in `log`) on the first mismatch. */
static gboolean
run_random_script (const char *path, int steps, GString *log, GString *why)
{
  JrDb *db = open_or_fail (path ? path : ":memory:");
  Model *m = model_new (), *saved = NULL; /* saved: the model at BEGIN */
  gint64 dead_id = 999999;
  gboolean ok = TRUE;
  for (int s = 0; s < steps && ok; s++)
    {
      int r = g_test_rand_int_range (0, path ? 100 : 96);
      gboolean want = TRUE, got = TRUE;
      if (r < 30)
        {
          const char *day = DAYS[g_test_rand_int_range (0, G_N_ELEMENTS (DAYS))];
          gint64 stamp = g_test_rand_int_range (0, 6);
          guint8 bytes[BODY_MAX];
          int len = random_body (bytes);
          gboolean null_body = len == 0 && g_test_rand_bit ();
          gint64 id = jr_db_insert_entry (db, day, stamp, null_body ? NULL : bytes, len, s);
          g_string_append_printf (log, "add %s %" G_GINT64_FORMAT " <%d bytes> = %" G_GINT64_FORMAT "; ", day,
                                  stamp, len, id);
          got = id > 0 && model_find (m, id) == NULL;
          MEntry *e = g_new0 (MEntry, 1);
          *e = (MEntry){ id, day, stamp, g_bytes_new (bytes, len) };
          g_ptr_array_add (m->entries, e);
        }
      else if (r < 42)
        {
          MEntry *e = m->entries->len > 0 && g_test_rand_int_range (0, 4) > 0
                        ? g_ptr_array_index (m->entries, g_test_rand_int_range (0, m->entries->len))
                        : NULL;
          gint64 id = e ? e->id : dead_id;
          e = model_find (m, id); /* a deleted id may belong to a new entry now */
          guint8 bytes[BODY_MAX];
          int len = random_body (bytes);
          got = jr_db_update_entry (db, id, bytes, len, s);
          want = e != NULL;
          if (e != NULL)
            {
              g_bytes_unref (e->body);
              e->body = g_bytes_new (bytes, len);
            }
          g_string_append_printf (log, "update %" G_GINT64_FORMAT " <%d bytes>; ", id, len);
        }
      else if (r < 52)
        {
          gint64 id = dead_id;
          if (m->entries->len > 0 && g_test_rand_int_range (0, 4) > 0)
            id = ((MEntry *) g_ptr_array_index (m->entries, g_test_rand_int_range (0, m->entries->len)))->id;
          MEntry *e = model_find (m, id); /* a deleted id may belong to a new entry now */
          if (e != NULL)
            g_ptr_array_remove (m->entries, e);
          dead_id = id;
          got = jr_db_delete_entry (db, id);
          g_string_append_printf (log, "delete %" G_GINT64_FORMAT "; ", id);
        }
      else if (r < 64)
        {
          int d = g_test_rand_int_range (0, G_N_ELEMENTS (DAYS));
          gint64 secs = g_test_rand_int_range (-2, 900);
          got = jr_db_add_active_seconds (db, DAYS[d], secs);
          if (secs > 0)
            {
              m->seconds[d] += secs;
              m->has_time[d] = TRUE;
            }
          g_string_append_printf (log, "time+ %s %" G_GINT64_FORMAT "; ", DAYS[d], secs);
        }
      else if (r < 76)
        {
          int k = g_test_rand_int_range (0, G_N_ELEMENTS (KEYS));
          g_autofree char *v = g_strdup_printf ("v%d", g_test_rand_int_range (0, 5));
          got = jr_db_set_setting (db, KEYS[k], v);
          g_free (m->settings[k]);
          m->settings[k] = g_strdup (v);
          g_string_append_printf (log, "set %s %s; ", KEYS[k], v);
        }
      else if (r < 82)
        {
          int k = g_test_rand_int_range (0, G_N_ELEMENTS (KEYS));
          got = jr_db_delete_setting (db, KEYS[k]);
          g_clear_pointer (&m->settings[k], g_free);
          g_string_append_printf (log, "unset %s; ", KEYS[k]);
        }
      else if (r < 88)
        {
          got = jr_db_begin (db);
          want = saved == NULL;
          if (saved == NULL)
            saved = model_copy (m);
          g_string_append (log, "begin; ");
        }
      else if (r < 92)
        {
          got = jr_db_commit (db);
          want = saved != NULL;
          g_clear_pointer (&saved, model_free);
          g_string_append (log, "commit; ");
        }
      else if (r < 96)
        {
          jr_db_rollback (db);
          if (saved != NULL)
            {
              model_free (m);
              m = saved;
              saved = NULL;
            }
          g_string_append (log, "rollback; ");
        }
      else /* restart: what was not committed is gone */
        {
          jr_db_close (db);
          db = open_or_fail (path);
          if (saved != NULL)
            {
              model_free (m);
              m = saved;
              saved = NULL;
            }
          g_string_append (log, "reopen; ");
        }
      if (got != want)
        {
          g_string_printf (why, "the last step returned %s", got ? "true" : "false");
          ok = FALSE;
        }
      ok = ok && same_as_model (db, m, why);
    }
  jr_db_close (db);
  model_free (m);
  model_free (saved);
  return ok;
}

/* Hidden test: random scripts of every operation (in memory, and on a
 * file with restarts), each checked after every step against the
 * reference: every day's entries in order, every day's time, every
 * setting, a random range of days and the months before a random one. */
static void
test_hidden_scripts (void)
{
  JrCases c = { .table = "hidden: random scripts" };
  g_autofree char *dir = g_dir_make_tmp ("journal-db-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  int in_memory = slow_run () ? 30 : 200, on_file = slow_run () ? 5 : 30;
  for (int n = 0; n < in_memory + on_file; n++)
    {
      gboolean file = n >= in_memory;
      GString *log = g_string_new (NULL), *why = g_string_new (NULL);
      gboolean ok = run_random_script (file ? path : NULL, file ? 40 : 60, log, why);
      jr_case (&c, file ? "on a file, with restarts" : "in memory", ok, "Input \"%s\"  %s", log->str,
               why->str);
      g_string_free (log, TRUE);
      g_string_free (why, TRUE);
      remove_tree (path);
    }
  remove_tree (dir);
  jr_cases_done (&c);
}

/* ---- Huge inputs, time and memory limits ---------------------------- */

static void
huge_body_entry (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  (void) id; (void) stamped_at;
  GBytes **out = user;
  *out = g_bytes_new (body, len);
}

/* Constraints: an entry up to 10 MB, a setting up to 1 MB. */
static void
test_huge_values (void)
{
  JrCases c = { .table = "huge: values" };
  JrDb *db = open_or_fail (":memory:");
  gsize big = 10 * 1024 * 1024;
  guint8 *body = g_malloc (big);
  for (gsize i = 0; i < big; i++)
    body[i] = (guint8) (i * 2654435761u >> 24);
  gint64 id = jr_db_insert_entry (db, "2026-10-06", 1, body, big, 1);
  GBytes *back = NULL;
  jr_db_foreach_entry (db, "2026-10-06", huge_body_entry, &back);
  gsize len = 0;
  const void *data = back ? g_bytes_get_data (back, &len) : NULL;
  jr_case (&c, "a 10 MB entry", id > 0 && len == big && memcmp (data, body, big) == 0,
           "Expected the same %" G_GSIZE_FORMAT " bytes back  Got %" G_GSIZE_FORMAT " bytes%s", big, len,
           len == big ? " that differ" : "");
  g_clear_pointer (&back, g_bytes_unref);
  g_free (body);

  char *value = g_malloc (1024 * 1024 + 1);
  memset (value, 'v', 1024 * 1024);
  value[1024 * 1024] = '\0';
  jr_db_set_setting (db, "big", value);
  g_autofree char *got = jr_db_get_setting (db, "big");
  jr_case (&c, "a 1 MB setting", g_strcmp0 (got, value) == 0, "Expected 1048576 bytes back  Got %" G_GSIZE_FORMAT,
           got ? strlen (got) : 0);
  g_free (value);
  jr_db_close (db);
  jr_cases_done (&c);
}

typedef struct {
  int rows;
  gint64 last_stamp, last_id;
  gboolean in_order;
} Scan;

static void
scan_entry (gint64 id, gint64 stamped_at, const void *body, gsize len, gpointer user)
{
  (void) body; (void) len;
  Scan *s = user;
  if (s->rows > 0 && (stamped_at < s->last_stamp || (stamped_at == s->last_stamp && id <= s->last_id)))
    s->in_order = FALSE;
  s->rows++;
  s->last_stamp = stamped_at;
  s->last_id = id;
}

static void
count_days (const char *day, int entries, gint64 seconds, gpointer user)
{
  (void) day; (void) seconds;
  int *n = user;
  n[0]++;
  n[1] += entries;
}

static void
count_months (const char *month, int days, gpointer user)
{
  (void) month;
  int *n = user;
  n[0]++;
  n[1] += days;
}

static double
ms_since (gint64 start)
{
  return (g_get_monotonic_time () - start) / 1000.0;
}

/* Huge input: ten years of writing three 2 KB entries every day (10,950
 * entries, 22 MB) in a file. Time limits (skipped under Valgrind and the
 * sanitizers) and a memory limit: SQLite's own memory stays small while
 * reading, because rows are streamed, never collected. */
static void
test_ten_years (void)
{
  JrCases c = { .table = "huge: ten years of entries" };
  int years = slow_run () ? 1 : 10, per_day = 3;
  gboolean timed = !slow_run ();
  g_autofree char *dir = g_dir_make_tmp ("journal-db-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  guint8 body[2048];
  for (gsize i = 0; i < sizeof body; i++)
    body[i] = (guint8) (i * 31 + 7);

  JrDb *db = open_or_fail (path);
  gint64 start = g_get_monotonic_time ();
  jr_db_begin (db);
  GDate *d = g_date_new_dmy (1, G_DATE_JANUARY, 2026);
  int days = 0;
  for (; g_date_get_year (d) < 2026 + years; g_date_add_days (d, 1), days++)
    {
      char day[JR_DB_DAY_LEN];
      g_snprintf (day, sizeof day, "%04d-%02d-%02d", g_date_get_year (d), g_date_get_month (d),
                  g_date_get_day (d));
      for (int e = 0; e < per_day; e++)
        jr_db_insert_entry (db, day, G_GINT64_CONSTANT (1767225600) + days * 86400 + e * 3600, body,
                            sizeof body, 1);
      jr_db_add_active_seconds (db, day, 900);
    }
  g_date_free (d);
  gboolean committed = jr_db_commit (db);
  double insert_ms = ms_since (start);
  jr_db_close (db);
  jr_case (&c, "write them all in one save", committed && (!timed || insert_ms < 5000),
           "Expected under 5000 ms  Got %.0f ms%s", insert_ms, committed ? "" : " (not saved)");

  start = g_get_monotonic_time ();
  db = open_or_fail (path);
  double open_ms = ms_since (start);
  jr_case (&c, "open the file", !timed || open_ms < 500, "Expected under 500 ms  Got %.1f ms", open_ms);

  gboolean measured = sqlite3_memory_used () > 0;
  sqlite3_memory_highwater (1); /* from here on */
  sqlite3_int64 base = sqlite3_memory_used ();

  int month[2] = { 0, 0 };
  start = g_get_monotonic_time ();
  jr_db_foreach_day (db, "2026-10-01", "2026-10-31", count_days, month);
  double month_ms = ms_since (start);
  jr_case (&c, "a month's calendar", month[0] == 31 && month[1] == 31 * per_day && (!timed || month_ms < 50),
           "Expected 31 days, %d entries, under 50 ms  Got %d days, %d entries, %.2f ms", 31 * per_day, month[0],
           month[1], month_ms);

  int months[2] = { 0, 0 };
  start = g_get_monotonic_time ();
  jr_db_foreach_month (db, "9999-12", count_months, months);
  double months_ms = ms_since (start);
  jr_case (&c, "the list of months", months[0] == 12 * years && months[1] == days && (!timed || months_ms < 300),
           "Expected %d months, %d days, under 300 ms  Got %d months, %d days, %.1f ms", 12 * years, days,
           months[0], months[1], months_ms);

  Scan scan = { 0, 0, 0, TRUE };
  start = g_get_monotonic_time ();
  jr_db_foreach_entry (db, "2026-10-06", scan_entry, &scan);
  double day_ms = ms_since (start);
  jr_case (&c, "one day's entries", scan.rows == per_day && scan.in_order && (!timed || day_ms < 20),
           "Expected %d in order, under 20 ms  Got %d%s, %.2f ms", per_day, scan.rows,
           scan.in_order ? "" : " out of order", day_ms);

  sqlite3_int64 peak = sqlite3_memory_highwater (0) - base;
  jr_case (&c, "memory while reading", !measured || peak < 2 * 1024 * 1024,
           "Expected SQLite to use under 2 MB more  Got %" G_GINT64_FORMAT " KB more", (gint64) peak / 1024);
  g_test_message ("ten years: write %.0f ms, open %.1f ms, month %.2f ms, months %.1f ms, day %.2f ms; "
                  "memory while reading +%" G_GINT64_FORMAT " KB",
                  insert_ms, open_ms, month_ms, months_ms, day_ms, (gint64) peak / 1024);
  if (!measured)
    g_test_message ("SQLite's memory counters are off in this build; memory limit not checked");
  jr_db_close (db);
  remove_tree (dir);
  jr_cases_done (&c);
}

/* Huge input: 100,000 entries on one day with clashing stamps, in order. */
static void
test_one_busy_day (void)
{
  JrCases c = { .table = "huge: one busy day" };
  int n = slow_run () ? 10000 : 100000;
  JrDb *db = open_or_fail (":memory:");
  jr_db_begin (db);
  for (int i = 0; i < n; i++)
    jr_db_insert_entry (db, "2026-10-06", g_test_rand_int_range (0, 5000), "x", 1, 1);
  jr_db_commit (db);
  Scan scan = { 0, 0, 0, TRUE };
  gint64 start = g_get_monotonic_time ();
  jr_db_foreach_entry (db, "2026-10-06", scan_entry, &scan);
  double ms = ms_since (start);
  jr_case (&c, "every entry, oldest stamp first", scan.rows == n && scan.in_order && (slow_run () || ms < 2000),
           "Expected %d in order, under 2000 ms  Got %d%s, %.0f ms", n, scan.rows,
           scan.in_order ? "" : " out of order", ms);
  jr_db_close (db);
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, note_warning, NULL);
  g_test_add_func ("/db/operations", test_operations);
  g_test_add_func ("/db/open", test_open);
  g_test_add_func ("/db/callbacks", test_callbacks);
  g_test_add_func ("/db/hidden/scripts", test_hidden_scripts);
  g_test_add_func ("/db/huge/values", test_huge_values);
  g_test_add_func ("/db/huge/ten-years", test_ten_years);
  g_test_add_func ("/db/huge/one-busy-day", test_one_busy_day);
  int rc = g_test_run ();
  g_free (last_warning);
  return rc;
}
