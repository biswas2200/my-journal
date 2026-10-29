/* Tests for the Notes store (src/notes/core/notesdb.c), LeetCode style
 * (see cases.h). Most cases are "design a class" scripts: operations
 * separated by ";", each "op args -> expected" (shell-style quoting):
 *
 *   subj NAME            -> id | 0      a subject (DSA); names are unique
 *                                       ignoring case, trimmed, not empty
 *   rensubj ID NAME      -> true|false  delsubj ID -> true|false
 *   topic SUBJ NAME      -> id | 0      unique within its subject
 *   rentopic ID NAME     -> true|false  deltopic ID -> true|false
 *   start TOPIC @T       -> id | 0      a study session starting at T
 *   save ID BODY SRC @T  -> true|false  body and sources; the end time
 *                                       follows while studying (see extend)
 *   get ID               -> started:ended:body:sources | null
 *   drop ID              -> true|false  remove it if it is still empty
 *   delsession ID        -> true|false
 *   subjects             -> a,b         by name, ignoring case
 *   topics SUBJ          -> a,b
 *   sessions TOPIC       -> id@start-end,...  newest first
 *   topicinfo TOPIC      -> subject:subject name:topic name | null
 *   enhance ID MD MODEL @T -> true|false  store the AI's enhanced copy
 *   enhanced ID          -> model@T:markdown | null
 *   set KEY VALUE        -> true         setting KEY -> value | null
 *   style TOPIC N        -> body|body    up to N recent notes from other
 *                                        topics, newest first, to show the
 *                                        AI how this person writes
 *   reopen               -> ok          close and open the file again
 *
 * Ids count from 1 in each table and are never reused. */
#include <string.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "cases.h"
#include "notesdb.h"

#define MINUTE 60

/* ---- the script runner ---------------------------------------------- */

typedef struct {
  JnDb *db;
  char *path;
} Store;

static void
collect_name (gint64 id, const char *name, gpointer user)
{
  (void) id;
  GString *s = user;
  if (s->len > 0)
    g_string_append_c (s, ',');
  g_string_append (s, name);
}

static void
collect_session (gint64 id, gint64 started, gint64 ended, gpointer user)
{
  GString *s = user;
  if (s->len > 0)
    g_string_append_c (s, ',');
  g_string_append_printf (s, "%" G_GINT64_FORMAT "@%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, id, started, ended);
}

static gint64
num (const char *s)
{
  return g_ascii_strtoll (s[0] == '@' ? s + 1 : s, NULL, 10);
}

static char *
yes (gboolean b)
{
  return g_strdup (b ? "true" : "false");
}

/* Runs one operation; returns what it gave, as the script writes it. */
static char *
run_op (Store *st, char **a, int n)
{
  const char *op = a[0];
  if (strcmp (op, "subj") == 0 && n == 2)
    return g_strdup_printf ("%" G_GINT64_FORMAT, jn_db_add_subject (st->db, a[1]));
  if (strcmp (op, "rensubj") == 0 && n == 3)
    return yes (jn_db_rename_subject (st->db, num (a[1]), a[2]));
  if (strcmp (op, "delsubj") == 0 && n == 2)
    return yes (jn_db_delete_subject (st->db, num (a[1])));
  if (strcmp (op, "topic") == 0 && n == 3)
    return g_strdup_printf ("%" G_GINT64_FORMAT, jn_db_add_topic (st->db, num (a[1]), a[2]));
  if (strcmp (op, "rentopic") == 0 && n == 3)
    return yes (jn_db_rename_topic (st->db, num (a[1]), a[2]));
  if (strcmp (op, "deltopic") == 0 && n == 2)
    return yes (jn_db_delete_topic (st->db, num (a[1])));
  if (strcmp (op, "start") == 0 && n == 3)
    return g_strdup_printf ("%" G_GINT64_FORMAT, jn_db_start_session (st->db, num (a[1]), num (a[2])));
  if (strcmp (op, "save") == 0 && n == 5)
    return yes (jn_db_save_session (st->db, num (a[1]), a[2], a[3], num (a[4])));
  if (strcmp (op, "get") == 0 && n == 2)
    {
      JnSession s = { 0 };
      if (!jn_db_get_session (st->db, num (a[1]), &s))
        return g_strdup ("null");
      char *r = g_strdup_printf ("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s:%s", s.started, s.ended, s.body,
                                 s.sources);
      jn_session_clear (&s);
      return r;
    }
  if (strcmp (op, "drop") == 0 && n == 2)
    return yes (jn_db_drop_if_empty (st->db, num (a[1])));
  if (strcmp (op, "delsession") == 0 && n == 2)
    return yes (jn_db_delete_session (st->db, num (a[1])));
  if (strcmp (op, "subjects") == 0 && n == 1)
    {
      GString *s = g_string_new (NULL);
      jn_db_foreach_subject (st->db, collect_name, s);
      return g_string_free (s, FALSE);
    }
  if (strcmp (op, "topics") == 0 && n == 2)
    {
      GString *s = g_string_new (NULL);
      jn_db_foreach_topic (st->db, num (a[1]), collect_name, s);
      return g_string_free (s, FALSE);
    }
  if (strcmp (op, "sessions") == 0 && n == 2)
    {
      GString *s = g_string_new (NULL);
      jn_db_foreach_session (st->db, num (a[1]), collect_session, s);
      return g_string_free (s, FALSE);
    }
  if (strcmp (op, "topicinfo") == 0 && n == 2)
    {
      gint64 subject = 0;
      g_autofree char *sn = NULL, *tn = NULL;
      if (!jn_db_get_topic (st->db, num (a[1]), &subject, &sn, &tn))
        return g_strdup ("null");
      return g_strdup_printf ("%" G_GINT64_FORMAT ":%s:%s", subject, sn, tn);
    }
  if (strcmp (op, "enhance") == 0 && n == 5)
    return yes (jn_db_set_enhanced (st->db, num (a[1]), a[2], a[3], num (a[4])));
  if (strcmp (op, "enhanced") == 0 && n == 2)
    {
      g_autofree char *model = NULL;
      gint64 at = 0;
      g_autofree char *md = jn_db_get_enhanced (st->db, num (a[1]), &model, &at);
      return md == NULL ? g_strdup ("null") : g_strdup_printf ("%s@%" G_GINT64_FORMAT ":%s", model, at, md);
    }
  if (strcmp (op, "set") == 0 && n == 3)
    return yes (jn_db_set_setting (st->db, a[1], a[2]));
  if (strcmp (op, "setting") == 0 && n == 2)
    {
      char *v = jn_db_get_setting (st->db, a[1]);
      return v != NULL ? v : g_strdup ("null");
    }
  if (strcmp (op, "style") == 0 && n == 3)
    {
      g_auto (GStrv) bodies = jn_db_style_samples (st->db, num (a[1]), (int) num (a[2]), 4000);
      return g_strjoinv ("|", bodies);
    }
  if (strcmp (op, "reopen") == 0 && n == 1)
    {
      jn_db_close (st->db);
      st->db = jn_db_open (st->path, NULL);
      return g_strdup (st->db != NULL ? "ok" : "failed");
    }
  return g_strdup_printf ("bad op %s/%d", op, n);
}

static char *
temp_path (char **dir)
{
  *dir = g_dir_make_tmp ("notesdb-XXXXXX", NULL);
  return g_build_filename (*dir, "sub", "notes.db", NULL);
}

static void
remove_store (Store *st, char *dir)
{
  jn_db_close (st->db);
  g_autofree char *sub = g_path_get_dirname (st->path);
  g_remove (st->path);
  g_rmdir (sub);
  g_rmdir (dir);
  g_free (st->path);
}

/* Returns the first mismatch ("op -> got, expected"), or NULL. */
static char *
run_script (const char *script)
{
  char *dir;
  Store st = { NULL, temp_path (&dir) };
  st.db = jn_db_open (st.path, NULL);
  g_assert_nonnull (st.db);
  char *bad = NULL;
  g_auto (GStrv) ops = g_strsplit (script, ";", 0);
  for (char **o = ops; *o != NULL && bad == NULL; o++)
    {
      g_auto (GStrv) parts = g_strsplit (*o, " -> ", 2);
      g_autofree char *cmd = g_strstrip (g_strdup (parts[0]));
      if (*cmd == '\0')
        continue;
      const char *want = parts[1] != NULL ? g_strstrip (parts[1]) : "";
      int argc = 0;
      char **argv = NULL;
      if (!g_shell_parse_argv (cmd, &argc, &argv, NULL))
        {
          bad = g_strdup_printf ("cannot parse \"%s\"", cmd);
          break;
        }
      g_autofree char *got = run_op (&st, argv, argc);
      if (strcmp (got, want) != 0)
        bad = g_strdup_printf ("\"%s\"  Expected %s  Got %s", cmd, want, got);
      g_strfreev (argv);
    }
  remove_store (&st, dir);
  g_free (dir);
  return bad;
}

typedef struct {
  const char *name, *script;
} ScriptCase;

static const ScriptCase scripts[] = {
  { "first subject",        "subj DSA -> 1; subjects -> DSA" },
  { "names trimmed",        "subj '  DBMS  ' -> 1; subjects -> DBMS" },
  { "empty name refused",   "subj '' -> 0; subj '   ' -> 0; subjects -> " },
  { "same name, any case",  "subj DSA -> 1; subj dsa -> 0; subj ' DSA ' -> 0; subjects -> DSA" },
  { "sorted ignoring case", "subj os -> 1; subj DBMS -> 2; subj algo -> 3; subjects -> algo,DBMS,os" },
  { "rename",               "subj DSA -> 1; rensubj 1 'Data structures' -> true; subjects -> Data structures" },
  { "rename to a taken name", "subj A -> 1; subj B -> 2; rensubj 2 a -> false; subjects -> A,B" },
  { "rename to the same, other case", "subj dsa -> 1; rensubj 1 DSA -> true; subjects -> DSA" },
  { "rename missing",       "rensubj 9 X -> false" },
  { "topics per subject",   "subj DSA -> 1; subj OS -> 2; topic 1 'Prefix sum' -> 1; topic 2 Paging -> 2;"
                            "topics 1 -> Prefix sum; topics 2 -> Paging" },
  { "same topic name in two subjects", "subj A -> 1; subj B -> 2; topic 1 Intro -> 1; topic 2 Intro -> 2" },
  { "topic name taken",     "subj A -> 1; topic 1 Intro -> 1; topic 1 INTRO -> 0" },
  { "topic of a missing subject", "topic 9 X -> 0" },
  { "rename topic",         "subj A -> 1; topic 1 X -> 1; topic 1 Y -> 2; rentopic 2 x -> false; rentopic 2 Z -> true;"
                            "topics 1 -> X,Z" },
  { "a study session",      "subj DSA -> 1; topic 1 'Prefix sum' -> 1; start 1 @1000 -> 1; get 1 -> 1000:1000::" },
  { "saving extends the end while studying",
                            "subj A -> 1; topic 1 T -> 1; start 1 @1000 -> 1;"
                            "save 1 '# Prefix sum' '' @1600 -> true; save 1 '# Prefix sum\n- pre[0] = 0' "
                            "'https://cp-algorithms.com' @2800 -> true;"
                            "get 1 -> 1000:2800:# Prefix sum\n- pre[0] = 0:https://cp-algorithms.com" },
  { "editing next week keeps the end",
                            "subj A -> 1; topic 1 T -> 1; start 1 @1000 -> 1; save 1 x '' @2000 -> true;"
                            "save 1 'x fixed' '' @700000 -> true; get 1 -> 1000:2000:x fixed:" },
  { "clock moved back",     "subj A -> 1; topic 1 T -> 1; start 1 @5000 -> 1; save 1 x '' @4000 -> true;"
                            "get 1 -> 5000:5000:x:" },
  { "sessions newest first","subj A -> 1; topic 1 T -> 1; start 1 @100 -> 1; start 1 @900 -> 2; start 1 @500 -> 3;"
                            "sessions 1 -> 2@900-900,3@500-500,1@100-100" },
  { "session of a missing topic", "start 9 @1 -> 0; save 9 x '' @2 -> false; get 9 -> null" },
  { "empty session dropped","subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; drop 1 -> true; get 1 -> null" },
  { "whitespace counts as empty", "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; save 1 ' \n ' '' @2 -> true;"
                            "drop 1 -> true" },
  { "written session kept", "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; save 1 x '' @2 -> true; drop 1 -> false;"
                            "get 1 -> 1:2:x:" },
  { "sources alone keep it","subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; save 1 '' 'https://a' @2 -> true;"
                            "drop 1 -> false" },
  { "delete a session",     "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; delsession 1 -> true; delsession 1 -> false;"
                            "sessions 1 -> " },
  { "deleting a topic deletes its sessions",
                            "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; deltopic 1 -> true; get 1 -> null;"
                            "topics 1 -> " },
  { "deleting a subject deletes everything in it",
                            "subj A -> 1; subj B -> 2; topic 1 T -> 1; topic 2 U -> 2; start 1 @1 -> 1;"
                            "start 2 @1 -> 2; delsubj 1 -> true; get 1 -> null; get 2 -> 1:1::; subjects -> B" },
  { "ids are never reused", "subj A -> 1; delsubj 1 -> true; subj A -> 2; topic 2 T -> 1; deltopic 1 -> true;"
                            "topic 2 T -> 2; start 2 @1 -> 1; delsession 1 -> true; start 2 @1 -> 2" },
  { "everything survives a restart",
                            "subj DSA -> 1; topic 1 'Prefix sum' -> 1; start 1 @1000 -> 1;"
                            "save 1 '## Idea\n  - inner' 'https://a\nhttps://b' @1200 -> true; reopen -> ok;"
                            "subjects -> DSA; topics 1 -> Prefix sum; get 1 -> 1000:1200:## Idea\n  - inner:https://a\nhttps://b" },
  { "topic names",          "subj DSA -> 1; topic 1 'Prefix sum' -> 1; topicinfo 1 -> 1:DSA:Prefix sum; topicinfo 9 -> null;"
                            "rensubj 1 Algorithms -> true; topicinfo 1 -> 1:Algorithms:Prefix sum" },
  { "an enhanced copy",     "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; enhance 1 '## Idea' gemini-3.8-flash @50 -> true;"
                            "enhanced 1 -> gemini-3.8-flash@50:## Idea; reopen -> ok; enhanced 1 -> gemini-3.8-flash@50:## Idea" },
  { "enhancing again replaces it", "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; enhance 1 old m @2 -> true;"
                            "enhance 1 new m @3 -> true; enhanced 1 -> m@3:new" },
  { "no copy yet",          "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; enhanced 1 -> null; enhance 9 x m @1 -> false" },
  { "the copy goes with its session", "subj A -> 1; topic 1 T -> 1; start 1 @1 -> 1; enhance 1 x m @1 -> true;"
                            "delsession 1 -> true; enhanced 1 -> null" },
  { "settings",             "setting ai_consent -> null; set ai_consent yes -> true; setting ai_consent -> yes;"
                            "set ai_consent no -> true; reopen -> ok; setting ai_consent -> no" },
  { "style samples from other topics, newest first",
                            "subj A -> 1; topic 1 T -> 1; topic 1 U -> 2; start 1 @10 -> 1; start 2 @20 -> 2;"
                            "start 2 @30 -> 3; start 2 @40 -> 4; save 2 '- two' '' @21 -> true; save 3 '- three' '' @31 -> true;"
                            "save 1 '- own topic' '' @11 -> true; style 1 5 -> - three|- two; style 1 1 -> - three; style 2 5 -> - own topic" },
  { "quotes and unicode",   "subj \"it's\" -> 1; subj 'टिप्पणी' -> 2; subjects -> it's,टिप्पणी" },
  { "case folding beyond ASCII", "subj 'Ärzte' -> 1; subj 'ärzte' -> 0; subj 'ÉCOLE' -> 2; subj 'école' -> 0" },
};

static void
test_scripts (void)
{
  JrCases c = { "scripts", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (scripts); i++)
    {
      g_autofree char *bad = run_script (scripts[i].script);
      jr_case (&c, scripts[i].name, bad == NULL, "%s", bad);
    }
  jr_cases_done (&c);
}

/* ---- when a session ends -------------------------------------------- */

/* Constraints: times are any Unix seconds. While studying (an edit
 * within 30 minutes of the last one) the end follows the edit; later
 * edits, or a clock that went back, leave it. */
static void
test_extend (void)
{
  static const struct { const char *name; gint64 started, ended, now, want; } t[] = {
    { "first edit",                1000, 1000, 1010, 1010 },
    { "edit 29 min later",         1000, 2000, 2000 + 29 * MINUTE, 2000 + 29 * MINUTE },
    { "edit exactly 30 min later", 1000, 2000, 2000 + 30 * MINUTE, 2000 + 30 * MINUTE },
    { "edit 31 min later",         1000, 2000, 2000 + 31 * MINUTE, 2000 },
    { "edit days later",           1000, 2000, 2000 + 86400 * 3, 2000 },
    { "same second",               1000, 2000, 2000, 2000 },
    { "clock went back",           1000, 2000, 1500, 2000 },
    { "clock before the start",    1000, 1000, 10, 1000 },
  };
  JrCases c = { "extend", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      gint64 got = jn_session_extend (t[i].started, t[i].ended, t[i].now);
      jr_case (&c, t[i].name, got == t[i].want, "Input %" G_GINT64_FORMAT "-%" G_GINT64_FORMAT " now %" G_GINT64_FORMAT
               "  Expected %" G_GINT64_FORMAT "  Got %" G_GINT64_FORMAT, t[i].started, t[i].ended, t[i].now,
               t[i].want, got);
    }
  /* Hidden: random times against the rule as written above. */
  for (int i = 0; i < 20000; i++)
    {
      gint64 started = g_test_rand_int_range (0, 1000000);
      gint64 ended = started + g_test_rand_int_range (0, 100000);
      gint64 now = ended + g_test_rand_int_range (-5000, 5000);
      gint64 want = (now > ended && now - ended <= 30 * MINUTE) ? now : ended;
      jr_case (&c, "random", jn_session_extend (started, ended, now) == want, "Input %" G_GINT64_FORMAT "-%"
               G_GINT64_FORMAT " now %" G_GINT64_FORMAT, started, ended, now);
    }
  jr_cases_done (&c);
}

/* ---- the file ------------------------------------------------------- */

static void
test_file (void)
{
  char *dir;
  g_autofree char *path = temp_path (&dir);
  JnDb *db = jn_db_open (path, NULL);
  g_assert_nonnull (db);
  struct stat st;
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600); /* only you */
  g_autofree char *sub = g_path_get_dirname (path);
  g_assert_cmpint (g_stat (sub, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);
  jn_db_close (db);

  /* An existing file others could read is made private. */
  g_assert_cmpint (g_chmod (path, 0644), ==, 0);
  db = jn_db_open (path, NULL);
  g_assert_nonnull (db);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  jn_db_close (db);

  /* A file from a newer Notes is refused and left alone. */
  g_autofree char *newer = g_build_filename (dir, "newer.db", NULL);
  g_assert_true (g_file_set_contents (newer, "", 0, NULL));
  {
    sqlite3 *h;
    g_assert_cmpint (sqlite3_open (newer, &h), ==, SQLITE_OK);
    g_assert_cmpint (sqlite3_exec (h, "PRAGMA user_version = 99;", NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close (h);
  }
  GError *error = NULL;
  g_assert_null (jn_db_open (newer, &error));
  g_assert_nonnull (strstr (error->message, "newer version"));
  g_clear_error (&error);

  /* Not a database at all. */
  g_assert_true (g_file_set_contents (newer, "not sqlite at all, just text................", -1, NULL));
  g_assert_null (jn_db_open (newer, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_remove (newer);
  g_remove (path);
  g_rmdir (sub);
  g_rmdir (dir);
  g_free (dir);
}

/* ---- hidden: random operations against a simple model --------------- */

typedef struct { gint64 id; char *name; gboolean alive; } MName;
typedef struct { gint64 id, subject; char *name; gboolean alive; } MTopic;

static gboolean
model_name_taken (GArray *a, gsize name_off, gint64 parent, gsize parent_off, const char *name, gint64 except)
{
  g_autofree char *want = g_utf8_casefold (name, -1);
  for (guint i = 0; i < a->len; i++)
    {
      char *base = a->data + i * g_array_get_element_size (a);
      gboolean alive = *(gboolean *) (base + (parent_off == 0 ? G_STRUCT_OFFSET (MName, alive)
                                                               : G_STRUCT_OFFSET (MTopic, alive)));
      gint64 id = *(gint64 *) base;
      if (!alive || id == except)
        continue;
      if (parent_off != 0 && *(gint64 *) (base + parent_off) != parent)
        continue;
      g_autofree char *have = g_utf8_casefold (*(char **) (base + name_off), -1);
      if (strcmp (have, want) == 0)
        return TRUE;
    }
  return FALSE;
}

static void
test_hidden_model (void)
{
  static const char *names[] = { "DSA", "dsa", "OS", "os ", " Algo", "DBMS", "x", "" };
  char *dir;
  Store st = { NULL, temp_path (&dir) };
  st.db = jn_db_open (st.path, NULL);
  GArray *subj = g_array_new (FALSE, TRUE, sizeof (MName));
  GArray *top = g_array_new (FALSE, TRUE, sizeof (MTopic));
  gint64 next_subj = 1, next_topic = 1;
  JrCases c = { "model (hidden)", 0, 0 };
  for (int i = 0; i < 1500; i++)
    {
      int op = g_test_rand_int_range (0, 5);
      const char *raw = names[g_test_rand_int_range (0, G_N_ELEMENTS (names))];
      g_autofree char *name = g_strstrip (g_strdup (raw));
      if (op == 0) /* add a subject */
        {
          gboolean ok = *name != '\0' && !model_name_taken (subj, G_STRUCT_OFFSET (MName, name), 0, 0, name, 0);
          gint64 got = jn_db_add_subject (st.db, raw);
          jr_case (&c, "add subject", got == (ok ? next_subj : 0), "Input \"%s\"  Expected %" G_GINT64_FORMAT
                   "  Got %" G_GINT64_FORMAT, raw, ok ? next_subj : 0, got);
          if (got > 0)
            {
              MName m = { got, g_strdup (name), TRUE };
              g_array_append_val (subj, m);
              next_subj = got + 1;
            }
        }
      else if (op == 1 && subj->len > 0) /* add a topic */
        {
          MName *s = &g_array_index (subj, MName, g_test_rand_int_range (0, subj->len));
          gboolean ok = s->alive && *name != '\0' &&
                        !model_name_taken (top, G_STRUCT_OFFSET (MTopic, name), s->id,
                                           G_STRUCT_OFFSET (MTopic, subject), name, 0);
          gint64 got = jn_db_add_topic (st.db, s->id, raw);
          jr_case (&c, "add topic", (got > 0) == ok, "Input %" G_GINT64_FORMAT " \"%s\"  Expected %s",
                   s->id, raw, ok ? "added" : "refused");
          if (got > 0)
            {
              MTopic t = { got, s->id, g_strdup (name), TRUE };
              g_array_append_val (top, t);
              next_topic = got + 1;
            }
        }
      else if (op == 2 && subj->len > 0) /* delete a subject */
        {
          MName *s = &g_array_index (subj, MName, g_test_rand_int_range (0, subj->len));
          gboolean got = jn_db_delete_subject (st.db, s->id);
          jr_case (&c, "delete subject", got == s->alive, "Input %" G_GINT64_FORMAT, s->id);
          for (guint k = 0; k < top->len; k++)
            if (g_array_index (top, MTopic, k).subject == s->id)
              g_array_index (top, MTopic, k).alive = FALSE;
          s->alive = FALSE;
        }
      else if (op == 3 && subj->len > 0) /* rename a subject */
        {
          MName *s = &g_array_index (subj, MName, g_test_rand_int_range (0, subj->len));
          gboolean ok = s->alive && *name != '\0' &&
                        !model_name_taken (subj, G_STRUCT_OFFSET (MName, name), 0, 0, name, s->id);
          gboolean got = jn_db_rename_subject (st.db, s->id, raw);
          jr_case (&c, "rename subject", got == ok, "Input %" G_GINT64_FORMAT " \"%s\"", s->id, raw);
          if (got)
            {
              g_free (s->name);
              s->name = g_strdup (name);
            }
        }
      else if (op == 4 && g_test_rand_int_range (0, 30) == 0) /* restart */
        {
          jn_db_close (st.db);
          st.db = jn_db_open (st.path, NULL);
        }
      /* After every step the topic lists agree. */
      for (guint k = 0; k < subj->len; k++)
        {
          MName *s = &g_array_index (subj, MName, k);
          GString *have = g_string_new (NULL);
          jn_db_foreach_topic (st.db, s->id, collect_name, have);
          guint n = 0;
          for (guint t = 0; t < top->len; t++)
            n += g_array_index (top, MTopic, t).subject == s->id && g_array_index (top, MTopic, t).alive;
          guint got_n = have->len > 0; /* names hold no commas */
          for (gsize p = 0; p < have->len; p++)
            got_n += have->str[p] == ',';
          jr_case (&c, "topic count", got_n == n, "Subject %" G_GINT64_FORMAT "  Expected %u  Got %u (%s)", s->id, n,
                   got_n, have->str);
          g_string_free (have, TRUE);
        }
    }
  (void) next_topic;
  jr_cases_done (&c);
  for (guint k = 0; k < subj->len; k++)
    g_free (g_array_index (subj, MName, k).name);
  for (guint k = 0; k < top->len; k++)
    g_free (g_array_index (top, MTopic, k).name);
  g_array_unref (subj);
  g_array_unref (top);
  remove_store (&st, dir);
  g_free (dir);
}

/* ---- huge inputs ------------------------------------------------------ */

static void
count_session (gint64 id, gint64 started, gint64 ended, gpointer user)
{
  (void) id; (void) started; (void) ended;
  (*(int *) user)++;
}

/* A 1 MiB note saves and reads back; a topic with 10 000 sessions lists
 * quickly (a year of daily sessions is under 400). */
static void
test_huge (void)
{
  char *dir;
  Store st = { NULL, temp_path (&dir) };
  st.db = jn_db_open (st.path, NULL);
  gint64 s = jn_db_add_subject (st.db, "DSA"), t = jn_db_add_topic (st.db, s, "Arrays");
  gint64 id = jn_db_start_session (st.db, t, 1000);
  GString *big = g_string_new (NULL);
  while (big->len < (1 << 20))
    g_string_append (big, "- prefix sums turn range sums into one subtraction\n");
  gint64 t0 = g_get_monotonic_time ();
  g_assert_true (jn_db_save_session (st.db, id, big->str, "", 1100));
  JnSession got = { 0 };
  g_assert_true (jn_db_get_session (st.db, id, &got));
  g_assert_cmpstr (got.body, ==, big->str);
  jn_session_clear (&got);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 2 * G_USEC_PER_SEC);
  g_string_free (big, TRUE);

  int n = g_getenv ("JR_TEST_WRAPPER") != NULL ? 1000 : 10000;
  for (int i = 0; i < n; i++)
    jn_db_start_session (st.db, t, 2000 + i);
  t0 = g_get_monotonic_time ();
  int count = 0;
  jn_db_foreach_session (st.db, t, count_session, &count);
  g_assert_cmpint (count, ==, n + 1);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, G_USEC_PER_SEC);
  remove_store (&st, dir);
  g_free (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/notesdb/scripts", test_scripts);
  g_test_add_func ("/notesdb/extend", test_extend);
  g_test_add_func ("/notesdb/file", test_file);
  g_test_add_func ("/notesdb/hidden-model", test_hidden_model);
  g_test_add_func ("/notesdb/huge", test_huge);
  return g_test_run ();
}
