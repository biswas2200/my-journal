/* bar_chart: see bar_chart.h. */
#include "bar_chart.h"

#define VALUE_SPACE 22  /* room above the tallest bar for its number */
#define LABEL_SPACE 26  /* room below the bars for weekday names */
#define GAP 14

struct _JrBarChart {
  GtkWidget parent_instance;
  int       minutes[JR_BAR_CHART_BARS];
  char      labels[JR_BAR_CHART_BARS][4];
  int       highlight;
};

G_DEFINE_FINAL_TYPE (JrBarChart, jr_bar_chart, GTK_TYPE_WIDGET)

static const GdkRGBA BAR = { 0x9B / 255.0f, 0x9B / 255.0f, 0x9B / 255.0f, 1.0f };
static const GdkRGBA BAR_HI = { 1.0f, 1.0f, 1.0f, 1.0f };
static const GdkRGBA AXIS = { 0x2B / 255.0f, 0x2B / 255.0f, 0x2B / 255.0f, 1.0f };
static const GdkRGBA TEXT = { 0xED / 255.0f, 0xED / 255.0f, 0xED / 255.0f, 1.0f };
static const GdkRGBA MUTED = { 0x9B / 255.0f, 0x9B / 255.0f, 0x9B / 255.0f, 1.0f };

/* Draws `text` centred on x at y, reusing one layout. */
static void
draw_text (GtkSnapshot *s, PangoLayout *layout, const char *text, float cx, float y,
           const GdkRGBA *color, gboolean bold)
{
  pango_layout_set_text (layout, text, -1);
  PangoAttrList *attrs = NULL;
  if (bold)
    {
      attrs = pango_attr_list_new ();
      pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    }
  pango_layout_set_attributes (layout, attrs);
  int w, h;
  pango_layout_get_pixel_size (layout, &w, &h);
  gtk_snapshot_save (s);
  gtk_snapshot_translate (s, &GRAPHENE_POINT_INIT (cx - w / 2.0f, y));
  gtk_snapshot_append_layout (s, layout, color);
  gtk_snapshot_restore (s);
  if (attrs != NULL)
    pango_attr_list_unref (attrs);
}

static void
jr_bar_chart_snapshot (GtkWidget *widget, GtkSnapshot *s)
{
  JrBarChart *self = JR_BAR_CHART (widget);
  float width = (float) gtk_widget_get_width (widget);
  float height = (float) gtk_widget_get_height (widget);
  float plot = height - VALUE_SPACE - LABEL_SPACE;
  if (plot <= 4 || width <= GAP * JR_BAR_CHART_BARS)
    return;

  int max = 1;
  for (int i = 0; i < JR_BAR_CHART_BARS; i++)
    max = MAX (max, self->minutes[i]);

  float bar_w = (width - GAP * (JR_BAR_CHART_BARS - 1)) / JR_BAR_CHART_BARS;
  float base = VALUE_SPACE + plot;
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, NULL);

  gtk_snapshot_append_color (s, &AXIS, &GRAPHENE_RECT_INIT (0, base, width, 1));
  for (int i = 0; i < JR_BAR_CHART_BARS; i++)
    {
      float x = i * (bar_w + GAP);
      float cx = x + bar_w / 2;
      int m = self->minutes[i];
      float h = m > 0 ? MAX (2.0f, plot * m / max) : 0;
      gboolean hi = i == self->highlight;
      char value[16];
      if (m > 0)
        {
          gtk_snapshot_append_color (s, hi ? &BAR_HI : &BAR,
                                     &GRAPHENE_RECT_INIT (x, base - h, bar_w, h));
          g_snprintf (value, sizeof value, "%d", m);
        }
      else
        g_strlcpy (value, "–", sizeof value);
      draw_text (s, layout, value, cx, base - h - 18, &TEXT, FALSE);
      draw_text (s, layout, self->labels[i], cx, base + 8, hi ? &TEXT : &MUTED, hi);
    }
  g_object_unref (layout);
}

static void
jr_bar_chart_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural, int *min_baseline, int *nat_baseline)
{
  (void) widget; (void) for_size; (void) min_baseline; (void) nat_baseline;
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *minimum = *natural = 7 * 24 + 6 * GAP;
  else
    {
      *minimum = 120;
      *natural = 200;
    }
}

static void
jr_bar_chart_class_init (JrBarChartClass *klass)
{
  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  wc->snapshot = jr_bar_chart_snapshot;
  wc->measure = jr_bar_chart_measure;
  gtk_widget_class_set_accessible_role (wc, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
jr_bar_chart_init (JrBarChart *self)
{
  self->highlight = -1;
  gtk_widget_add_css_class (GTK_WIDGET (self), "chart");
}

GtkWidget *
jr_bar_chart_new (void)
{
  return g_object_new (JR_TYPE_BAR_CHART, NULL);
}

void
jr_bar_chart_set (JrBarChart *self, const int minutes[JR_BAR_CHART_BARS],
                  const char *const labels[JR_BAR_CHART_BARS], int highlight)
{
  GString *desc = g_string_new ("Minutes written, last 7 days:");
  for (int i = 0; i < JR_BAR_CHART_BARS; i++)
    {
      self->minutes[i] = minutes[i];
      g_strlcpy (self->labels[i], labels[i], sizeof self->labels[i]);
      g_string_append_printf (desc, " %s %d,", labels[i], minutes[i]);
    }
  self->highlight = highlight;
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  desc->str, -1);
  g_string_free (desc, TRUE);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
