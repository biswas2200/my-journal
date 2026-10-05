/* activity_view: see activity_view.h. */
#include "activity_view.h"

#include "bar_chart.h"
#include "shade.h"
#include "widgets.h"

#define JR_TYPE_ACTIVITY_VIEW (jr_activity_view_get_type ())
G_DECLARE_FINAL_TYPE (JrActivityView, jr_activity_view, JR, ACTIVITY_VIEW, GtkBox)

struct _JrActivityView {
  GtkBox     parent_instance;
  JrWindow  *win;
  int        year, month;    /* month on the calendar */
  GtkWidget *month_label, *next_button, *grid_holder;
  GtkWidget *days_value, *time_value, *avg_value, *entries_value;
  GtkWidget *chart;
};

G_DEFINE_FINAL_TYPE (JrActivityView, jr_activity_view, GTK_TYPE_BOX)

/* Per-day numbers for one month (index = day of month). */
typedef struct {
  JrDay  first;
  int    entries[32];
  gint64 seconds[32];
} Month;

static void
collect_day (const char *iso, int entries, gint64 seconds, gpointer user)
{
  Month *m = user;
  JrDay d;
  if (jr_day_from_iso (iso, &d) && d.year == m->first.year && d.month == m->first.month)
    {
      m->entries[d.day] = entries;
      m->seconds[d.day] = seconds;
    }
}

static void
load_month (JrActivityView *self, Month *m)
{
  *m = (Month){ .first = { self->year, self->month, 1 } };
  char from[JR_ISO_LEN], to[JR_ISO_LEN];
  jr_day_to_iso (m->first, from);
  jr_day_to_iso ((JrDay){ self->year, self->month, jr_days_in_month (self->year, self->month) }, to);
  jr_journal_foreach_day (jr_window_get_journal (self->win), from, to, collect_day, m);

  /* Include today's not-yet-flushed seconds. */
  JrDay today = jr_day_today ();
  if (today.year == self->year && today.month == self->month)
    {
      char iso[JR_ISO_LEN];
      jr_day_to_iso (today, iso);
      m->seconds[today.day] = jr_window_day_seconds (self->win, iso);
    }
}

static void
on_cell_clicked (GtkButton *button, gpointer data)
{
  JrActivityView *self = data;
  JrDay day;
  if (jr_day_from_iso (gtk_widget_get_name (GTK_WIDGET (button)), &day))
    jr_window_show_day_soon (self->win, day);
}

static GtkWidget *
make_cell (JrActivityView *self, JrDay day, JrDay today, int entries, gint64 seconds)
{
  GtkWidget *cell = gtk_button_new ();
  gtk_widget_add_css_class (cell, "cal-cell");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  char num[4], mins[16] = "";
  g_snprintf (num, sizeof num, "%d", day.day);
  gtk_box_append (GTK_BOX (box), jr_label (num, NULL, NULL));

  int minutes = jr_minutes_from_seconds (seconds);
  int cmp = jr_day_compare (day, today);
  if (cmp > 0)
    {
      gtk_widget_add_css_class (cell, "future");
      gtk_widget_set_sensitive (cell, FALSE);
    }
  else
    {
      char css[8];
      g_snprintf (css, sizeof css, "shade-%d", jr_shade_level (minutes, entries > 0));
      gtk_widget_add_css_class (cell, css);
      if (minutes > 0)
        g_snprintf (mins, sizeof mins, "%dm", minutes);
    }
  if (cmp == 0)
    gtk_widget_add_css_class (cell, "today");
  gtk_box_append (GTK_BOX (box), jr_label (mins, "cal-minutes", NULL));
  gtk_button_set_child (GTK_BUTTON (cell), box);

  char iso[JR_ISO_LEN], tip[64];
  jr_day_to_iso (day, iso);
  gtk_widget_set_name (cell, iso);
  if (cmp <= 0)
    {
      g_snprintf (tip, sizeof tip, "%d %s: %d min, %d %s", day.day, jr_month_name (day.month),
                  minutes, entries, entries == 1 ? "entry" : "entries");
      gtk_widget_set_tooltip_text (cell, tip);
      g_signal_connect (cell, "clicked", G_CALLBACK (on_cell_clicked), self);
    }
  return cell;
}

static void
show_month (JrActivityView *self)
{
  Month m;
  load_month (self, &m);
  JrDay today = jr_day_today ();

  char title[40];
  g_snprintf (title, sizeof title, "%s %d", jr_month_name (self->month), self->year);
  gtk_label_set_text (GTK_LABEL (self->month_label), title);
  gtk_widget_set_sensitive (self->next_button,
                            self->year < today.year ||
                            (self->year == today.year && self->month < today.month));

  /* Calendar grid, Monday first. The old grid is destroyed. */
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  static const char *const names[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
  for (int c = 0; c < 7; c++)
    {
      GtkWidget *l = jr_label (names[c], "cal-weekday", NULL);
      gtk_label_set_xalign (GTK_LABEL (l), 0.5f);
      gtk_grid_attach (GTK_GRID (grid), l, c, 0, 1, 1);
    }
  int offset = jr_day_weekday (m.first) - 1;
  int days = jr_days_in_month (self->year, self->month);
  int written = 0, entries = 0;
  gint64 seconds = 0;
  for (int d = 1; d <= days; d++)
    {
      int pos = offset + d - 1;
      JrDay day = { self->year, self->month, d };
      gtk_grid_attach (GTK_GRID (grid), make_cell (self, day, today, m.entries[d], m.seconds[d]),
                       pos % 7, 1 + pos / 7, 1, 1);
      written += m.entries[d] > 0;
      entries += m.entries[d];
      seconds += m.seconds[d];
    }
  GtkWidget *old = gtk_widget_get_first_child (self->grid_holder);
  if (old != NULL)
    gtk_box_remove (GTK_BOX (self->grid_holder), old);
  gtk_box_append (GTK_BOX (self->grid_holder), grid);

  /* Stat cards. "Of" counts the days of this month so far. */
  int elapsed;
  if (self->year == today.year && self->month == today.month)
    elapsed = today.day;
  else if (self->year < today.year || (self->year == today.year && self->month < today.month))
    elapsed = days;
  else
    elapsed = 0;
  char buf[32];
  g_snprintf (buf, sizeof buf, "%d of %d", written, elapsed);
  gtk_label_set_text (GTK_LABEL (self->days_value), buf);
  int total_minutes = jr_minutes_from_seconds (seconds);
  jr_format_minutes (total_minutes, buf, sizeof buf);
  gtk_label_set_text (GTK_LABEL (self->time_value), buf);
  if (written > 0)
    jr_format_minutes (jr_minutes_from_seconds (seconds / written), buf, sizeof buf);
  else
    g_strlcpy (buf, "–", sizeof buf);
  gtk_label_set_text (GTK_LABEL (self->avg_value), buf);
  g_snprintf (buf, sizeof buf, "%d", entries);
  gtk_label_set_text (GTK_LABEL (self->entries_value), buf);
}

typedef struct {
  JrDay  from;
  gint64 seconds[JR_BAR_CHART_BARS];
} Week;

static void
collect_week (const char *iso, int entries, gint64 seconds, gpointer user)
{
  (void) entries;
  Week *w = user;
  JrDay d;
  if (!jr_day_from_iso (iso, &d))
    return;
  int i = jr_day_diff (w->from, d);
  if (i >= 0 && i < JR_BAR_CHART_BARS)
    w->seconds[i] = seconds;
}

static void
fill_chart (JrActivityView *self)
{
  JrDay today = jr_day_today ();
  Week w = { .from = jr_day_add (today, -(JR_BAR_CHART_BARS - 1)) };
  char from[JR_ISO_LEN], to[JR_ISO_LEN];
  jr_day_to_iso (w.from, from);
  jr_day_to_iso (today, to);
  jr_journal_foreach_day (jr_window_get_journal (self->win), from, to, collect_week, &w);
  w.seconds[JR_BAR_CHART_BARS - 1] = jr_window_day_seconds (self->win, to);

  int minutes[JR_BAR_CHART_BARS];
  const char *labels[JR_BAR_CHART_BARS];
  for (int i = 0; i < JR_BAR_CHART_BARS; i++)
    {
      minutes[i] = jr_minutes_from_seconds (w.seconds[i]);
      labels[i] = jr_weekday_abbr (jr_day_add (w.from, i));
    }
  jr_bar_chart_set (JR_BAR_CHART (self->chart), minutes, labels, JR_BAR_CHART_BARS - 1);
}

static void
on_prev (GtkButton *b, gpointer data)
{
  (void) b;
  JrActivityView *self = data;
  if (--self->month < 1)
    {
      self->month = 12;
      self->year--;
    }
  show_month (self);
}

static void
on_next (GtkButton *b, gpointer data)
{
  (void) b;
  JrActivityView *self = data;
  if (++self->month > 12)
    {
      self->month = 1;
      self->year++;
    }
  show_month (self);
}

static void
on_back (GtkButton *b, gpointer data)
{
  (void) b;
  JrActivityView *self = data;
  jr_window_show_today (self->win);
}

static void
on_security (GtkButton *b, gpointer data)
{
  (void) b;
  JrActivityView *self = data;
  jr_window_show_security (self->win);
}

static GtkWidget *
stat_card (const char *title, GtkWidget **value_out)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (card, "card");
  gtk_widget_set_hexpand (card, TRUE);
  gtk_box_append (GTK_BOX (card), jr_label (title, "card-sub", "mono", NULL));
  *value_out = jr_label ("", "stat-big", "mono", NULL);
  gtk_box_append (GTK_BOX (card), *value_out);
  return card;
}

static GtkWidget *
icon_button (const char *icon, const char *tip)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (b, tip);
  return b;
}

GtkWidget *
jr_activity_view_new (JrWindow *win, GtkWidget **header_out)
{
  JrActivityView *self = g_object_new (JR_TYPE_ACTIVITY_VIEW, NULL);
  self->win = win;
  JrDay today = jr_day_today ();
  self->year = today.year;
  self->month = today.month;

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 36);
  gtk_widget_set_margin_top (body, 28);
  gtk_widget_set_margin_bottom (body, 20);
  gtk_widget_set_margin_start (body, 40);
  gtk_widget_set_margin_end (body, 40);
  gtk_widget_set_vexpand (body, TRUE);

  /* Left: calendar. */
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_size_request (left, 320, -1);
  GtkWidget *nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->month_label = jr_label ("", "cal-month", "mono", NULL);
  gtk_widget_set_hexpand (self->month_label, TRUE);
  gtk_box_append (GTK_BOX (nav), self->month_label);
  GtkWidget *prev = icon_button ("jr-chevron-left-symbolic", "Previous month");
  self->next_button = icon_button ("jr-chevron-right-symbolic", "Next month");
  g_signal_connect (prev, "clicked", G_CALLBACK (on_prev), self);
  g_signal_connect (self->next_button, "clicked", G_CALLBACK (on_next), self);
  gtk_box_append (GTK_BOX (nav), prev);
  gtk_box_append (GTK_BOX (nav), self->next_button);
  gtk_box_append (GTK_BOX (left), nav);
  self->grid_holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (left), self->grid_holder);

  GtkWidget *legend = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (legend), jr_label ("Fewer", "muted", "mono", NULL));
  for (int i = 0; i <= 3; i++)
    {
      GtkWidget *sw = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (sw, "legend-swatch");
      char css[8];
      g_snprintf (css, sizeof css, "shade-%d", i);
      gtk_widget_add_css_class (sw, css);
      gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (legend), sw);
    }
  gtk_box_append (GTK_BOX (legend), jr_label ("More minutes written", "muted", "mono", NULL));
  gtk_box_append (GTK_BOX (left), legend);
  gtk_box_append (GTK_BOX (body), left);

  /* Right: stat cards and the 7-day chart. */
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_hexpand (right, TRUE);
  GtkWidget *cards = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (cards), 10);
  gtk_grid_set_column_spacing (GTK_GRID (cards), 10);
  gtk_grid_set_column_homogeneous (GTK_GRID (cards), TRUE);
  gtk_grid_attach (GTK_GRID (cards), stat_card ("Days written", &self->days_value), 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (cards), stat_card ("Time writing", &self->time_value), 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (cards), stat_card ("Average per day written", &self->avg_value), 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (cards), stat_card ("Entries", &self->entries_value), 1, 1, 1, 1);
  gtk_box_append (GTK_BOX (right), cards);
  GtkWidget *chart_title = jr_label ("MINUTES WRITTEN · LAST 7 DAYS", "section-title", "mono", NULL);
  gtk_widget_set_margin_top (chart_title, 14);
  gtk_box_append (GTK_BOX (right), chart_title);
  self->chart = jr_bar_chart_new ();
  gtk_widget_set_vexpand (self->chart, TRUE);
  gtk_box_append (GTK_BOX (right), self->chart);
  gtk_box_append (GTK_BOX (body), right);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), body);
  gtk_box_append (GTK_BOX (self), scroller);

  GtkWidget *status = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (status, "statusbar");
  gtk_box_append (GTK_BOX (status),
                  jr_label ("Minutes count while you type, and pause after 60 seconds idle. "
                            "An empty day is just empty.", NULL, NULL));
  gtk_box_append (GTK_BOX (self), status);

  show_month (self);
  fill_chart (self);

  /* Header: back to the journal, title, lock. */
  GtkWidget *header = jr_header_new (NULL);
  GtkWidget *back = jr_button ("jr-arrow-left-symbolic", "Journal");
  g_signal_connect (back, "clicked", G_CALLBACK (on_back), self);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), back);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), jr_label ("Activity & time", "title-text", NULL));
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), jr_header_sep ());
  GtkWidget *lock = jr_button ("jr-lock-symbolic", "Lock");
  gtk_widget_add_css_class (lock, "flat");
  gtk_widget_set_tooltip_text (lock, "Lock and security");
  g_signal_connect (lock, "clicked", G_CALLBACK (on_security), self);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), lock);
  *header_out = header;
  return GTK_WIDGET (self);
}

static void
jr_activity_view_class_init (JrActivityViewClass *klass)
{
  (void) klass;
}

static void
jr_activity_view_init (JrActivityView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
}
