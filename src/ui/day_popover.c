/* day_popover: see day_popover.h. */
#include "day_popover.h"

#include <stdio.h>
#include <string.h>
#include "shade.h"
#include "widgets.h"

#define WEEK_DAYS 7
#define BAR_MAX_PX 64

typedef struct {
  JrWindow  *win;
  GtkWidget *popover;
  GtkWidget *dim_target; /* weak */
  GtkWidget *prev_focus; /* weak: focus to give back on close */
  GtkWidget *stack;      /* NULL while closed */
  GtkWidget *entry;
  GtkWidget *month_list, *month_title;
} Popover;

static void
go_to_day (Popover *p, JrDay day)
{
  gtk_popover_popdown (GTK_POPOVER (p->popover));
  jr_window_show_day_soon (p->win, day);
}

/* One row: "Tue 6 Oct", detail, bar, minutes. The day is kept on the row. */
static GtkWidget *
day_row (JrDay day, JrDay today, int entries, gint64 seconds, int max_minutes)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);

  char name[24];
  jr_day_format_short (day, name, sizeof name);
  GtkWidget *name_label = jr_label (name, "day-name", NULL);
  gtk_widget_set_size_request (name_label, 92, -1);
  gtk_box_append (GTK_BOX (box), name_label);

  char detail[40];
  gboolean is_today = jr_day_compare (day, today) == 0;
  if (entries == 0)
    g_strlcpy (detail, is_today ? "Today" : "No entry", sizeof detail);
  else
    g_snprintf (detail, sizeof detail, "%s%d %s", is_today ? "Today · " : "", entries,
                entries == 1 ? "entry" : "entries");
  GtkWidget *detail_label = jr_label (detail, "muted", NULL);
  gtk_label_set_wrap (GTK_LABEL (detail_label), TRUE);
  gtk_widget_set_size_request (detail_label, 96, -1);
  gtk_box_append (GTK_BOX (box), detail_label);

  int minutes = jr_minutes_from_seconds (seconds);
  GtkWidget *track = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (track, TRUE);
  gtk_widget_set_valign (track, GTK_ALIGN_CENTER);
  if (minutes > 0)
    {
      GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (bar, "minute-bar");
      int px = MAX (6, BAR_MAX_PX * minutes / MAX (max_minutes, 1));
      gtk_widget_set_size_request (bar, px, 4);
      gtk_box_append (GTK_BOX (track), bar);
    }
  gtk_box_append (GTK_BOX (box), track);

  char mins[24] = "";
  if (minutes > 0)
    jr_format_minutes (minutes, mins, sizeof mins);
  GtkWidget *mins_label = jr_label (mins, NULL, NULL);
  gtk_label_set_xalign (GTK_LABEL (mins_label), 1.0f);
  gtk_widget_set_size_request (mins_label, 56, -1);
  gtk_box_append (GTK_BOX (box), mins_label);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  if (is_today)
    gtk_widget_add_css_class (row, "today");
  if (entries == 0)
    gtk_widget_add_css_class (row, "no-entry");
  /* Pack the date into the row's name; read back on activation. */
  char iso[JR_ISO_LEN];
  jr_day_to_iso (day, iso);
  gtk_widget_set_name (row, iso);
  return row;
}

static void
on_day_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  (void) list;
  JrDay day;
  if (jr_day_from_iso (gtk_widget_get_name (GTK_WIDGET (row)), &day))
    go_to_day (data, day);
}

static GtkWidget *
new_list (Popover *p)
{
  GtkWidget *list = gtk_list_box_new ();
  gtk_widget_add_css_class (list, "day-list");
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (list), TRUE);
  g_signal_connect (list, "row-activated", G_CALLBACK (on_day_row_activated), p);
  return list;
}

/* ---- this week ------------------------------------------------------- */

typedef struct {
  JrDay  from;
  int    entries[WEEK_DAYS];
  gint64 seconds[WEEK_DAYS];
} Week;

static void
collect_week (const char *iso, int entries, gint64 seconds, gpointer user)
{
  Week *w = user;
  JrDay d;
  if (!jr_day_from_iso (iso, &d))
    return;
  int i = jr_day_diff (w->from, d);
  if (i >= 0 && i < WEEK_DAYS)
    {
      w->entries[i] = entries;
      w->seconds[i] = seconds;
    }
}

static void
fill_week (Popover *p, GtkWidget *list, JrDay today)
{
  Week w = { .from = jr_day_add (today, -(WEEK_DAYS - 1)) };
  char from[JR_ISO_LEN], to[JR_ISO_LEN];
  jr_day_to_iso (w.from, from);
  jr_day_to_iso (today, to);
  jr_journal_foreach_day (jr_window_get_journal (p->win), from, to, collect_week, &w);

  /* Today's count includes seconds not yet flushed. */
  w.seconds[WEEK_DAYS - 1] = jr_window_day_seconds (p->win, to);
  int max_minutes = 1;
  for (int i = 0; i < WEEK_DAYS; i++)
    max_minutes = MAX (max_minutes, jr_minutes_from_seconds (w.seconds[i]));
  for (int i = WEEK_DAYS - 1; i >= 0; i--)
    gtk_list_box_append (GTK_LIST_BOX (list),
                         day_row (jr_day_add (w.from, i), today, w.entries[i], w.seconds[i],
                                  max_minutes));
}

/* ---- one month ------------------------------------------------------- */

typedef struct {
  Popover *p;
  JrDay    today;
  GArray  *rows; /* collected so they can be shown newest first */
} MonthFill;

typedef struct {
  JrDay  day;
  int    entries;
  gint64 seconds;
} DayStat;

static void
collect_month_day (const char *iso, int entries, gint64 seconds, gpointer user)
{
  MonthFill *m = user;
  DayStat s = { .entries = entries, .seconds = seconds };
  if (entries > 0 && jr_day_from_iso (iso, &s.day))
    g_array_append_val (m->rows, s);
}

static void
show_month (Popover *p, int year, int month)
{
  char title[40];
  g_snprintf (title, sizeof title, "%s %d", jr_month_name (month), year);
  gtk_label_set_text (GTK_LABEL (p->month_title), title);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (p->month_list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (p->month_list), child);

  char from[JR_ISO_LEN], to[JR_ISO_LEN];
  jr_day_to_iso ((JrDay){ year, month, 1 }, from);
  jr_day_to_iso ((JrDay){ year, month, jr_days_in_month (year, month) }, to);
  MonthFill m = { p, jr_day_today (), g_array_new (FALSE, FALSE, sizeof (DayStat)) };
  jr_journal_foreach_day (jr_window_get_journal (p->win), from, to, collect_month_day, &m);

  int max_minutes = 1;
  for (guint i = 0; i < m.rows->len; i++)
    max_minutes = MAX (max_minutes,
                       jr_minutes_from_seconds (g_array_index (m.rows, DayStat, i).seconds));
  for (guint i = m.rows->len; i > 0; i--)
    {
      DayStat *s = &g_array_index (m.rows, DayStat, i - 1);
      gtk_list_box_append (GTK_LIST_BOX (p->month_list),
                           day_row (s->day, m.today, s->entries, s->seconds, max_minutes));
    }
  g_array_unref (m.rows);
  gtk_stack_set_visible_child_name (GTK_STACK (p->stack), "month");
}

static void
on_month_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  (void) list;
  int year, month;
  if (sscanf (gtk_widget_get_name (GTK_WIDGET (row)), "%d-%d", &year, &month) == 2)
    show_month (data, year, month);
}

static void
collect_months (const char *ym, int days_written, gpointer user)
{
  GtkWidget *list = user;
  int year, month;
  if (sscanf (ym, "%d-%d", &year, &month) != 2)
    return;

  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  char title[40], detail[40];
  g_snprintf (title, sizeof title, "%s %d", jr_month_name (month), year);
  g_snprintf (detail, sizeof detail, "%d %s written", days_written, days_written == 1 ? "day" : "days");
  GtkWidget *t = jr_label (title, NULL, NULL);
  gtk_widget_set_hexpand (t, TRUE);
  gtk_box_append (GTK_BOX (box), t);
  gtk_box_append (GTK_BOX (box), jr_label (detail, "muted", NULL));
  gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("jr-chevron-right-symbolic"));
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  gtk_widget_set_name (row, ym);
  gtk_list_box_append (GTK_LIST_BOX (list), row);
}

static void
on_month_back (GtkButton *b, gpointer data)
{
  (void) b;
  Popover *p = data;
  gtk_stack_set_visible_child_name (GTK_STACK (p->stack), "main");
}

/* ---- jump to a date -------------------------------------------------- */

static void
on_jump_activate (GtkEntry *entry, gpointer data)
{
  Popover *p = data;
  JrDay day;
  if (jr_day_parse_input (gtk_editable_get_text (GTK_EDITABLE (entry)), jr_day_today (), &day))
    go_to_day (p, day);
  else
    {
      gtk_widget_add_css_class (GTK_WIDGET (entry), "error");
      gtk_widget_set_tooltip_text (GTK_WIDGET (entry),
                                   "Try “4 Oct” or “2026-10-04”. Future days cannot be opened.");
    }
}

static void
on_jump_changed (GtkEditable *e, gpointer data)
{
  (void) data;
  gtk_widget_remove_css_class (GTK_WIDGET (e), "error");
}

/* ---- open / close ---------------------------------------------------- */

static void
build (Popover *p)
{
  JrDay today = jr_day_today ();
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (root, 330, -1);

  gtk_box_append (GTK_BOX (root), jr_label ("Jump to a date", "muted", NULL));
  p->entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (p->entry), "4 Oct, or 2026-10-04");
  gtk_widget_set_margin_top (p->entry, 6);
  g_signal_connect (p->entry, "activate", G_CALLBACK (on_jump_activate), p);
  g_signal_connect (p->entry, "changed", G_CALLBACK (on_jump_changed), p);
  gtk_box_append (GTK_BOX (root), p->entry);

  /* Main page: this week, then earlier months. */
  GtkWidget *main = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (main), jr_label ("THIS WEEK", "section-title", NULL));
  GtkWidget *week = new_list (p);
  fill_week (p, week, today);
  gtk_box_append (GTK_BOX (main), week);

  char this_month[8];
  g_snprintf (this_month, sizeof this_month, "%04d-%02d", today.year, today.month);
  GtkWidget *months = gtk_list_box_new ();
  gtk_widget_add_css_class (months, "day-list");
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (months), GTK_SELECTION_NONE);
  g_signal_connect (months, "row-activated", G_CALLBACK (on_month_row_activated), p);
  jr_journal_foreach_month (jr_window_get_journal (p->win), this_month, collect_months, months);
  if (gtk_widget_get_first_child (months) != NULL)
    {
      GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (sep, "month-sep");
      gtk_box_append (GTK_BOX (main), sep);
      gtk_box_append (GTK_BOX (main), jr_label ("EARLIER", "section-title", NULL));
      gtk_box_append (GTK_BOX (main), months);
    }
  else
    g_object_ref_sink (months), g_object_unref (months);

  /* Month page: back button and that month's written days. */
  GtkWidget *month = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *back = gtk_button_new ();
  GtkWidget *bb = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (bb), gtk_image_new_from_icon_name ("jr-chevron-left-symbolic"));
  p->month_title = gtk_label_new ("");
  gtk_box_append (GTK_BOX (bb), p->month_title);
  gtk_button_set_child (GTK_BUTTON (back), bb);
  gtk_widget_add_css_class (back, "flat");
  gtk_widget_set_halign (back, GTK_ALIGN_START);
  gtk_widget_set_margin_top (back, 10);
  g_signal_connect (back, "clicked", G_CALLBACK (on_month_back), p);
  gtk_box_append (GTK_BOX (month), back);
  p->month_list = new_list (p);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 360);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), p->month_list);
  gtk_box_append (GTK_BOX (month), scroll);

  p->stack = gtk_stack_new ();
  gtk_stack_set_vhomogeneous (GTK_STACK (p->stack), FALSE);
  gtk_stack_add_named (GTK_STACK (p->stack), main, "main");
  gtk_stack_add_named (GTK_STACK (p->stack), month, "month");
  gtk_box_append (GTK_BOX (root), p->stack);

  gtk_popover_set_child (GTK_POPOVER (p->popover), root);
}

static void
on_show (GtkWidget *popover, gpointer data)
{
  Popover *p = data;
  if (p->stack == NULL)
    build (p);
  GtkRoot *root = gtk_widget_get_root (popover);
  GtkWidget *focus = root ? gtk_root_get_focus (root) : NULL;
  if (focus != NULL && p->prev_focus == NULL)
    {
      p->prev_focus = focus;
      g_object_add_weak_pointer (G_OBJECT (focus), (gpointer *) &p->prev_focus);
    }
  if (p->dim_target != NULL)
    gtk_widget_add_css_class (p->dim_target, "dimmed");
  gtk_widget_grab_focus (p->entry);
}

static void
refocus_idle (gpointer data)
{
  GtkWidget *widget = data;
  if (gtk_widget_get_root (widget) != NULL) /* still on screen */
    gtk_widget_grab_focus (widget);
  g_object_unref (widget);
}

static void
on_closed (GtkPopover *popover, gpointer data)
{
  Popover *p = data;
  if (p->dim_target != NULL)
    gtk_widget_remove_css_class (p->dim_target, "dimmed");
  /* Hand focus back before dropping the content: GTK otherwise keeps a
   * reference to the popover through the entry that had focus, and every
   * opened dropdown would stay in memory. */
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (popover));
  if (root != NULL)
    gtk_root_set_focus (root, NULL);
  /* Free the content while closed. */
  p->stack = p->entry = p->month_list = p->month_title = NULL;
  gtk_popover_set_child (popover, NULL);
  if (p->prev_focus != NULL)
    {
      /* After GtkMenuButton has taken focus back for itself. */
      GtkWidget *prev = p->prev_focus;
      g_object_remove_weak_pointer (G_OBJECT (prev), (gpointer *) &p->prev_focus);
      p->prev_focus = NULL;
      g_idle_add_once (refocus_idle, g_object_ref (prev));
    }
}

static void
popover_free (gpointer data)
{
  Popover *p = data;
  if (p->dim_target != NULL)
    g_object_remove_weak_pointer (G_OBJECT (p->dim_target), (gpointer *) &p->dim_target);
  if (p->prev_focus != NULL)
    g_object_remove_weak_pointer (G_OBJECT (p->prev_focus), (gpointer *) &p->prev_focus);
  g_free (p);
}

GtkWidget *
jr_day_popover_new (JrWindow *win, JrDay current, GtkWidget *dim_target)
{
  (void) current;
  Popover *p = g_new0 (Popover, 1);
  p->win = win;
  p->popover = gtk_popover_new ();
  gtk_popover_set_has_arrow (GTK_POPOVER (p->popover), FALSE);
  gtk_widget_set_halign (p->popover, GTK_ALIGN_START);
  p->dim_target = dim_target;
  if (dim_target != NULL)
    g_object_add_weak_pointer (G_OBJECT (dim_target), (gpointer *) &p->dim_target);
  g_signal_connect (p->popover, "show", G_CALLBACK (on_show), p);
  g_signal_connect (p->popover, "closed", G_CALLBACK (on_closed), p);
  g_object_set_data_full (G_OBJECT (p->popover), "jr-popover", p, popover_free);
  return p->popover;
}
