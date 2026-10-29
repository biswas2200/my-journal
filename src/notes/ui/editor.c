/* editor: see editor.h. */
#include "editor.h"

#include <string.h>

#define H1_MARK "\xe2\x81\xa3" /* U+2063, invisible: marks a heading */
#define H2_MARK "\xe2\x81\xa4" /* U+2064, invisible: marks a subheading */
#define H1_CHAR 0x2063
#define H2_CHAR 0x2064

struct _JnEditor {
  GtkBox         parent_instance;
  GtkTextView   *view, *sources;
  GtkTextBuffer *buf, *src;
  GtkTextTag    *prefix, *marker, *glyph;           /* on the prefix */
  GtkTextTag    *h1, *h2, *bullet, *sub, *arrow;    /* on the whole line */
  gboolean       markdown;
  gboolean       loading;
  int            busy;        /* inside our own edits: no cursor fixing */
  int            dirty_first, dirty_last;
  guint          restyle_id;
  int            last_cursor; /* offset, to tell moving left from right */
};

G_DEFINE_FINAL_TYPE (JnEditor, jn_editor, GTK_TYPE_BOX)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- line kinds ----------------------------------------------------- */

static const char *
prefix_text (JnKind k)
{
  switch (k)
    {
    case JN_HEADING:    return H1_MARK;
    case JN_SUBHEADING: return H2_MARK;
    default:            return jn_kind_glyph (k);
    }
}

static int
prefix_chars (JnKind k)
{
  return (int) g_utf8_strlen (prefix_text (k), -1);
}

static void
line_start (JnEditor *e, int line, GtkTextIter *it)
{
  gtk_text_buffer_get_iter_at_line (e->buf, it, line);
}

static void
line_end (JnEditor *e, int line, GtkTextIter *it)
{
  line_start (e, line, it);
  if (!gtk_text_iter_ends_line (it))
    gtk_text_iter_forward_to_line_end (it);
}

/* Only a tagged prefix counts: typed "●" is text. */
static JnKind
line_kind (JnEditor *e, int line)
{
  GtkTextIter it;
  line_start (e, line, &it);
  if (!gtk_text_iter_has_tag (&it, e->prefix))
    return JN_TEXT;
  switch (gtk_text_iter_get_char (&it))
    {
    case H1_CHAR:  return JN_HEADING;
    case H2_CHAR:  return JN_SUBHEADING;
    case 0x25CF:   return JN_BULLET;      /* ● */
    case 0x25E6:   return JN_SUB_BULLET;  /* ◦ */
    case 0x2192:   return JN_ARROW;       /* → */
    default:       return JN_TEXT;
    }
}

/* Where the line's own text starts (after its prefix). */
static void
text_start (JnEditor *e, int line, GtkTextIter *it)
{
  JnKind k = line_kind (e, line);
  line_start (e, line, it);
  if (k != JN_TEXT)
    gtk_text_iter_forward_chars (it, prefix_chars (k));
}

static char *
line_text (JnEditor *e, int line)
{
  GtkTextIter a, b;
  text_start (e, line, &a);
  line_end (e, line, &b);
  char *t = gtk_text_buffer_get_text (e->buf, &a, &b, TRUE);
  /* Our invisible markers never belong to the text itself. */
  GString *s = g_string_new (NULL);
  for (const char *p = t; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c != H1_CHAR && c != H2_CHAR)
        g_string_append_unichar (s, c);
    }
  g_free (t);
  return g_string_free (s, FALSE);
}

static void
restyle_line (JnEditor *e, int line)
{
  GtkTextIter a, b;
  line_start (e, line, &a);
  b = a;
  gtk_text_iter_forward_line (&b); /* include the newline: margins apply to the paragraph */
  GtkTextTag *style[] = { e->h1, e->h2, e->bullet, e->sub, e->arrow };
  for (gsize i = 0; i < G_N_ELEMENTS (style); i++)
    gtk_text_buffer_remove_tag (e->buf, style[i], &a, &b);
  GtkTextTag *t = NULL;
  switch (line_kind (e, line))
    {
    case JN_HEADING:    t = e->h1; break;
    case JN_SUBHEADING: t = e->h2; break;
    case JN_BULLET:     t = e->bullet; break;
    case JN_SUB_BULLET: t = e->sub; break;
    case JN_ARROW:      t = e->arrow; break;
    default:            break;
    }
  if (t != NULL)
    gtk_text_buffer_apply_tag (e->buf, t, &a, &b);
}

static void
restyle_lines (JnEditor *e, int first, int last)
{
  int n = gtk_text_buffer_get_line_count (e->buf);
  for (int l = MAX (first, 0); l <= MIN (last, n - 1); l++)
    restyle_line (e, l);
}

static gboolean
restyle_idle (gpointer data)
{
  JnEditor *e = data;
  e->restyle_id = 0;
  if (!e->markdown && e->dirty_first >= 0)
    restyle_lines (e, e->dirty_first, e->dirty_last);
  e->dirty_first = e->dirty_last = -1;
  return G_SOURCE_REMOVE;
}

/* Styles follow edits from a high-priority idle (before the next frame
 * and the next key), as tags cannot change inside the insert handlers. */
static void
mark_dirty (JnEditor *e, int first, int last)
{
  e->dirty_first = e->dirty_first < 0 ? first : MIN (e->dirty_first, first);
  e->dirty_last = MAX (e->dirty_last, last);
  if (e->restyle_id == 0)
    e->restyle_id = g_idle_add_full (G_PRIORITY_HIGH, restyle_idle, e, NULL);
}

/* Replaces the line's prefix with the one for `kind`. */
static void
set_line_kind (JnEditor *e, int line, JnKind kind)
{
  e->busy++;
  JnKind old = line_kind (e, line);
  GtkTextIter a, b;
  if (old != JN_TEXT)
    {
      line_start (e, line, &a);
      b = a;
      gtk_text_iter_forward_chars (&b, prefix_chars (old));
      gtk_text_buffer_delete (e->buf, &a, &b);
    }
  if (kind != JN_TEXT)
    {
      line_start (e, line, &a);
      GtkTextTag *look = kind == JN_HEADING || kind == JN_SUBHEADING ? e->marker : e->glyph;
      gtk_text_buffer_insert_with_tags (e->buf, &a, prefix_text (kind), -1, e->prefix, look, NULL);
    }
  restyle_line (e, line);
  e->busy--;
}

static int
cursor_line (JnEditor *e, GtkTextIter *cursor)
{
  gtk_text_buffer_get_iter_at_mark (e->buf, cursor, gtk_text_buffer_get_insert (e->buf));
  return gtk_text_iter_get_line (cursor);
}

static void
place_at_text_start (JnEditor *e, int line)
{
  GtkTextIter it;
  text_start (e, line, &it);
  gtk_text_buffer_place_cursor (e->buf, &it);
}

/* ---- markdown mode: the raw stored lines --------------------------- */

static char *
raw_line (JnEditor *e, int line)
{
  GtkTextIter a, b;
  line_start (e, line, &a);
  line_end (e, line, &b);
  return gtk_text_buffer_get_text (e->buf, &a, &b, TRUE);
}

static void
replace_raw_line (JnEditor *e, int line, const char *text)
{
  GtkTextIter a, b;
  line_start (e, line, &a);
  line_end (e, line, &b);
  gtk_text_buffer_delete (e->buf, &a, &b);
  line_start (e, line, &a);
  gtk_text_buffer_insert (e->buf, &a, text, -1);
}

/* ---- public ----------------------------------------------------------- */

JnKind
jn_editor_kind_at_cursor (JnEditor *e)
{
  GtkTextIter c;
  int line = cursor_line (e, &c);
  if (!e->markdown)
    return line_kind (e, line);
  g_autofree char *raw = raw_line (e, line);
  const char *text;
  return jn_line_parse (raw, &text);
}

void
jn_editor_set_kind (JnEditor *e, JnKind kind)
{
  GtkTextIter a, b;
  gtk_text_buffer_get_selection_bounds (e->buf, &a, &b);
  int first = gtk_text_iter_get_line (&a), last = gtk_text_iter_get_line (&b);
  if (last > first && gtk_text_iter_starts_line (&b))
    last--; /* a selection ending at a line start leaves that line */
  gtk_text_buffer_begin_user_action (e->buf);
  for (int l = first; l <= last; l++)
    {
      if (e->markdown)
        {
          g_autofree char *raw = raw_line (e, l);
          const char *text;
          jn_line_parse (raw, &text);
          g_autofree char *t = g_strdup (text);
          g_autofree char *line = jn_line_format (kind, t);
          replace_raw_line (e, l, line);
        }
      else
        set_line_kind (e, l, kind);
    }
  gtk_text_buffer_end_user_action (e->buf);
  if (first == last)
    {
      /* One line: keep writing at its end. */
      GtkTextIter c;
      line_end (e, first, &c);
      e->busy++;
      gtk_text_buffer_place_cursor (e->buf, &c);
      e->busy--;
    }
}

void
jn_editor_load (JnEditor *e, const char *body, const char *sources)
{
  e->loading = TRUE;
  e->busy++;
  gtk_text_buffer_set_enable_undo (e->buf, FALSE);
  gtk_text_buffer_set_text (e->buf, "", 0);
  if (e->markdown)
    gtk_text_buffer_set_text (e->buf, body != NULL ? body : "", -1);
  else
    {
      g_auto (GStrv) lines = g_strsplit (body != NULL ? body : "", "\n", -1);
      GtkTextIter end;
      for (int i = 0; lines[i] != NULL; i++)
        {
          const char *text;
          JnKind k = jn_line_parse (lines[i], &text);
          gtk_text_buffer_get_end_iter (e->buf, &end);
          if (i > 0)
            {
              gtk_text_buffer_insert (e->buf, &end, "\n", 1);
              gtk_text_buffer_get_end_iter (e->buf, &end);
            }
          if (k != JN_TEXT)
            {
              GtkTextTag *look = k == JN_HEADING || k == JN_SUBHEADING ? e->marker : e->glyph;
              gtk_text_buffer_insert_with_tags (e->buf, &end, prefix_text (k), -1, e->prefix, look, NULL);
              gtk_text_buffer_get_end_iter (e->buf, &end);
            }
          gtk_text_buffer_insert (e->buf, &end, text, -1);
        }
      restyle_lines (e, 0, gtk_text_buffer_get_line_count (e->buf) - 1);
    }
  gtk_text_buffer_set_text (e->src, sources != NULL ? sources : "", -1);
  gtk_text_buffer_set_enable_undo (e->buf, TRUE);
  GtkTextIter start;
  gtk_text_buffer_get_start_iter (e->buf, &start);
  gtk_text_buffer_place_cursor (e->buf, &start);
  e->busy--;
  e->loading = FALSE;
  if (!e->markdown)
    place_at_text_start (e, 0);
}

char *
jn_editor_body (JnEditor *e)
{
  if (e->markdown)
    {
      GtkTextIter a, b;
      gtk_text_buffer_get_bounds (e->buf, &a, &b);
      return gtk_text_buffer_get_text (e->buf, &a, &b, TRUE);
    }
  GString *s = g_string_new (NULL);
  int n = gtk_text_buffer_get_line_count (e->buf);
  for (int l = 0; l < n; l++)
    {
      g_autofree char *text = line_text (e, l);
      g_autofree char *line = jn_line_format (line_kind (e, l), text);
      if (l > 0)
        g_string_append_c (s, '\n');
      g_string_append (s, line);
    }
  return g_string_free (s, FALSE);
}

char *
jn_editor_sources (JnEditor *e)
{
  GtkTextIter a, b;
  gtk_text_buffer_get_bounds (e->src, &a, &b);
  return gtk_text_buffer_get_text (e->src, &a, &b, TRUE);
}

void
jn_editor_set_markdown (JnEditor *e, gboolean on)
{
  if (e->markdown == on)
    return;
  g_autofree char *body = jn_editor_body (e);
  g_autofree char *sources = jn_editor_sources (e);
  e->markdown = on;
  if (on)
    gtk_widget_add_css_class (GTK_WIDGET (e->view), "markdown");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (e->view), "markdown");
  jn_editor_load (e, body, sources);
}

gboolean
jn_editor_get_markdown (JnEditor *e)
{
  return e->markdown;
}

void
jn_editor_cursor_to_end (JnEditor *e)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (e->buf, &end);
  gtk_text_buffer_place_cursor (e->buf, &end);
}

void
jn_editor_set_editable (JnEditor *e, gboolean editable)
{
  gtk_text_view_set_editable (e->view, editable);
  gtk_text_view_set_cursor_visible (e->view, editable);
  gtk_text_view_set_editable (e->sources, editable);
}

void
jn_editor_focus (JnEditor *e)
{
  gtk_widget_grab_focus (GTK_WIDGET (e->view));
}

GtkTextView *
jn_editor_view (JnEditor *e)
{
  return e->view;
}

GtkTextView *
jn_editor_sources_view (JnEditor *e)
{
  return e->sources;
}

/* ---- keys ------------------------------------------------------------ */

static gboolean
is_list (JnKind k)
{
  return k == JN_BULLET || k == JN_SUB_BULLET || k == JN_ARROW;
}

gboolean
jn_editor_key (JnEditor *e, guint keyval, GdkModifierType state)
{
  if (e->markdown || !gtk_text_view_get_editable (e->view) ||
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0)
    return FALSE;
  GtkTextIter c, ts;
  int line = cursor_line (e, &c);
  JnKind kind = line_kind (e, line);
  gboolean selection = gtk_text_buffer_get_has_selection (e->buf);
  text_start (e, line, &ts);
  gboolean at_text_start = gtk_text_iter_equal (&c, &ts);

  switch (keyval)
    {
    case GDK_KEY_space:
      {
        if (selection)
          return FALSE;
        /* A marker typed on a plain line, or in an empty item of a list
         * (Enter continued an arrow, now "## " makes a heading). */
        GtkTextIter a;
        if (kind == JN_TEXT)
          line_start (e, line, &a);
        else
          a = ts;
        g_autofree char *typed = gtk_text_buffer_get_text (e->buf, &a, &c, TRUE);
        g_autofree char *candidate = g_strconcat (typed, " ", NULL);
        gsize n = 0;
        JnKind k = jn_line_shortcut (candidate, &n);
        if (k == JN_TEXT)
          return FALSE;
        if (kind != JN_TEXT)
          {
            /* Only in an item with nothing else in it. */
            GtkTextIter end;
            line_end (e, line, &end);
            if (!gtk_text_iter_equal (&c, &end))
              return FALSE;
          }
        gtk_text_buffer_begin_user_action (e->buf);
        gtk_text_buffer_delete (e->buf, &a, &c);
        set_line_kind (e, line, k);
        gtk_text_buffer_end_user_action (e->buf);
        place_at_text_start (e, line);
        return TRUE;
      }
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      {
        if (selection || kind == JN_TEXT || (state & GDK_SHIFT_MASK) != 0)
          return FALSE;
        g_autofree char *text = line_text (e, line);
        if (at_text_start && *text != '\0')
          {
            /* Enter before a heading's or an item's text: an empty line
             * above, the line itself stays as it is. */
            GtkTextIter a;
            line_start (e, line, &a);
            e->busy++;
            gtk_text_buffer_insert (e->buf, &a, "\n", 1);
            e->busy--;
            mark_dirty (e, line, line + 1);
            place_at_text_start (e, line + 1);
            return TRUE;
          }
        if (!is_list (kind))
          return FALSE;
        gtk_text_buffer_begin_user_action (e->buf);
        if (*text == '\0')
          {
            set_line_kind (e, line, JN_TEXT); /* Enter on an empty item ends the list */
            gtk_text_buffer_end_user_action (e->buf);
            place_at_text_start (e, line);
            return TRUE;
          }
        e->busy++;
        gtk_text_buffer_insert (e->buf, &c, "\n", 1);
        e->busy--;
        set_line_kind (e, line + 1, kind);
        gtk_text_buffer_end_user_action (e->buf);
        place_at_text_start (e, line + 1);
        return TRUE;
      }
    case GDK_KEY_BackSpace:
      if (selection || kind == JN_TEXT || !at_text_start)
        return FALSE;
      gtk_text_buffer_begin_user_action (e->buf);
      set_line_kind (e, line, kind == JN_SUB_BULLET ? JN_BULLET : JN_TEXT);
      gtk_text_buffer_end_user_action (e->buf);
      place_at_text_start (e, line);
      return TRUE;
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab:
      {
        if (!is_list (kind))
          return FALSE;
        gboolean back = keyval == GDK_KEY_ISO_Left_Tab || (state & GDK_SHIFT_MASK) != 0;
        JnKind k = back ? jn_kind_outdent (kind) : jn_kind_indent (kind);
        GtkTextIter end;
        line_end (e, line, &end);
        int from_end = gtk_text_iter_get_offset (&end) - gtk_text_iter_get_offset (&c);
        gtk_text_buffer_begin_user_action (e->buf);
        set_line_kind (e, line, k);
        gtk_text_buffer_end_user_action (e->buf);
        /* The cursor keeps its place in the text. */
        line_end (e, line, &end);
        gtk_text_iter_backward_chars (&end, from_end);
        gtk_text_buffer_place_cursor (e->buf, &end);
        return TRUE;
      }
    default:
      return FALSE;
    }
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  (void) c; (void) keycode;
  return jn_editor_key (data, keyval, state);
}

/* ---- buffer signals ---------------------------------------------------- */

static void
after_insert (GtkTextBuffer *buf, GtkTextIter *at, const char *text, int len, gpointer data)
{
  (void) buf;
  JnEditor *e = data;
  int last = gtk_text_iter_get_line (at), lines = 0;
  for (int i = 0; i < len; i++)
    lines += text[i] == '\n';
  mark_dirty (e, last - lines, last);
}

static void
after_delete (GtkTextBuffer *buf, GtkTextIter *a, GtkTextIter *b, gpointer data)
{
  (void) buf; (void) b;
  JnEditor *e = data;
  int l = gtk_text_iter_get_line (a);
  mark_dirty (e, l, l);
}

/* The cursor never rests inside a prefix: moving into one from the right
 * goes on to the end of the line above; anything else lands at the start
 * of the text. */
static void
on_mark_set (GtkTextBuffer *buf, GtkTextIter *where, GtkTextMark *mark, gpointer data)
{
  JnEditor *e = data;
  if (mark != gtk_text_buffer_get_insert (buf) || e->busy > 0 || e->markdown ||
      gtk_text_buffer_get_has_selection (buf))
    return;
  int offset = gtk_text_iter_get_offset (where), line = gtk_text_iter_get_line (where);
  GtkTextIter ts;
  text_start (e, line, &ts);
  int start = gtk_text_iter_get_offset (&ts);
  int was = e->last_cursor;
  e->last_cursor = offset;
  if (offset >= start)
    return;
  e->busy++;
  if (was == start && line > 0)
    {
      GtkTextIter up;
      line_end (e, line - 1, &up);
      gtk_text_buffer_place_cursor (buf, &up);
      e->last_cursor = gtk_text_iter_get_offset (&up);
    }
  else
    {
      gtk_text_buffer_place_cursor (buf, &ts);
      e->last_cursor = start;
    }
  e->busy--;
}

static void
on_changed (GtkTextBuffer *buf, gpointer data)
{
  (void) buf;
  JnEditor *e = data;
  if (!e->loading)
    g_signal_emit (e, signals[CHANGED], 0);
}

/* ---- the widget -------------------------------------------------------- */

static void
on_tool (GtkButton *b, gpointer data)
{
  JnEditor *e = data;
  jn_editor_set_kind (e, (JnKind) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "jn-kind")));
  jn_editor_focus (e);
}

static GtkWidget *
tool (JnEditor *e, const char *label, JnKind kind, const char *tip)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, "tool");
  gtk_widget_set_tooltip_text (b, tip);
  gtk_widget_set_focus_on_click (b, FALSE);
  g_object_set_data (G_OBJECT (b), "jn-kind", GINT_TO_POINTER (kind));
  g_signal_connect (b, "clicked", G_CALLBACK (on_tool), e);
  return b;
}

static void
jn_editor_dispose (GObject *object)
{
  JnEditor *e = JN_EDITOR (object);
  g_clear_handle_id (&e->restyle_id, g_source_remove);
  G_OBJECT_CLASS (jn_editor_parent_class)->dispose (object);
}

static void
jn_editor_class_init (JnEditorClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = jn_editor_dispose;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
jn_editor_init (JnEditor *e)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (e), GTK_ORIENTATION_VERTICAL);
  e->dirty_first = e->dirty_last = -1;

  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (bar, "note-tools");
  gtk_box_append (GTK_BOX (bar), tool (e, "H", JN_HEADING, "Heading (Ctrl+1, or type \"# \")"));
  gtk_box_append (GTK_BOX (bar), tool (e, "h", JN_SUBHEADING, "Subheading (Ctrl+2, or type \"## \")"));
  gtk_box_append (GTK_BOX (bar), tool (e, "\xe2\x97\x8f", JN_BULLET, "Bullet (Ctrl+., or type \"- \")"));
  gtk_box_append (GTK_BOX (bar), tool (e, "\xe2\x97\xa6", JN_SUB_BULLET, "Sub-bullet (Tab on a bullet)"));
  gtk_box_append (GTK_BOX (bar), tool (e, "\xe2\x86\x92", JN_ARROW, "Arrow (Ctrl+-, or type \"> \")"));
  gtk_box_append (GTK_BOX (bar), tool (e, "\xc2\xb6", JN_TEXT, "Plain text (Ctrl+0)"));
  gtk_box_append (GTK_BOX (e), bar);

  GtkWidget *src_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (src_box, "sources");
  GtkWidget *src_label = gtk_label_new ("Sources: the links you study from, one per line");
  gtk_label_set_xalign (GTK_LABEL (src_label), 0.0f);
  gtk_widget_add_css_class (src_label, "sources-title");
  gtk_box_append (GTK_BOX (src_box), src_label);
  e->sources = GTK_TEXT_VIEW (gtk_text_view_new ());
  e->src = gtk_text_view_get_buffer (e->sources);
  gtk_text_view_set_wrap_mode (e->sources, GTK_WRAP_CHAR);
  gtk_text_view_set_accepts_tab (e->sources, FALSE);
  gtk_widget_add_css_class (GTK_WIDGET (e->sources), "sources-text");
  gtk_accessible_update_property (GTK_ACCESSIBLE (e->sources), GTK_ACCESSIBLE_PROPERTY_LABEL, "Sources", -1);
  gtk_box_append (GTK_BOX (src_box), GTK_WIDGET (e->sources));
  /* A click anywhere in the box (its title too) is a click in the links. */
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect_swapped (click, "pressed", G_CALLBACK (gtk_widget_grab_focus), e->sources);
  gtk_widget_add_controller (src_box, GTK_EVENT_CONTROLLER (click));
  gtk_box_append (GTK_BOX (e), src_box);

  e->view = GTK_TEXT_VIEW (gtk_text_view_new ());
  e->buf = gtk_text_view_get_buffer (e->view);
  gtk_text_view_set_wrap_mode (e->view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (e->view, FALSE);
  gtk_text_view_set_left_margin (e->view, 28);
  gtk_text_view_set_right_margin (e->view, 28);
  gtk_text_view_set_top_margin (e->view, 16);
  gtk_text_view_set_bottom_margin (e->view, 40);
  gtk_text_view_set_pixels_below_lines (e->view, 6);
  gtk_text_view_set_pixels_inside_wrap (e->view, 4);
  gtk_widget_add_css_class (GTK_WIDGET (e->view), "note-body");
  gtk_accessible_update_property (GTK_ACCESSIBLE (e->view), GTK_ACCESSIBLE_PROPERTY_LABEL, "Note", -1);

  e->prefix = gtk_text_buffer_create_tag (e->buf, "jn-prefix", NULL);
  e->marker = gtk_text_buffer_create_tag (e->buf, "jn-marker", "invisible", TRUE, NULL);
  e->glyph = gtk_text_buffer_create_tag (e->buf, "jn-glyph", "foreground", "#9B9B9B", NULL);
  e->h1 = gtk_text_buffer_create_tag (e->buf, "jn-h1", "scale", 1.55, "weight", PANGO_WEIGHT_BOLD,
                                      "pixels-above-lines", 14, NULL);
  e->h2 = gtk_text_buffer_create_tag (e->buf, "jn-h2", "scale", 1.25, "weight", PANGO_WEIGHT_BOLD,
                                      "pixels-above-lines", 10, NULL);
  e->bullet = gtk_text_buffer_create_tag (e->buf, "jn-bullet", "left-margin", 44, "indent", -16, NULL);
  e->sub = gtk_text_buffer_create_tag (e->buf, "jn-sub", "left-margin", 72, "indent", -16, NULL);
  e->arrow = gtk_text_buffer_create_tag (e->buf, "jn-arrow", "left-margin", 46, "indent", -18, NULL);

  g_signal_connect_after (e->buf, "insert-text", G_CALLBACK (after_insert), e);
  g_signal_connect_after (e->buf, "delete-range", G_CALLBACK (after_delete), e);
  g_signal_connect (e->buf, "mark-set", G_CALLBACK (on_mark_set), e);
  g_signal_connect (e->buf, "changed", G_CALLBACK (on_changed), e);
  g_signal_connect (e->src, "changed", G_CALLBACK (on_changed), e);

  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), e);
  gtk_widget_add_controller (GTK_WIDGET (e->view), keys);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (e->view));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (e), scroll);
}

GtkWidget *
jn_editor_new (void)
{
  return g_object_new (JN_TYPE_EDITOR, NULL);
}
