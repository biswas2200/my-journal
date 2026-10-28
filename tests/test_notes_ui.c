/* UI tests for Notes (src/notes/ui), on a private headless display.
 *
 * The editor is driven key by key, as a person types: characters go in
 * at the cursor, and Return, BackSpace, Tab, Shift+Tab and space first go
 * to the editor's key handler, then (if it does not take them) do what
 * GTK would. The whole test runs inside Notes' file lock, as the real
 * program does. */
#include <string.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include "cases.h"
#include "editor.h"
#include "notes_window.h"
#include "notesjail.h"
#include "programs.h"

static gboolean
stop_waiting (gpointer flag)
{
  *(gboolean *) flag = TRUE;
  return G_SOURCE_REMOVE;
}

static void
pump (int ms)
{
  gboolean done = FALSE;
  g_timeout_add (ms, stop_waiting, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
}

static GtkTextBuffer *
buf (JnEditor *e)
{
  return gtk_text_view_get_buffer (jn_editor_view (e));
}

/* One key, as GTK would handle it after the editor. */
static void
press (JnEditor *e, guint keyval, GdkModifierType state)
{
  if (jn_editor_key (e, keyval, state))
    return;
  GtkTextBuffer *b = buf (e);
  switch (keyval)
    {
    case GDK_KEY_space:  gtk_text_buffer_insert_interactive_at_cursor (b, " ", 1, TRUE); break;
    case GDK_KEY_Return: gtk_text_buffer_insert_interactive_at_cursor (b, "\n", 1, TRUE); break;
    case GDK_KEY_BackSpace:
      {
        GtkTextIter c;
        gtk_text_buffer_get_iter_at_mark (b, &c, gtk_text_buffer_get_insert (b));
        gtk_text_buffer_backspace (b, &c, TRUE, TRUE);
        break;
      }
    default: break;
    }
}

/* Types `text`: "⏎" Return, "⌫" BackSpace, "⇥" Tab, "⇤" Shift+Tab, " " a
 * space through the key handler; everything else is inserted. */
static void
type (JnEditor *e, const char *text)
{
  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c == 0x23CE)
        press (e, GDK_KEY_Return, 0);
      else if (c == 0x232B)
        press (e, GDK_KEY_BackSpace, 0);
      else if (c == 0x21E5)
        press (e, GDK_KEY_Tab, 0);
      else if (c == 0x21E4)
        press (e, GDK_KEY_ISO_Left_Tab, GDK_SHIFT_MASK);
      else if (c == ' ')
        press (e, GDK_KEY_space, 0);
      else
        {
          char ch[8];
          int n = g_unichar_to_utf8 (c, ch);
          gtk_text_buffer_insert_interactive_at_cursor (buf (e), ch, n, TRUE);
        }
      g_main_context_iteration (NULL, FALSE); /* lets the styles follow, as between real keys */
    }
}

static JnEditor *
new_editor (GtkWidget **window)
{
  *window = gtk_window_new ();
  JnEditor *e = JN_EDITOR (jn_editor_new ());
  gtk_window_set_child (GTK_WINDOW (*window), GTK_WIDGET (e));
  gtk_window_present (GTK_WINDOW (*window));
  pump (100);
  return e;
}

/* ---- the editor ---------------------------------------------------------- */

/* Every kind, and text that only looks like a kind, comes back exactly. */
static void
test_round_trip (void)
{
  static const char *bodies[] = {
    "",
    "plain",
    "# Prefix sum\n## Idea\n- pre[0] = 0\n  - build once\n→ each query is O(1)\nwhat about 2D?",
    "\\# not a heading\n\\- not a bullet\n\\→ not an arrow\n● typed glyph\n#tag",
    "- \n\n# \n→ ",
    "- unicode टिप्पणी ✓\n  - deeper",
  };
  GtkWidget *w;
  JnEditor *e = new_editor (&w);
  JrCases c = { "round trip", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (bodies); i++)
    for (int md = 0; md < 2; md++)
      {
        jn_editor_set_markdown (e, md);
        jn_editor_load (e, bodies[i], "https://a\nhttps://b");
        pump (20);
        g_autofree char *got = jn_editor_body (e);
        g_autofree char *src = jn_editor_sources (e);
        jr_case (&c, md ? "markdown mode" : "formatted", g_strcmp0 (got, bodies[i]) == 0 &&
                 g_strcmp0 (src, "https://a\nhttps://b") == 0, "Input \"%s\"  Got \"%s\"", bodies[i], got);
      }
  jr_cases_done (&c);
  gtk_window_destroy (GTK_WINDOW (w));
}

/* Typing scripts and the stored lines they must give. */
static void
test_typing (void)
{
  static const struct { const char *name, *keys, *want; } t[] = {
    { "dash space makes a bullet",   "- pre[0] = 0",                     "- pre[0] = 0" },
    { "star space too",              "* x",                              "- x" },
    { "hash space makes a heading",  "# Prefix sum",                     "# Prefix sum" },
    { "two hashes a subheading",     "## Idea",                          "## Idea" },
    { "> makes an arrow",            "> O(1)",                           "→ O(1)" },
    { "-> too",                      "-> O(1)",                          "→ O(1)" },
    { "Enter continues a list",      "- a⏎b⏎c",                          "- a\n- b\n- c" },
    { "Enter on an empty item ends it", "- a⏎⏎after",                    "- a\nafter" },
    { "Tab indents a bullet",        "- a⏎⇥b",                           "- a\n  - b" },
    { "Enter keeps the indent",      "- a⏎⇥b⏎c",                         "- a\n  - b\n  - c" },
    { "Shift+Tab outdents",          "- a⏎⇥b⏎⇤c",                        "- a\n  - b\n- c" },
    { "Shift+Tab ends a bullet",     "- a⇤",                             "a" },
    { "Backspace after the bullet",  "- ⌫text",                          "text" },
    { "Backspace after a sub-bullet","- a⏎⇥⌫b",                          "- a\n- b" },
    { "Enter on plain text adds a line", "- a⏎⏎⏎b",                       "- a\n\nb" },
    { "Enter after a heading is text", "# Title⏎body",                   "# Title\nbody" },
    { "markers only at line start",  "a - b # c",                        "a - b # c" },
    { "three hashes stay text",      "### x",                            "### x" },
    { "the whole note",              "# Prefix sum⏎## Idea⏎- pre[0] = 0⏎⇥build once⏎⏎> each query is O(1)",
                                     "# Prefix sum\n## Idea\n- pre[0] = 0\n  - build once\n→ each query is O(1)" },
  };
  GtkWidget *w;
  JnEditor *e = new_editor (&w);
  JrCases c = { "typing", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (t); i++)
    {
      jn_editor_load (e, "", "");
      jn_editor_focus (e);
      type (e, t[i].keys);
      pump (10);
      g_autofree char *got = jn_editor_body (e);
      jr_case (&c, t[i].name, g_strcmp0 (got, t[i].want) == 0, "Keys \"%s\"  Expected \"%s\"  Got \"%s\"", t[i].keys,
               t[i].want, got);
    }
  jr_cases_done (&c);
  gtk_window_destroy (GTK_WINDOW (w));
}

/* Undo takes back a conversion in one step. */
static void
test_undo (void)
{
  GtkWidget *w;
  JnEditor *e = new_editor (&w);
  jn_editor_load (e, "", "");
  type (e, "- a");
  GtkTextBuffer *b = buf (e);
  gtk_text_buffer_undo (b); /* the "a" */
  gtk_text_buffer_undo (b); /* the bullet, back to "-" */
  g_autofree char *got = jn_editor_body (e);
  g_assert_cmpstr (got, ==, "-");
  gtk_window_destroy (GTK_WINDOW (w));
}

/* The cursor never stops inside a bullet or a heading marker. */
static void
test_cursor_skips_prefix (void)
{
  GtkWidget *w;
  JnEditor *e = new_editor (&w);
  jn_editor_load (e, "text\n- bullet\n# heading", "");
  GtkTextBuffer *b = buf (e);
  JrCases c = { "cursor", 0, 0 };
  for (int line = 1; line <= 2; line++)
    {
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_line (b, &it, line);
      gtk_text_buffer_place_cursor (b, &it); /* a click at the very start */
      GtkTextIter got;
      gtk_text_buffer_get_iter_at_mark (b, &got, gtk_text_buffer_get_insert (b));
      jr_case (&c, line == 1 ? "bullet" : "heading", gtk_text_iter_get_line_offset (&got) == (line == 1 ? 2 : 1),
               "Expected after the prefix  Got offset %d", gtk_text_iter_get_line_offset (&got));
      g_assert_cmpint (jn_editor_kind_at_cursor (e), ==, line == 1 ? JN_BULLET : JN_HEADING);
    }
  /* Left from the start of the text goes on to the line above. */
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_line_offset (b, &it, 1, 2);
  gtk_text_buffer_place_cursor (b, &it);
  gtk_text_buffer_get_iter_at_line_offset (b, &it, 1, 1);
  gtk_text_buffer_place_cursor (b, &it);
  GtkTextIter got;
  gtk_text_buffer_get_iter_at_mark (b, &got, gtk_text_buffer_get_insert (b));
  jr_case (&c, "left goes up", gtk_text_iter_get_line (&got) == 0 && gtk_text_iter_ends_line (&got),
           "Expected the end of line 0  Got line %d offset %d", gtk_text_iter_get_line (&got),
           gtk_text_iter_get_line_offset (&got));
  /* Enter before a heading's text opens a line above it. */
  jn_editor_load (e, "# Title\n- item", "");
  for (int line = 0; line <= 2; line += 2) /* the item is line 2 once a line is above the title */
    {
      gtk_text_buffer_get_iter_at_line (b, &it, line);
      gtk_text_buffer_place_cursor (b, &it); /* lands after the prefix */
      press (e, GDK_KEY_Return, 0);
    }
  g_autofree char *above = jn_editor_body (e);
  jr_case (&c, "Enter before the text", g_strcmp0 (above, "\n# Title\n\n- item") == 0, "Got \"%s\"", above);
  jr_cases_done (&c);
  gtk_window_destroy (GTK_WINDOW (w));
}

/* The toolbar changes every selected line, in both modes. */
static void
test_toolbar (void)
{
  GtkWidget *w;
  JnEditor *e = new_editor (&w);
  JrCases c = { "toolbar", 0, 0 };
  for (int md = 0; md < 2; md++)
    {
      jn_editor_set_markdown (e, md);
      jn_editor_load (e, "one\ntwo\nthree\nfour", "");
      GtkTextBuffer *b = buf (e);
      GtkTextIter a, z;
      gtk_text_buffer_get_iter_at_line (b, &a, 1);
      gtk_text_buffer_get_iter_at_line (b, &z, 3); /* ends at the start of "four": not that line */
      gtk_text_buffer_select_range (b, &a, &z);
      jn_editor_set_kind (e, JN_BULLET);
      g_autofree char *got = jn_editor_body (e);
      jr_case (&c, md ? "markdown" : "formatted", g_strcmp0 (got, "one\n- two\n- three\nfour") == 0, "Got \"%s\"", got);
      gtk_text_buffer_get_iter_at_line (b, &a, 2);
      gtk_text_buffer_place_cursor (b, &a);
      jn_editor_set_kind (e, JN_HEADING);
      g_autofree char *got2 = jn_editor_body (e);
      jr_case (&c, md ? "markdown, one line" : "formatted, one line",
               g_strcmp0 (got2, "one\n- two\n# three\nfour") == 0, "Got \"%s\"", got2);
    }
  jr_cases_done (&c);
  gtk_window_destroy (GTK_WINDOW (w));
}

/* Markdown typed by hand reads back into formats. */
static void
test_markdown_mode (void)
{
  GtkWidget *w;
  JnEditor *e = new_editor (&w);
  jn_editor_load (e, "# Title\n- a", "");
  jn_editor_set_markdown (e, TRUE);
  GtkTextBuffer *b = buf (e);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (b, &end);
  gtk_text_buffer_insert (b, &end, "\n\t* deep\n+ plus\n-> so", -1);
  jn_editor_set_markdown (e, FALSE);
  g_autofree char *got = jn_editor_body (e);
  g_assert_cmpstr (got, ==, "# Title\n- a\n  - deep\n- plus\n→ so");
  gtk_window_destroy (GTK_WINDOW (w));
}

/* ---- the window ------------------------------------------------------------ */

typedef struct {
  int calls, code;
} Left;

static void
on_left (int code, gpointer user)
{
  Left *l = user;
  l->calls++;
  l->code = code;
}

static void
tally (gint64 id, gint64 started, gint64 ended, gpointer user)
{
  (void) id; (void) started; (void) ended;
  (*(int *) user)++;
}

static int
sessions_of (JnDb *db, gint64 topic)
{
  int n = 0;
  jn_db_foreach_session (db, topic, tally, &n);
  return n;
}

static GtkWidget *
find_button (GtkWidget *root, const char *label)
{
  if (GTK_IS_BUTTON (root) && g_strcmp0 (gtk_button_get_label (GTK_BUTTON (root)), label) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *f = find_button (c, label);
      if (f != NULL)
        return f;
    }
  return NULL;
}

static void
test_window_flow (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *exports = g_build_filename (dir, "exports", NULL);
  JnDb *db = jn_db_open (path, NULL);
  JnWindow *win = jn_window_new (NULL, db, exports);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);
  JnEditor *e = jn_window_editor (win);

  /* A subject and a topic; opening the topic creates nothing yet. */
  gint64 dsa = jn_window_add_subject (win, "DSA");
  g_assert_cmpint (dsa, >, 0);
  g_assert_cmpint (jn_window_add_subject (win, "dsa"), ==, 0); /* taken */
  gint64 prefix = jn_window_add_topic (win, "Prefix sum");
  g_assert_cmpint (prefix, >, 0);
  g_assert_cmpint (jn_window_topic (win), ==, prefix);
  g_assert_cmpint (jn_window_session (win), ==, 0);
  g_assert_cmpint (sessions_of (db, prefix), ==, 0);

  /* The first keystroke starts the session; it saves a second later. */
  jn_editor_focus (e);
  type (e, "# Prefix sum⏎- pre[0] = 0");
  gint64 first = jn_window_session (win);
  g_assert_cmpint (first, >, 0);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (jn_editor_sources_view (e)), "https://cp-algorithms.com", -1);
  pump (1300);
  JnSession s = { 0 };
  g_assert_true (jn_db_get_session (db, first, &s));
  g_assert_cmpstr (s.body, ==, "# Prefix sum\n- pre[0] = 0");
  g_assert_cmpstr (s.sources, ==, "https://cp-algorithms.com");
  jn_session_clear (&s);

  /* Typed just before switching topics: saved without waiting. */
  type (e, "⏎range sum");
  gint64 sliding = jn_window_add_topic (win, "Sliding window");
  g_assert_cmpint (jn_window_topic (win), ==, sliding);
  g_assert_true (jn_db_get_session (db, first, &s));
  g_assert_cmpstr (s.body, ==, "# Prefix sum\n- pre[0] = 0\n- range sum");
  jn_session_clear (&s);

  /* Looking at a topic and leaving it adds no empty session. */
  jn_window_open_topic (win, prefix);
  g_assert_cmpint (sessions_of (db, sliding), ==, 0);
  /* A new session, left empty, is dropped too. */
  jn_window_open_session (win, first);
  jn_window_new_session (win);
  type (e, "x⌫");
  g_assert_cmpint (sessions_of (db, prefix), ==, 2);
  jn_window_open_session (win, first);
  g_assert_cmpint (sessions_of (db, prefix), ==, 1);
  g_autofree char *body = jn_editor_body (e);
  g_assert_cmpstr (body, ==, "# Prefix sum\n- pre[0] = 0\n- range sum");

  /* Export: the topic as Markdown in <exports>/DSA/Prefix sum.md. */
  g_autoptr (GError) error = NULL;
  g_autofree char *file = jn_window_export (win, &error);
  g_assert_no_error (error);
  g_autofree char *want_path = g_build_filename (exports, "DSA", "Prefix sum.md", NULL);
  g_assert_cmpstr (file, ==, want_path);
  g_autofree char *md = NULL;
  g_assert_true (g_file_get_contents (file, &md, NULL, NULL));
  g_assert_true (g_str_has_prefix (md, "# Prefix sum\n\nSubject: DSA\n\n## "));
  g_assert_nonnull (strstr (md, "Sources:\n- <https://cp-algorithms.com>\n\n### Prefix sum\n- pre[0] = 0\n- range sum\n"));

  /* Home: saves, reports where to go, closes. */
  Left left = { 0, -1 };
  jn_window_set_navigation (win, on_left, &left);
  type (e, "⏎last line");
  GtkWidget *home = find_button (GTK_WIDGET (win), "Home");
  g_assert_nonnull (home);
  g_object_add_weak_pointer (G_OBJECT (win), (gpointer *) &win);
  g_signal_emit_by_name (home, "clicked");
  pump (100);
  g_assert_null (win);
  g_assert_cmpint (left.calls, ==, 1);
  g_assert_cmpint (left.code, ==, JR_EXIT_HOME);
  db = jn_db_open (path, NULL);
  g_assert_true (jn_db_get_session (db, first, &s));
  g_assert_cmpstr (s.body, ==, "# Prefix sum\n- pre[0] = 0\n- range sum\n- last line"); /* the list goes on */
  jn_session_clear (&s);
  jn_db_close (db);

  g_remove (file);
  g_autofree char *sub = g_path_get_dirname (file);
  g_rmdir (sub);
  g_rmdir (exports);
  g_remove (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_setenv ("GSK_RENDERER", "cairo", FALSE);
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    {
      g_test_skip ("no display");
      return 77;
    }
  /* Inside the same file lock as the real Notes. */
  JnJailPaths *paths = jn_jail_default_paths ();
  g_autoptr (GError) error = NULL;
  g_assert_true (jn_jail_enter (paths, &error));
  jn_jail_paths_free (paths);

  g_test_add_func ("/notes-ui/round-trip", test_round_trip);
  g_test_add_func ("/notes-ui/typing", test_typing);
  g_test_add_func ("/notes-ui/undo", test_undo);
  g_test_add_func ("/notes-ui/cursor-skips-prefix", test_cursor_skips_prefix);
  g_test_add_func ("/notes-ui/toolbar", test_toolbar);
  g_test_add_func ("/notes-ui/markdown-mode", test_markdown_mode);
  g_test_add_func ("/notes-ui/window-flow", test_window_flow);
  return g_test_run ();
}
