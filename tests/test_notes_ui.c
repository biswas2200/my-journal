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
#include "clock_bar.h"
#include "editor.h"
#include "md_view.h"
#include "notebackup.h"
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
    { "a heading in an empty item",  "> a⏎## Doubts",                    "→ a\n## Doubts" },
    { "a bullet in an empty arrow",  "> a⏎- b",                          "→ a\n- b" },
    { "not inside an item's text",   "- a # b",                          "- a # b" },
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

/* Markdown mode shows the note formatted beside what is typed, and keeps
 * up with typing; outside Markdown mode there is no preview. */
static void
test_markdown_preview (void)
{
  GtkWidget *w;
  JnEditor *e = new_editor (&w);
  JnMdView *pv = jn_editor_preview (e);
  jn_editor_load (e, "# Title\n- a **bold** point\n→ so", "");
  g_assert_false (gtk_widget_is_visible (GTK_WIDGET (pv)));
  jn_editor_set_markdown (e, TRUE);
  pump (100);
  g_assert_true (gtk_widget_is_visible (GTK_WIDGET (pv)));
  GtkTextBuffer *pb = gtk_text_view_get_buffer (GTK_TEXT_VIEW (pv));
  GtkTextIter a, z;
  gtk_text_buffer_get_bounds (pb, &a, &z);
  g_autofree char *shown = gtk_text_buffer_get_text (pb, &a, &z, FALSE);
  g_assert_nonnull (strstr (shown, "Title"));
  g_assert_nonnull (strstr (shown, "bold point"));
  g_assert_null (strstr (shown, "# "));
  g_assert_null (strstr (shown, "**"));
  g_assert_null (strstr (shown, "- a"));
  /* Typing shows up in the preview shortly after. */
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buf (e), &end);
  gtk_text_buffer_insert (buf (e), &end, "\n## Later part", -1);
  pump (600);
  g_assert_cmpstr (jn_md_view_get_markdown (pv), ==, "# Title\n- a **bold** point\n→ so\n## Later part");
  jn_editor_set_markdown (e, FALSE);
  g_assert_false (gtk_widget_is_visible (GTK_WIDGET (pv)));
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

/* The enhanced copy shown formatted: no markup left on screen, links
 * with their address, a clean diagram drawn, an unsafe one refused. */
static void
test_md_view (void)
{
  /* As in the window: inside a scrolled view, a fixed width. */
  GtkWidget *w = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (w), 520, 600);
  JnMdView *v = JN_MD_VIEW (jn_md_view_new ());
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (v));
  gtk_window_set_child (GTK_WINDOW (w), scroll);
  gtk_window_present (GTK_WINDOW (w));
  static const char md[] =
    "# Prefix sum (enhanced)\n"
    "\n"
    "## Idea\n"
    "- pre[i] is a **sum** of the first *i* items [1]\n"
    "  - build it once\n"
    "→ each query is `O(1)`\n"
    "1. read the input\n"
    "See [USACO](https://usaco.guide/silver/prefix-sums).\n"
    "```python\n"
    "def psum(a):\n"
    "    return a\n"
    "```\n"
    "```svg\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"60\"><script>x()</script>"
    "<rect x=\"5\" y=\"5\" width=\"100\" height=\"30\" fill=\"#93c5fd\"/></svg>\n"
    "```\n"
    "```svg\n"
    "<!DOCTYPE svg [<!ENTITY a \"x\">]><svg xmlns=\"http://www.w3.org/2000/svg\"><text>&a;</text></svg>\n"
    "```\n"
    "---\n";
  jn_md_view_set_markdown (v, md);
  pump (100);
  GtkTextBuffer *b = gtk_text_view_get_buffer (GTK_TEXT_VIEW (v));
  GtkTextIter a, z;
  gtk_text_buffer_get_bounds (b, &a, &z);
  g_autofree char *shown = gtk_text_buffer_get_text (b, &a, &z, FALSE);
  JrCases c = { "md view", 0, 0 };
  static const char *absent[] = { "##", "**", "```", "<svg", "](http", "script" };
  for (gsize i = 0; i < G_N_ELEMENTS (absent); i++)
    jr_case (&c, absent[i], strstr (shown, absent[i]) == NULL, "Expected no \"%s\" on screen: %s", absent[i], shown);
  static const char *present[] = { "Prefix sum (enhanced)", "\xe2\x97\x8f pre[i] is a sum of the first i items [1]",
                                   "\xe2\x97\xa6 build it once", "\xe2\x86\x92 each query is O(1)", "1. read the input",
                                   "See USACO.", "python\ndef psum(a):\n    return a", "did not pass the safety check" };
  for (gsize i = 0; i < G_N_ELEMENTS (present); i++)
    jr_case (&c, present[i], strstr (shown, present[i]) != NULL, "Expected \"%s\" on screen: %s", present[i], shown);
  int refused = 0;
  jr_case (&c, "one diagram drawn, one refused", jn_md_view_diagrams (v, &refused) == 1 && refused == 1,
           "Got %d drawn, %d refused", jn_md_view_diagrams (v, NULL), refused);
  const char *usaco = strstr (shown, "USACO");
  int at = usaco != NULL ? (int) g_utf8_pointer_to_offset (shown, usaco) : 0;
  jr_case (&c, "the link has its address", g_strcmp0 (jn_md_view_link_at (v, at), "https://usaco.guide/silver/prefix-sums") == 0,
           "Got %s", jn_md_view_link_at (v, at));
  jr_case (&c, "plain text is no link", jn_md_view_link_at (v, 0) == NULL, "Expected none");
  /* The title is set large. */
  GtkTextIter first;
  gtk_text_buffer_get_start_iter (b, &first);
  GSList *tags = gtk_text_iter_get_tags (&first);
  double scale = 1.0;
  for (GSList *l = tags; l != NULL; l = l->next)
    {
      gboolean set = FALSE;
      double sc = 1.0;
      g_object_get (l->data, "scale-set", &set, "scale", &sc, NULL);
      if (set)
        scale = MAX (scale, sc);
    }
  g_slist_free (tags);
  jr_case (&c, "title is large", scale > 1.3, "Got scale %.2f", scale);
  jr_case (&c, "markdown kept", g_strcmp0 (jn_md_view_get_markdown (v), md) == 0, "Expected the source kept");
  /* A diagram wider than the view is scaled down: the view never needs
   * more width than it has (else the pane would run off the window). */
  jn_md_view_set_markdown (v, "```svg\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1200\" height=\"300\">"
                              "<rect width=\"1200\" height=\"300\" fill=\"#93c5fd\"/></svg>\n```\n");
  pump (200);
  int need = 0, room = 520; /* the window's width above; GTK would grow it to fit */
  gtk_widget_measure (GTK_WIDGET (v), GTK_ORIENTATION_HORIZONTAL, -1, &need, NULL, NULL, NULL);
  jr_case (&c, "wide diagram fits", need <= room, "Needs %d px, the pane has %d px", need, room);
  jr_cases_done (&c);
  gtk_window_destroy (GTK_WINDOW (w));
}

/* ---- the AI: a stand-in that records what it is sent ------------------ */

typedef struct {
  JnEnhancer    base;
  int           calls;
  char         *note, *subject, *topic, *links, *style;
  const char   *reply, *error;
} Fake;

typedef struct {
  Fake         *f;
  JnEnhanceDone done;
  gpointer      user;
} FakeCall;

static gboolean
fake_answer (gpointer data)
{
  FakeCall *c = data;
  JnAiSource src[] = { { (char *) "https://cp-algorithms.com/prefix", NULL, NULL },
                       { (char *) "ftp://old", NULL, (char *) "only http and https links are fetched" } };
  gsize n = c->f->links != NULL && *c->f->links != '\0' ? 2 : 0; /* no links, no sources */
  JnAiAnswer a = { (char *) c->f->reply, (char *) "gemini-3.8-flash", NULL, src, n };
  c->done (c->f->error == NULL ? &a : NULL, c->f->error, c->user);
  g_free (c);
  return G_SOURCE_REMOVE;
}

static void
fake_start (JnEnhancer *self, const JnAiInput *in, char **links, GCancellable *cancel, JnEnhanceDone done,
            gpointer user)
{
  (void) cancel;
  Fake *f = (Fake *) self;
  f->calls++;
  g_free (f->note);
  g_free (f->subject);
  g_free (f->topic);
  g_free (f->links);
  g_free (f->style);
  f->note = g_strdup (in->note);
  f->subject = g_strdup (in->subject);
  f->topic = g_strdup (in->topic);
  f->links = g_strjoinv ("|", links);
  f->style = g_strjoinv ("|", (char **) in->style);
  FakeCall *c = g_new0 (FakeCall, 1);
  c->f = f;
  c->done = done;
  c->user = user;
  g_timeout_add (50, fake_answer, c); /* answers later, like the real one */
}

static char *
fake_describe (JnEnhancer *self)
{
  (void) self;
  return g_strdup ("a stand-in AI");
}

static void
fake_free (JnEnhancer *self)
{
  (void) self; /* the test owns it */
}

static void
close_other_windows (GtkWindow *keep)
{
  GList *all = gtk_window_list_toplevels ();
  for (GList *l = all; l != NULL; l = l->next)
    if (l->data != keep && gtk_widget_get_visible (l->data))
      gtk_window_destroy (l->data);
  g_list_free (all);
}

static GtkWidget *
find_row_label (GtkWidget *root, const char *text)
{
  if (GTK_IS_LABEL (root) && g_strcmp0 (gtk_label_get_text (GTK_LABEL (root)), text) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *f = find_row_label (c, text);
      if (f != NULL)
        return f;
    }
  return NULL;
}

static void
test_enhance (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-ai-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  JnDb *db = jn_db_open (path, NULL);
  /* Another topic's note: a sample of how this person writes. */
  gint64 s0 = jn_db_add_subject (db, "OS"), t0 = jn_db_add_topic (db, s0, "Paging");
  gint64 old = jn_db_start_session (db, t0, 100);
  jn_db_save_session (db, old, "- pages are fixed-size blocks", "", 200);
  JnWindow *win = jn_window_new (NULL, db, dir);
  Fake fake = { { fake_start, fake_describe, fake_free }, 0, NULL, NULL, NULL, NULL, NULL, "## Idea\n- more [1]", NULL };
  jn_window_set_enhancer (win, &fake.base);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);
  JnEditor *e = jn_window_editor (win);

  gint64 dsa = jn_window_add_subject (win, "DSA");
  g_assert_cmpint (dsa, >, 0);
  jn_window_add_topic (win, "Prefix sum");
  g_assert_false (jn_window_enhance (win)); /* nothing written yet */
  g_assert_cmpint (fake.calls, ==, 0);
  type (e, "# Prefix sum⏎- pre[0] = 0");
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (jn_editor_sources_view (e)), "https://cp-algorithms.com/prefix\nftp://old", -1);
  gint64 session = jn_window_session (win);

  /* The first time, it asks; nothing goes anywhere until you say yes. */
  g_assert_false (jn_window_enhance (win));
  pump (200);
  g_assert_cmpint (fake.calls, ==, 0);
  close_other_windows (GTK_WINDOW (win)); /* "Cancel" */
  pump (100);
  g_assert_cmpint (fake.calls, ==, 0);
  g_assert_null (jn_db_get_setting (db, "ai_consent"));

  /* Once agreed: the note, its links and a style sample go to the AI. */
  jn_db_set_setting (db, "ai_consent", "yes");
  g_assert_true (jn_window_enhance (win));
  g_assert_true (jn_window_is_enhancing (win));
  g_assert_false (jn_window_enhance (win)); /* one at a time */
  g_assert_cmpint (fake.calls, ==, 1);
  g_assert_cmpstr (fake.note, ==, "# Prefix sum\n- pre[0] = 0");
  g_assert_cmpstr (fake.subject, ==, "DSA");
  g_assert_cmpstr (fake.topic, ==, "Prefix sum");
  g_assert_cmpstr (fake.links, ==, "https://cp-algorithms.com/prefix|ftp://old");
  g_assert_cmpstr (fake.style, ==, "- pages are fixed-size blocks");

  /* Writing goes on meanwhile; the answer lands next to the note. */
  type (e, "⏎range sums");
  pump (300);
  g_assert_false (jn_window_is_enhancing (win));
  /* The list offers the topic reference as soon as the copy is in. */
  g_assert_nonnull (find_row_label (GTK_WIDGET (win), "\xe2\x98\x85 Topic reference"));
  g_autofree char *shown = jn_window_enhanced_text (win);
  g_assert_nonnull (shown);
  g_assert_true (g_str_has_prefix (shown, "# Prefix sum (enhanced)\n\nDSA \xc2\xb7 studied "));
  g_assert_nonnull (strstr (shown, "1. <https://cp-algorithms.com/prefix>\n2. <ftp://old> (not used: only http"));
  g_assert_nonnull (strstr (shown, "## Idea\n- more [1]\n"));
  g_autofree char *model = NULL;
  g_autofree char *stored = jn_db_get_enhanced (db, session, &model, NULL);
  g_assert_cmpstr (stored, ==, shown);
  g_assert_cmpstr (model, ==, "gemini-3.8-flash");
  /* Your note is untouched by it. */
  g_autofree char *body = jn_editor_body (e);
  g_assert_cmpstr (body, ==, "# Prefix sum\n- pre[0] = 0\n- range sums");

  /* Elsewhere and back: the copy comes back with its session. */
  jn_window_open_session (win, old);
  g_assert_null (jn_window_enhanced_text (win));
  jn_window_open_session (win, session);
  g_autofree char *again = jn_window_enhanced_text (win);
  g_assert_cmpstr (again, ==, stored);

  /* The topic reference: every copy of the topic, one document. */
  g_assert_false (jn_window_in_reference (win));
  jn_window_open_reference (win, jn_window_topic (win));
  g_assert_true (jn_window_in_reference (win));
  g_autofree char *ref = jn_window_enhanced_text (win);
  g_assert_true (g_str_has_prefix (ref, "# Prefix sum (reference)\n\nDSA \xc2\xb7 1 session with an enhanced copy"));
  g_assert_nonnull (strstr (ref, "### Idea\n- more [1]\n"));
  g_autoptr (GError) xerr = NULL;
  g_autofree char *ref_file = jn_window_export (win, &xerr);
  g_assert_no_error (xerr);
  g_autofree char *want_ref = g_build_filename (dir, "DSA", "Prefix sum (reference).md", NULL);
  g_assert_cmpstr (ref_file, ==, want_ref);
  g_assert_true (g_file_test (ref_file, G_FILE_TEST_EXISTS));
  jn_window_open_session (win, session); /* back to the note */
  g_assert_false (jn_window_in_reference (win));

  /* An error: said plainly, the old copy kept, and however long the
   * message, the page does not grow wider than the window. */
  fake.error = "Gemini is busy or over its quota; try again in a minute (Resource exhausted. Please try again later. "
               "Please refer to https://cloud.google.com/vertex-ai/generative-ai/docs/error-code-429 for more details.)";
  g_assert_true (jn_window_enhance (win));
  pump (300);
  /* The page (note, copy, status line) needs no more width than it has:
   * otherwise its right side would be cut off. */
  GtkWidget *page = gtk_widget_get_parent (gtk_widget_get_parent (GTK_WIDGET (e)));
  graphene_point_t right = GRAPHENE_POINT_INIT ((float) gtk_widget_get_width (page), 0), in_win;
  g_assert_true (gtk_widget_compute_point (page, GTK_WIDGET (win), &right, &in_win));
  g_assert_cmpfloat (in_win.x, <=, (float) gtk_widget_get_width (GTK_WIDGET (win)) + 0.5f);
  g_autofree char *kept = jn_db_get_enhanced (db, session, NULL, NULL);
  g_assert_cmpstr (kept, ==, stored);

  /* Sources are optional: a note with no links is enhanced too, and
   * nothing claims pages were read. */
  fake.error = NULL;
  jn_window_add_topic (win, "Two pointers");
  type (e, "- two indexes move toward each other");
  g_assert_true (jn_window_enhance (win));
  g_assert_cmpint (fake.calls, ==, 3);
  g_assert_cmpstr (fake.links, ==, "");
  g_assert_null (strstr (jn_window_status (win), "reading your sources"));
  g_assert_nonnull (strstr (jn_window_status (win), "asking the AI"));
  pump (300);
  g_autofree char *bare = jn_window_enhanced_text (win);
  g_assert_nonnull (bare);
  g_assert_true (g_str_has_prefix (bare, "# Two pointers (enhanced)\n"));
  g_assert_null (strstr (bare, "Sources:"));

  gtk_window_close (GTK_WINDOW (win));
  pump (100);
  g_remove (ref_file);
  g_autofree char *ref_dir = g_path_get_dirname (ref_file);
  g_rmdir (ref_dir);
  g_free (fake.note);
  g_free (fake.subject);
  g_free (fake.topic);
  g_free (fake.links);
  g_free (fake.style);
  g_remove (path);
  g_rmdir (dir);
}

/* ---- the study stopwatch and timer ------------------------------------------ */

typedef struct {
  gint64 now;              /* the stand-in clock, microseconds */
  gint64 studied, reports; /* "study": seconds handed over, and how often */
  int    finished;
} Clock;

static gint64
fake_now (gpointer user)
{
  return ((Clock *) user)->now;
}

static void
on_study (JnClockBar *b, gint64 seconds, gpointer user)
{
  (void) b;
  Clock *k = user;
  k->studied += seconds;
  k->reports++;
}

static void
on_finished (JnClockBar *b, gpointer user)
{
  (void) b;
  ((Clock *) user)->finished++;
}

static void
at (Clock *k, JnClockBar *b, double seconds)
{
  k->now = (gint64) (seconds * G_USEC_PER_SEC);
  jn_clock_bar_tick (b);
}

/* The bar alone: time shown, study time reported once, timer's end. */
static void
test_clock_bar (void)
{
  GtkWidget *w = gtk_window_new ();
  JnClockBar *b = JN_CLOCK_BAR (jn_clock_bar_new ());
  gtk_window_set_child (GTK_WINDOW (w), GTK_WIDGET (b));
  gtk_window_present (GTK_WINDOW (w));
  Clock k = { 0 };
  jn_clock_bar_set_now_func (b, fake_now, &k);
  g_signal_connect (b, "study", G_CALLBACK (on_study), &k);
  g_signal_connect (b, "finished", G_CALLBACK (on_finished), &k);

  /* Stopwatch: counts up; a minute or more is reported while running. */
  g_assert_cmpint (jn_clock_bar_mode (b), ==, JN_CLOCK_STOPWATCH);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "0:00");
  jn_clock_bar_toggle (b);
  g_assert_true (jn_clock_bar_running (b));
  at (&k, b, 59.9);
  g_assert_cmpint (k.reports, ==, 0);
  at (&k, b, 90.4);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "1:30");
  g_assert_cmpint (k.studied, ==, 90);
  at (&k, b, 100);
  jn_clock_bar_toggle (b); /* pause: the rest is reported */
  g_assert_false (jn_clock_bar_running (b));
  g_assert_cmpint (k.studied, ==, 100);
  at (&k, b, 500); /* paused: nothing moves */
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "1:40");
  jn_clock_bar_flush (b);
  g_assert_cmpint (k.studied, ==, 100);
  int reports = (int) k.reports;

  /* Reset while running reports what ran. */
  jn_clock_bar_toggle (b);
  at (&k, b, 530);
  jn_clock_bar_reset (b);
  g_assert_cmpint (k.studied, ==, 130);
  g_assert_cmpint (k.reports, ==, reports + 1);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "0:00");
  g_assert_false (jn_clock_bar_running (b));

  /* Changing to the timer while running stops and reports first. */
  jn_clock_bar_toggle (b);
  at (&k, b, 545);
  jn_clock_bar_set_mode (b, JN_CLOCK_TIMER);
  jn_clock_bar_set_minutes (b, 1);
  g_assert_cmpint (k.studied, ==, 145);
  g_assert_false (jn_clock_bar_running (b));
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "1:00");

  /* Timer: counts down, ends once at zero with all its time reported. */
  jn_clock_bar_toggle (b);
  at (&k, b, 545.5);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "1:00");
  at (&k, b, 575);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "0:30");
  at (&k, b, 700);
  g_assert_cmpint (k.finished, ==, 1);
  g_assert_false (jn_clock_bar_running (b));
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "0:00");
  g_assert_cmpint (k.studied, ==, 205);
  at (&k, b, 800);
  g_assert_cmpint (k.finished, ==, 1);
  g_assert_cmpint (k.studied, ==, 205);
  /* Start again: from the top. */
  jn_clock_bar_toggle (b);
  g_assert_true (jn_clock_bar_running (b));
  at (&k, b, 810);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "0:50");
  /* The minutes cannot change under a running timer. */
  jn_clock_bar_set_minutes (b, 5);
  at (&k, b, 811);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "0:49");
  jn_clock_bar_pause (b);
  jn_clock_bar_set_minutes (b, 5);
  g_assert_cmpstr (jn_clock_bar_text (b), ==, "5:00");
  g_assert_cmpint (k.studied, ==, 216);
  gtk_window_destroy (GTK_WINDOW (w));
}

static gint64
midnight_today (void)
{
  g_autoptr (GDateTime) n = g_date_time_new_now_local ();
  g_autoptr (GDateTime) m = g_date_time_new_local (g_date_time_get_year (n), g_date_time_get_month (n),
                                                   g_date_time_get_day_of_month (n), 0, 0, 0);
  return g_date_time_to_unix (m);
}

/* The page (note, clock, status) fits beside the sidebar (250 px) in an
 * 800-px window; any wider and its right side is cut off. */
static void
page_fits (JnWindow *win)
{
  pump (100);
  GtkWidget *page = gtk_widget_get_parent (gtk_widget_get_parent (GTK_WIDGET (jn_window_editor (win))));
  int least = 0;
  gtk_widget_measure (page, GTK_ORIENTATION_HORIZONTAL, -1, &least, NULL, NULL, NULL);
  g_assert_cmpint (least, <=, 800 - 250);
}

/* In the window: time goes to the topic it was studied on, also when the
 * topic changes with the clock running, and when Notes closes. */
static void
test_study_clock (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-clock-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  JnDb *db = jn_db_open (path, NULL);
  /* Notes' own look (its buttons are roomier than GTK's), as in the app. */
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/io/github/biswas2200/Journal/Notes/notes.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  JnWindow *win = jn_window_new (NULL, db, dir);
  gtk_window_set_default_size (GTK_WINDOW (win), 800, 600); /* a small window */
  gtk_window_present (GTK_WINDOW (win));
  pump (100);
  JnClockBar *b = jn_window_clock_bar (win);
  Clock k = { 0 };
  jn_clock_bar_set_now_func (b, fake_now, &k);

  /* Nothing to study on yet. */
  g_assert_false (gtk_widget_is_sensitive (GTK_WIDGET (b)));
  jn_window_add_subject (win, "DSA");
  gint64 heaps = jn_window_add_topic (win, "Heaps");
  g_assert_true (gtk_widget_is_sensitive (GTK_WIDGET (b)));
  g_assert_nonnull (strstr (jn_window_studied (win), "no time"));
  page_fits (win);

  jn_clock_bar_toggle (b);
  at (&k, b, 125);
  jn_window_add_topic (win, "Graphs"); /* opens it, with the clock running */
  gint64 graphs = jn_window_topic (win);
  g_assert_cmpint (jn_db_studied (db, heaps, 0), ==, 125);
  g_assert_true (jn_clock_bar_running (b));
  at (&k, b, 155);
  jn_window_open_topic (win, heaps);
  g_assert_cmpint (jn_db_studied (db, graphs, 0), ==, 30);
  g_assert_cmpint (jn_db_studied (db, heaps, midnight_today ()), ==, 125);
  g_assert_nonnull (strstr (jn_window_studied (win), "Today 2 min"));

  /* The timer's end is said in the status line. */
  jn_clock_bar_pause (b);
  jn_clock_bar_set_minutes (b, 1);
  jn_clock_bar_set_mode (b, JN_CLOCK_TIMER);
  jn_clock_bar_toggle (b);
  at (&k, b, 300);
  g_assert_nonnull (strstr (jn_window_status (win), "Time's up"));
  page_fits (win); /* the timer shows its minutes too */
  g_assert_cmpint (jn_db_studied (db, heaps, 0), ==, 185);
  g_assert_nonnull (strstr (jn_window_studied (win), "Today 3 min"));

  /* Closing Notes keeps what ran. */
  jn_clock_bar_toggle (b);
  at (&k, b, 320);
  gtk_window_close (GTK_WINDOW (win));
  pump (100);
  db = jn_db_open (path, NULL);
  g_assert_cmpint (jn_db_studied (db, heaps, 0), ==, 205);
  jn_db_close (db);
  g_remove (path);
  g_rmdir (dir);
  gtk_style_context_remove_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css));
  g_object_unref (css);
}

/* A failing store (as on a full disk) logs "SQLite: ..." warnings; they
 * are expected here and dropped. Any other warning still fails the test. */
static gboolean other_warning;

static void
drop_sqlite_warnings (const char *domain, GLogLevelFlags level, const char *message, gpointer user)
{
  (void) user;
  if (g_str_has_prefix (message, "SQLite:"))
    return;
  other_warning = TRUE;
  g_log_default_handler (domain, level, message, NULL);
}

static char *
read_file (const char *path)
{
  char *text = NULL;
  return g_file_get_contents (path, &text, NULL, NULL) ? text : NULL;
}

static void
remove_tree (const char *path)
{
  GDir *d = g_dir_open (path, 0, NULL);
  if (d != NULL)
    {
      for (const char *n = g_dir_read_name (d); n != NULL; n = g_dir_read_name (d))
        {
          g_autofree char *child = g_build_filename (path, n, NULL);
          remove_tree (child);
        }
      g_dir_close (d);
      g_rmdir (path);
    }
  else
    g_remove (path);
}

/* A save that fails never loses the text: it goes to a rescue copy at
 * once, the status says so in plain words, Notes keeps trying, and it
 * only closes once the text is on disk somewhere. */
static void
test_save_failure (void)
{
  GLogLevelFlags fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  guint quiet = g_log_set_handler ("journal", G_LOG_LEVEL_WARNING, drop_sqlite_warnings, NULL);
  g_autofree char *dir = g_dir_make_tmp ("notes-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *exports = g_build_filename (dir, "exports", NULL);
  g_autofree char *unsaved = g_build_filename (exports, "Unsaved", NULL);
  JnDb *db = jn_db_open (path, NULL);
  JnWindow *win = jn_window_new (NULL, db, exports);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);
  JnEditor *e = jn_window_editor (win);
  jn_window_add_subject (win, "DSA");
  jn_window_add_topic (win, "Heaps");
  jn_editor_focus (e);
  type (e, "# Heaps⏎- sift down");
  pump (1300);
  gint64 session = jn_window_session (win);
  g_assert_null (jn_window_rescue_path (win));

  /* The disk "fills up": the next save fails, the text goes to a rescue
   * copy straight away. */
  jn_db_set_read_only (db, TRUE);
  type (e, "⏎- O(log n)");
  pump (1300);
  g_assert_nonnull (strstr (jn_window_status (win), "Could not save"));
  g_assert_nonnull (strstr (jn_window_status (win), "Unsaved"));
  const char *rescue = jn_window_rescue_path (win);
  g_assert_nonnull (rescue);
  g_assert_true (g_str_has_prefix (rescue, unsaved));
  g_autofree char *copy = read_file (rescue);
  g_assert_nonnull (copy);
  g_assert_nonnull (strstr (copy, "- sift down\n- O(log n)"));
  g_assert_nonnull (strstr (copy, "Subject: DSA"));

  /* More typing: the same rescue file, kept up to date. */
  type (e, "⏎- heapify");
  jn_window_save (win);
  g_autofree char *rescue2 = g_strdup (jn_window_rescue_path (win));
  g_assert_cmpstr (rescue2, ==, rescue);
  g_autofree char *copy2 = read_file (rescue2);
  g_assert_nonnull (strstr (copy2, "- O(log n)\n- heapify"));

  /* Nowhere for a rescue copy either: Notes will not close. */
  g_autofree char *moved = g_strconcat (exports, ".away", NULL);
  g_assert_cmpint (g_rename (exports, moved), ==, 0);
  g_assert_true (g_file_set_contents (exports, "a file where the folder was", -1, NULL));
  type (e, "⏎- last");
  g_object_add_weak_pointer (G_OBJECT (win), (gpointer *) &win);
  gtk_window_close (GTK_WINDOW (win));
  pump (300);
  g_assert_nonnull (win);
  g_assert_nonnull (strstr (jn_window_status (win), "not saved"));

  /* The disk has room again: the next try saves everything, and now it
   * closes. */
  jn_db_set_read_only (db, FALSE);
  gtk_window_close (GTK_WINDOW (win));
  pump (300);
  g_assert_null (win);
  db = jn_db_open (path, NULL);
  JnSession s = { 0 };
  g_assert_true (jn_db_get_session (db, session, &s));
  g_assert_cmpstr (s.body, ==, "# Heaps\n- sift down\n- O(log n)\n- heapify\n- last");
  jn_session_clear (&s);
  jn_db_close (db);

  /* Closing while saves fail, with room for the rescue copy: it closes,
   * the text is in the copy. */
  g_remove (exports);
  g_assert_cmpint (g_rename (moved, exports), ==, 0);
  db = jn_db_open (path, NULL);
  win = jn_window_new (NULL, db, exports);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);
  e = jn_window_editor (win);
  jn_window_open_session (win, session);
  jn_db_set_read_only (db, TRUE);
  jn_editor_focus (e);
  type (e, "⏎- closing now");
  g_object_add_weak_pointer (G_OBJECT (win), (gpointer *) &win);
  gtk_window_close (GTK_WINDOW (win));
  pump (300);
  g_assert_null (win);
  g_autoptr (GDir) list = g_dir_open (unsaved, 0, NULL);
  gboolean found = FALSE;
  for (const char *n = g_dir_read_name (list); n != NULL; n = g_dir_read_name (list))
    {
      g_autofree char *f = g_build_filename (unsaved, n, NULL);
      g_autofree char *text = read_file (f);
      found |= text != NULL && strstr (text, "- last\n- closing now") != NULL;
    }
  g_assert_true (found);

  remove_tree (dir);
  g_log_remove_handler ("journal", quiet);
  g_log_set_always_fatal (fatal);
  g_assert_false (other_warning);
}

/* Backups: one when Notes opens, the day's copy brought up to date when
 * it closes. */
static void
test_backups (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-ui-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *exports = g_build_filename (dir, "exports", NULL);
  g_autofree char *backups = g_build_filename (dir, "backups", NULL);
  JnDb *db = jn_db_open (path, NULL);
  JnWindow *win = jn_window_new (NULL, db, exports);
  jn_window_set_backup_dir (win, backups);
  g_autofree char *latest = jn_backup_latest (backups);
  g_assert_nonnull (latest);
  gtk_window_present (GTK_WINDOW (win));
  pump (200);
  JnEditor *e = jn_window_editor (win);
  jn_window_add_subject (win, "OS");
  jn_window_add_topic (win, "Paging");
  jn_editor_focus (e);
  type (e, "- page table");
  gint64 session = jn_window_session (win);
  g_object_add_weak_pointer (G_OBJECT (win), (gpointer *) &win);
  gtk_window_close (GTK_WINDOW (win));
  pump (300);
  g_assert_null (win);
  JnDb *copy = jn_db_open (latest, NULL);
  g_assert_nonnull (copy);
  JnSession s = { 0 };
  g_assert_true (jn_db_get_session (copy, session, &s));
  g_assert_cmpstr (s.body, ==, "- page table");
  jn_session_clear (&s);
  jn_db_close (copy);
  remove_tree (dir);
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
  g_test_add_func ("/notes-ui/markdown-preview", test_markdown_preview);
  g_test_add_func ("/notes-ui/window-flow", test_window_flow);
  g_test_add_func ("/notes-ui/enhance", test_enhance);
  g_test_add_func ("/notes-ui/md-view", test_md_view);
  g_test_add_func ("/notes-ui/clock-bar", test_clock_bar);
  g_test_add_func ("/notes-ui/study-clock", test_study_clock);
  g_test_add_func ("/notes-ui/save-failure", test_save_failure);
  g_test_add_func ("/notes-ui/backups", test_backups);
  return g_test_run ();
}
