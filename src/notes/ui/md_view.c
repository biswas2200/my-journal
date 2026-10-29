/* md_view: see md_view.h. */
#include "md_view.h"

#include <string.h>
#include <librsvg/rsvg.h>
#include "markdown.h"
#include "svgsafe.h"

#define DIAGRAM_MAX_WIDTH 620

struct _JnMdView {
  GtkTextView parent_instance;
  char       *markdown;
  int         diagrams, refused;
  GtkTextTag *h[4], *bold, *italic, *code, *block, *label, *cite, *link, *rule, *note;
  GtkTextTag *depth[5];
  int         wrapped_width; /* the width the text was last wrapped for */
  guint       rewrap_id;
  GPtrArray  *pictures;      /* the diagrams on screen (not owned) */
};

G_DEFINE_FINAL_TYPE (JnMdView, jn_md_view, GTK_TYPE_TEXT_VIEW)

static GtkTextBuffer *
buf (JnMdView *v)
{
  return gtk_text_view_get_buffer (GTK_TEXT_VIEW (v));
}

static void
put (JnMdView *v, const char *text, GtkTextTag *a, GtkTextTag *b)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buf (v), &end);
  gtk_text_buffer_insert_with_tags (buf (v), &end, text, -1, a, b, NULL);
}

/* One line's inline spans; `line` is the block's tag (heading, depth). */
static void
put_spans (JnMdView *v, const char *text, GtkTextTag *line)
{
  GPtrArray *spans = jn_md_spans (text);
  for (guint i = 0; i < spans->len; i++)
    {
      JnMdSpan *s = spans->pdata[i];
      GtkTextIter end;
      gtk_text_buffer_get_end_iter (buf (v), &end);
      switch (s->kind)
        {
        case JN_SPAN_BOLD:   put (v, s->text, line, v->bold); break;
        case JN_SPAN_ITALIC: put (v, s->text, line, v->italic); break;
        case JN_SPAN_CODE:   put (v, s->text, line, v->code); break;
        case JN_SPAN_CITE:
          {
            g_autofree char *c = g_strdup_printf ("[%s]", s->text);
            put (v, c, line, v->cite);
            break;
          }
        case JN_SPAN_LINK:
          {
            /* One tag per link holds its address. */
            GtkTextTag *t = gtk_text_buffer_create_tag (buf (v), NULL, NULL);
            g_object_set_data_full (G_OBJECT (t), "jn-url", g_strdup (s->url), g_free);
            int start = gtk_text_iter_get_offset (&end);
            put (v, s->text, line, v->link);
            GtkTextIter a, b;
            gtk_text_buffer_get_iter_at_offset (buf (v), &a, start);
            gtk_text_buffer_get_end_iter (buf (v), &b);
            gtk_text_buffer_apply_tag (buf (v), t, &a, &b);
            break;
          }
        case JN_SPAN_TEXT:
        default:
          put (v, s->text, line, NULL);
          break;
        }
    }
  g_ptr_array_unref (spans);
}

/* A cleaned SVG drawn into a texture, at most DIAGRAM_MAX_WIDTH wide. */
static GdkTexture *
draw_svg (const char *clean)
{
  g_autoptr (GError) error = NULL;
  RsvgHandle *h = rsvg_handle_new_from_data ((const guint8 *) clean, strlen (clean), &error);
  if (h == NULL)
    return NULL;
  double w = 0, ht = 0;
  if (!rsvg_handle_get_intrinsic_size_in_pixels (h, &w, &ht) || w < 1 || ht < 1)
    {
      w = 600;
      ht = 300;
    }
  double scale = w > DIAGRAM_MAX_WIDTH ? DIAGRAM_MAX_WIDTH / w : 1.0;
  int W = MAX (1, (int) (w * scale)), H = MAX (1, (int) (ht * scale));
  if (H > 2000)
    H = 2000;
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, W, H);
  cairo_t *cr = cairo_create (surface);
  RsvgRectangle viewport = { 0, 0, W, H };
  gboolean ok = rsvg_handle_render_document (h, cr, &viewport, &error);
  cairo_destroy (cr);
  g_object_unref (h);
  GdkTexture *tex = NULL;
  if (ok)
    {
      cairo_surface_flush (surface);
      int stride = cairo_image_surface_get_stride (surface);
      GBytes *bytes = g_bytes_new (cairo_image_surface_get_data (surface), (gsize) stride * (gsize) H);
      tex = gdk_memory_texture_new (W, H, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
      g_bytes_unref (bytes);
    }
  cairo_surface_destroy (surface);
  return tex;
}

static void
put_svg (JnMdView *v, const char *svg)
{
  g_autofree char *clean = jn_svg_clean (svg, -1);
  g_autoptr (GdkTexture) tex = clean != NULL ? draw_svg (clean) : NULL;
  if (tex == NULL)
    {
      v->refused++;
      put (v, "(A diagram is not shown: it did not pass the safety check.)\n", v->note, NULL);
      return;
    }
  v->diagrams++;
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buf (v), &end);
  GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor (buf (v), &end);
  GtkWidget *pic = gtk_picture_new_for_paintable (GDK_PAINTABLE (tex));
  /* Sized by fit_pictures: never wider than the view. */
  gtk_picture_set_can_shrink (GTK_PICTURE (pic), TRUE);
  gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_CONTAIN);
  g_ptr_array_add (v->pictures, pic);
  gtk_widget_add_css_class (pic, "diagram");
  gtk_widget_set_halign (pic, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pic), GTK_ACCESSIBLE_PROPERTY_LABEL, "Diagram", -1);
  gtk_text_view_add_child_at_anchor (GTK_TEXT_VIEW (v), pic, anchor);
  put (v, "\n", NULL, NULL);
}

/* Each diagram as wide as it is drawn, but never wider than the view
 * (less its margins); the height keeps the shape. */
static void
fit_pictures (JnMdView *v)
{
  int avail = gtk_widget_get_width (GTK_WIDGET (v)) - gtk_text_view_get_left_margin (GTK_TEXT_VIEW (v)) -
              gtk_text_view_get_right_margin (GTK_TEXT_VIEW (v)) - 48;
  for (guint i = 0; i < v->pictures->len; i++)
    {
      GtkWidget *pic = v->pictures->pdata[i];
      GdkPaintable *p = gtk_picture_get_paintable (GTK_PICTURE (pic));
      int w = gdk_paintable_get_intrinsic_width (p), h = gdk_paintable_get_intrinsic_height (p);
      if (avail > 40 && w > avail)
        {
          h = h * avail / w;
          w = avail;
        }
      gtk_widget_set_size_request (pic, w, h);
    }
}

void
jn_md_view_set_markdown (JnMdView *v, const char *markdown)
{
  g_free (v->markdown);
  v->markdown = g_strdup (markdown != NULL ? markdown : "");
  v->diagrams = v->refused = 0;
  g_ptr_array_set_size (v->pictures, 0);
  gtk_text_buffer_set_text (buf (v), "", 0);
  GPtrArray *blocks = jn_md_blocks (v->markdown);
  for (guint i = 0; i < blocks->len; i++)
    {
      JnMdBlock *b = blocks->pdata[i];
      switch (b->kind)
        {
        case JN_MD_HEADING:
          put_spans (v, b->text, v->h[MIN (b->level, 4) - 1]);
          break;
        case JN_MD_BULLET:
          {
            put (v, b->level == 0 ? "\xe2\x97\x8f " : "\xe2\x97\xa6 ", v->depth[b->level], NULL);
            put_spans (v, b->text, v->depth[b->level]);
            break;
          }
        case JN_MD_ARROW:
          put (v, "\xe2\x86\x92 ", v->depth[b->level], NULL);
          put_spans (v, b->text, v->depth[b->level]);
          break;
        case JN_MD_NUMBERED:
          {
            g_autofree char *n = g_strdup_printf ("%d. ", b->level);
            put (v, n, v->depth[0], NULL);
            put_spans (v, b->text, v->depth[0]);
            break;
          }
        case JN_MD_CODE:
          if (b->lang != NULL)
            {
              put (v, b->lang, v->label, NULL);
              put (v, "\n", v->label, NULL);
            }
          put (v, b->text, v->block, NULL);
          break;
        case JN_MD_SVG:
          put_svg (v, b->text);
          continue; /* it ends its own line */
        case JN_MD_RULE:
          put (v, "\xe2\x80\x94\xe2\x80\x94\xe2\x80\x94", v->rule, NULL);
          break;
        case JN_MD_BLANK:
          break;
        case JN_MD_PARAGRAPH:
        default:
          put_spans (v, b->text, NULL);
          break;
        }
      put (v, "\n", NULL, NULL);
    }
  g_ptr_array_unref (blocks);
  fit_pictures (v);
}

const char *
jn_md_view_get_markdown (JnMdView *v)
{
  return v->markdown;
}

int
jn_md_view_diagrams (JnMdView *v, int *refused)
{
  if (refused != NULL)
    *refused = v->refused;
  return v->diagrams;
}

const char *
jn_md_view_link_at (JnMdView *v, int offset)
{
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_offset (buf (v), &it, offset);
  GSList *tags = gtk_text_iter_get_tags (&it);
  const char *url = NULL;
  for (GSList *l = tags; l != NULL && url == NULL; l = l->next)
    url = g_object_get_data (l->data, "jn-url");
  g_slist_free (tags);
  return url;
}

/* A click on a link opens it in the browser (only http and https ever
 * become links). */
static void
on_click (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
  (void) n;
  JnMdView *v = data;
  int bx, by;
  gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (v), GTK_TEXT_WINDOW_WIDGET, (int) x, (int) y, &bx, &by);
  GtkTextIter it;
  if (!gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (v), &it, bx, by))
    return;
  const char *url = jn_md_view_link_at (v, gtk_text_iter_get_offset (&it));
  if (url == NULL)
    return;
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
  GtkUriLauncher *l = gtk_uri_launcher_new (url);
  gtk_uri_launcher_launch (l, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (v))), NULL, NULL, NULL);
  g_object_unref (l);
}

/* GTK 4.22's GtkTextView can keep the line wrapping of a width it had a
 * moment before (the re-layout it asks for during allocation is
 * dropped), so text runs past the edge when a pane opens narrower.
 * When the width changes, wrap again from an idle, outside allocation. */
static gboolean
rewrap (gpointer data)
{
  JnMdView *v = data;
  v->rewrap_id = 0;
  v->wrapped_width = gtk_widget_get_width (GTK_WIDGET (v));
  fit_pictures (v);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (v), GTK_WRAP_NONE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (v), GTK_WRAP_WORD_CHAR);
  return G_SOURCE_REMOVE;
}

static void
jn_md_view_size_allocate (GtkWidget *w, int width, int height, int baseline)
{
  GTK_WIDGET_CLASS (jn_md_view_parent_class)->size_allocate (w, width, height, baseline);
  JnMdView *v = JN_MD_VIEW (w);
  if (width != v->wrapped_width && v->rewrap_id == 0)
    v->rewrap_id = g_idle_add (rewrap, v);
}

static void
jn_md_view_dispose (GObject *o)
{
  g_clear_handle_id (&JN_MD_VIEW (o)->rewrap_id, g_source_remove);
  g_clear_pointer (&JN_MD_VIEW (o)->pictures, g_ptr_array_unref);
  G_OBJECT_CLASS (jn_md_view_parent_class)->dispose (o);
}

static void
jn_md_view_finalize (GObject *o)
{
  g_free (JN_MD_VIEW (o)->markdown);
  G_OBJECT_CLASS (jn_md_view_parent_class)->finalize (o);
}

static void
jn_md_view_class_init (JnMdViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = jn_md_view_dispose;
  G_OBJECT_CLASS (klass)->finalize = jn_md_view_finalize;
  GTK_WIDGET_CLASS (klass)->size_allocate = jn_md_view_size_allocate;
}

static void
jn_md_view_init (JnMdView *v)
{
  GtkTextView *tv = GTK_TEXT_VIEW (v);
  v->pictures = g_ptr_array_new ();
  gtk_text_view_set_editable (tv, FALSE);
  gtk_text_view_set_cursor_visible (tv, FALSE);
  gtk_text_view_set_wrap_mode (tv, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (tv, 20);
  gtk_text_view_set_right_margin (tv, 20);
  gtk_text_view_set_top_margin (tv, 14);
  gtk_text_view_set_bottom_margin (tv, 30);
  gtk_text_view_set_pixels_below_lines (tv, 4);
  gtk_widget_add_css_class (GTK_WIDGET (v), "md-view");
  GtkTextBuffer *b = gtk_text_view_get_buffer (tv);
  static const double scales[] = { 1.5, 1.25, 1.1, 1.0 };
  for (int i = 0; i < 4; i++)
    v->h[i] = gtk_text_buffer_create_tag (b, NULL, "scale", scales[i], "weight", PANGO_WEIGHT_BOLD,
                                          "pixels-above-lines", 12 - 2 * i, NULL);
  v->bold = gtk_text_buffer_create_tag (b, NULL, "weight", PANGO_WEIGHT_BOLD, NULL);
  v->italic = gtk_text_buffer_create_tag (b, NULL, "style", PANGO_STYLE_ITALIC, NULL);
  v->code = gtk_text_buffer_create_tag (b, NULL, "family", "monospace", "background", "#1B2430", NULL);
  v->block = gtk_text_buffer_create_tag (b, NULL, "family", "monospace", "paragraph-background", "#141B24",
                                         "left-margin", 34, "pixels-above-lines", 2, NULL);
  v->label = gtk_text_buffer_create_tag (b, NULL, "family", "monospace", "scale", 0.8, "foreground", "#7F93AD",
                                         "left-margin", 34, "pixels-above-lines", 6, NULL);
  v->cite = gtk_text_buffer_create_tag (b, NULL, "scale", 0.72, "rise", 4 * PANGO_SCALE, "foreground", "#8FB0DD", NULL);
  v->link = gtk_text_buffer_create_tag (b, NULL, "foreground", "#8FB0DD", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  v->rule = gtk_text_buffer_create_tag (b, NULL, "foreground", "#3A4A5E", "justification", GTK_JUSTIFY_CENTER, NULL);
  v->note = gtk_text_buffer_create_tag (b, NULL, "style", PANGO_STYLE_ITALIC, "foreground", "#9B9B9B", NULL);
  for (int i = 0; i < 5; i++)
    v->depth[i] = gtk_text_buffer_create_tag (b, NULL, "left-margin", 20 + 22 * (i + 1), "indent", -16, NULL);
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_click), v);
  gtk_widget_add_controller (GTK_WIDGET (v), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
jn_md_view_new (void)
{
  return g_object_new (JN_TYPE_MD_VIEW, NULL);
}
