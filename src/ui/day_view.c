/* day_view: see day_view.h. */
#include "day_view.h"

#include <string.h>
#include "active_time.h"
#include "day_popover.h"
#include "shade.h"
#include "text.h"
#include "vault.h"
#include "widgets.h"

#define SAVE_DELAY_MS 1000
#define WORDS_DELAY_MS 250
#define COLUMN_WIDTH 700

/* One stamped block. Widgets belong to the view; this struct is ours. */
typedef struct {
  JrDayView     *view;
  gint64         id;          /* 0 = not in the database (yet, or any more) */
  gint64         stamped_at;  /* 0 = draft, stamped on first keystroke */
  char           day[JR_ISO_LEN]; /* the day the stamp belongs to */
  gboolean       dirty;       /* text changed since last save */
  gboolean       words_dirty;
  guint          words;
  GtkWidget     *row, *stamp, *text;
  GtkTextBuffer *buffer;
  GtkEventController *focus;
  gulong         changed_id;
} Block;

struct _JrDayView {
  GtkBox     parent_instance;
  JrWindow  *win;
  JrDay      day;
  char       day_iso[JR_ISO_LEN];
  gboolean   is_today;
  gboolean   editable;
  GPtrArray *blocks;       /* Block*, in display order */
  guint      save_id, words_id, remeasure_id;
  int        remeasure_frames;
  gint64     last_edit;    /* unix seconds of the last keystroke here */
  int        clock_minute; /* minute last drawn on the clock */
  gboolean   save_failed;

  GtkWidget *scroller, *blocks_box, *new_button, *empty_label, *edit_button;
  GtkWidget *clock, *sub, *session_value, *today_value;
  GtkWidget *words_label, *saved_label, *saved_icon;
  GtkWidget *day_button;
};

G_DEFINE_FINAL_TYPE (JrDayView, jr_day_view, GTK_TYPE_BOX)

static JrJournal *
journal_of (JrDayView *self)
{
  return jr_window_get_journal (self->win);
}

/* ---- status bar ------------------------------------------------------ */

static void
update_words (JrDayView *self)
{
  guint total = 0;
  for (guint i = 0; i < self->blocks->len; i++)
    {
      Block *b = g_ptr_array_index (self->blocks, i);
      if (b->words_dirty)
        {
          GtkTextIter s, e;
          gtk_text_buffer_get_bounds (b->buffer, &s, &e);
          char *text = gtk_text_buffer_get_text (b->buffer, &s, &e, FALSE);
          b->words = jr_count_words (text);
          jr_secret_free (text);
          b->words_dirty = FALSE;
        }
      total += b->words;
    }
  char buf[32];
  g_snprintf (buf, sizeof buf, "%u %s", total, total == 1 ? "word" : "words");
  gtk_label_set_text (GTK_LABEL (self->words_label), buf);
}

static gboolean
on_words_timeout (gpointer data)
{
  JrDayView *self = data;
  self->words_id = 0;
  update_words (self);
  return G_SOURCE_REMOVE;
}

static void
show_saved (JrDayView *self, gint64 now)
{
  if (self->save_failed)
    {
      gtk_label_set_text (GTK_LABEL (self->saved_label), "Could not save · will retry");
      gtk_widget_set_visible (self->saved_icon, FALSE);
      return;
    }
  char clock[16], buf[32];
  jr_format_clock (now, clock, sizeof clock);
  g_snprintf (buf, sizeof buf, "Saved %s", clock);
  gtk_label_set_text (GTK_LABEL (self->saved_label), buf);
  gtk_widget_set_visible (self->saved_icon, TRUE);
}

/* ---- saving ---------------------------------------------------------- */

static gboolean
save_block (JrDayView *self, Block *b, gint64 now)
{
  JrJournal *j = journal_of (self);
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds (b->buffer, &s, &e);
  char *text = gtk_text_buffer_get_text (b->buffer, &s, &e, FALSE);
  gboolean ok = TRUE;

  if (text[0] == '\0')
    {
      /* An emptied block leaves the database; its stamp stays on screen
       * in case the writer types again. */
      if (b->id != 0)
        ok = jr_journal_delete_entry (j, b->id);
      if (ok)
        b->id = 0;
    }
  else if (b->id == 0)
    ok = (b->id = jr_journal_add_entry (j, b->day, b->stamped_at, text, now)) != 0;
  else
    ok = jr_journal_update_entry (j, b->id, text, now);

  jr_secret_free (text); /* wipe our plain copy */
  if (ok)
    b->dirty = FALSE;
  return ok;
}

void
jr_day_view_save (JrDayView *self)
{
  g_clear_handle_id (&self->save_id, g_source_remove);
  if (!jr_journal_is_unlocked (journal_of (self)))
    return;

  gint64 now = jr_now ();
  gboolean any = FALSE, ok = TRUE;
  for (guint i = 0; i < self->blocks->len; i++)
    {
      Block *b = g_ptr_array_index (self->blocks, i);
      if (b->dirty && b->stamped_at != 0)
        {
          any = TRUE;
          ok = save_block (self, b, now) && ok;
        }
    }
  self->save_failed = !ok;
  if (any)
    show_saved (self, now);
}

static gboolean
on_save_timeout (gpointer data)
{
  JrDayView *self = data;
  self->save_id = 0;
  jr_day_view_save (self);
  return G_SOURCE_REMOVE;
}

/* ---- blocks ---------------------------------------------------------- */

static void
set_stamp_label (Block *b)
{
  char clock[16];
  jr_format_clock (b->stamped_at != 0 ? b->stamped_at : jr_now (), clock, sizeof clock);
  gtk_label_set_text (GTK_LABEL (b->stamp), clock);
}

static void
stamp_block (Block *b, gint64 now)
{
  b->stamped_at = now;
  /* The day is the local date at the moment of stamping (00:30 -> new day). */
  jr_day_to_iso (jr_day_from_time (now), b->day);
  gtk_widget_remove_css_class (b->row, "draft");
  set_stamp_label (b);
}

static void
on_buffer_changed (GtkTextBuffer *buffer, gpointer data)
{
  (void) buffer;
  Block *b = data;
  JrDayView *self = b->view;
  gint64 now = jr_now ();

  if (b->stamped_at == 0)
    stamp_block (b, now);
  b->dirty = TRUE;
  b->words_dirty = TRUE;
  self->last_edit = now;
  jr_window_note_keystroke (self->win);

  /* Debounce: restart the timers on every change. */
  g_clear_handle_id (&self->save_id, g_source_remove);
  self->save_id = g_timeout_add (SAVE_DELAY_MS, on_save_timeout, self);
  if (self->words_id == 0)
    self->words_id = g_timeout_add (WORDS_DELAY_MS, on_words_timeout, self);
  gtk_label_set_text (GTK_LABEL (self->saved_label), "Saving…");
  gtk_widget_set_visible (self->saved_icon, FALSE);
}

static void
on_focus_enter (GtkEventControllerFocus *c, gpointer data)
{
  (void) c;
  Block *b = data;
  gtk_widget_add_css_class (b->row, "active");
}

static void
on_focus_leave (GtkEventControllerFocus *c, gpointer data)
{
  (void) c;
  Block *b = data;
  gtk_widget_remove_css_class (b->row, "active");
}

static void
block_free (gpointer data)
{
  Block *b = data;
  /* The text view outlives this struct by a moment while GTK tears the
   * screen down, and losing focus then would call back into freed memory. */
  if (b->focus != NULL)
    g_signal_handlers_disconnect_by_data (b->focus, b);
  if (b->buffer != NULL)
    {
      g_signal_handler_disconnect (b->buffer, b->changed_id);
      /* Drop the text now rather than whenever GTK frees the buffer. */
      gtk_text_buffer_set_text (b->buffer, "", 0);
    }
  g_free (b);
}

static Block *
add_block (JrDayView *self, gint64 id, gint64 stamped_at, const char *text)
{
  Block *b = g_new0 (Block, 1);
  b->view = self;
  b->id = id;
  b->stamped_at = stamped_at;
  if (stamped_at != 0)
    jr_day_to_iso (jr_day_from_time (stamped_at), b->day);

  b->row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (b->row, "block");
  gtk_widget_set_margin_bottom (b->row, 18);
  if (stamped_at == 0)
    gtk_widget_add_css_class (b->row, "draft");

  b->stamp = jr_label ("", "stamp", NULL);
  gtk_widget_set_valign (b->stamp, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (b->row), b->stamp);
  set_stamp_label (b);

  b->text = gtk_text_view_new ();
  gtk_widget_add_css_class (b->text, "body");
  gtk_widget_set_hexpand (b->text, TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (b->text), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_pixels_inside_wrap (GTK_TEXT_VIEW (b->text), 7);
  gtk_text_view_set_pixels_below_lines (GTK_TEXT_VIEW (b->text), 7);
  gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (b->text), FALSE);
  /* Private: input methods must not learn or remember what is written here. */
  gtk_text_view_set_input_hints (GTK_TEXT_VIEW (b->text), GTK_INPUT_HINT_PRIVATE);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (b->text), self->editable);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (b->text), self->editable);
  if (!self->editable)
    gtk_widget_add_css_class (b->text, "readonly");
  gtk_accessible_update_property (GTK_ACCESSIBLE (b->text), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Journal entry", -1);
  b->buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (b->text));
  if (text != NULL)
    gtk_text_buffer_set_text (b->buffer, text, -1);
  b->words = jr_count_words (text);
  b->changed_id = g_signal_connect (b->buffer, "changed", G_CALLBACK (on_buffer_changed), b);
  gtk_box_append (GTK_BOX (b->row), b->text);

  b->focus = gtk_event_controller_focus_new ();
  g_signal_connect (b->focus, "enter", G_CALLBACK (on_focus_enter), b);
  g_signal_connect (b->focus, "leave", G_CALLBACK (on_focus_leave), b);
  gtk_widget_add_controller (b->text, b->focus);

  gtk_box_append (GTK_BOX (self->blocks_box), b->row);
  g_ptr_array_add (self->blocks, b);
  return b;
}

static void
on_entry_loaded (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  add_block (user, id, stamped_at, text);
}

static Block *
last_block (JrDayView *self)
{
  return self->blocks->len > 0 ? g_ptr_array_index (self->blocks, self->blocks->len - 1) : NULL;
}

static void
focus_block_end (Block *b)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (b->buffer, &end);
  gtk_text_buffer_place_cursor (b->buffer, &end);
  gtk_widget_grab_focus (b->text);
}

void
jr_day_view_focus (JrDayView *self)
{
  Block *b = last_block (self);
  if (b != NULL && self->editable)
    focus_block_end (b);
}

gboolean
jr_day_view_is_current_today (JrDayView *self)
{
  return self->is_today && jr_day_compare (self->day, jr_day_today ()) == 0;
}

void
jr_day_view_new_entry (JrDayView *self)
{
  if (!self->is_today)
    return;
  Block *last = last_block (self);
  if (last != NULL)
    {
      GtkTextIter s, e;
      gtk_text_buffer_get_bounds (last->buffer, &s, &e);
      if (gtk_text_iter_equal (&s, &e))
        {
          /* The last block is still empty: reuse it as the new draft. */
          if (last->id != 0)
            {
              jr_journal_delete_entry (journal_of (self), last->id);
              last->id = 0;
            }
          last->stamped_at = 0;
          last->dirty = FALSE;
          gtk_widget_add_css_class (last->row, "draft");
          set_stamp_label (last);
          focus_block_end (last);
          return;
        }
    }
  jr_day_view_save (self);
  focus_block_end (add_block (self, 0, 0, NULL));
}

static void
on_new_clicked (GtkButton *button, gpointer data)
{
  (void) button;
  jr_day_view_new_entry (data);
}

void
jr_day_view_open_days (JrDayView *self)
{
  gtk_menu_button_popup (GTK_MENU_BUTTON (self->day_button));
}

/* ---- live clock and stats ------------------------------------------- */

static void
set_minutes (GtkWidget *label, gint64 seconds)
{
  char buf[32];
  jr_format_minutes (jr_minutes_from_seconds (seconds), buf, sizeof buf);
  if (g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), buf) != 0)
    gtk_label_set_text (GTK_LABEL (label), buf);
}

static void
update_stats (JrDayView *self)
{
  if (self->is_today)
    set_minutes (self->session_value, jr_window_session_seconds (self->win));
  set_minutes (self->today_value, jr_window_day_seconds (self->win, self->day_iso));
}

gboolean
jr_day_view_tick (JrDayView *self, gint64 now)
{
  if (!self->is_today)
    return FALSE;

  /* Redraw the clock and draft stamps only when the minute changes. */
  int minute = (int) (now / 60);
  if (minute != self->clock_minute)
    {
      self->clock_minute = minute;
      char clock[16];
      jr_format_clock (now, clock, sizeof clock);
      gtk_label_set_text (GTK_LABEL (self->clock), clock);
      for (guint i = 0; i < self->blocks->len; i++)
        {
          Block *b = g_ptr_array_index (self->blocks, i);
          if (b->stamped_at == 0)
            set_stamp_label (b);
        }
    }
  update_stats (self);

  /* After midnight, move to the new day once the writer pauses. Blocks
   * already carry their own day, so nothing is misfiled meanwhile. */
  return jr_day_compare (jr_day_from_time (now), self->day) != 0 &&
         now - self->last_edit > JR_IDLE_LIMIT_SECS;
}

/* ---- past days ------------------------------------------------------- */

static void
on_edit_clicked (GtkButton *button, gpointer data)
{
  JrDayView *self = data;
  self->editable = TRUE;
  for (guint i = 0; i < self->blocks->len; i++)
    {
      Block *b = g_ptr_array_index (self->blocks, i);
      gtk_text_view_set_editable (GTK_TEXT_VIEW (b->text), TRUE);
      gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (b->text), TRUE);
      gtk_widget_remove_css_class (b->text, "readonly");
    }
  gtk_widget_set_visible (GTK_WIDGET (button), FALSE);
  gtk_label_set_text (GTK_LABEL (self->sub), "Editing · the original time stamps stay as they were");
  jr_day_view_focus (self);
}

static void
on_back_today (GtkButton *button, gpointer data)
{
  (void) button;
  JrDayView *self = data;
  jr_window_show_today (self->win);
}

static void
on_activity_clicked (GtkButton *button, gpointer data)
{
  (void) button;
  JrDayView *self = data;
  jr_window_show_activity (self->win);
}

static void
on_security_clicked (GtkButton *button, gpointer data)
{
  (void) button;
  JrDayView *self = data;
  jr_window_show_security (self->win);
}

/* ---- construction ---------------------------------------------------- */

static GtkWidget *
build_header (JrDayView *self)
{
  GtkWidget *header = jr_header_new (NULL);

  char date[32];
  jr_day_format_long (self->day, date, sizeof date);
  self->day_button = gtk_menu_button_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (box), gtk_label_new (date));
  gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("jr-chevron-down-symbolic"));
  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->day_button), box);
  gtk_widget_set_tooltip_text (self->day_button, "Open another day (Ctrl+K)");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->day_button),
                               jr_day_popover_new (self->win, self->day, self->scroller));
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), self->day_button);

  if (self->is_today)
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), jr_label ("Today", "title-text", NULL));
  else
    {
      GtkWidget *back = gtk_button_new_with_label ("Back to today");
      gtk_widget_add_css_class (back, "flat");
      g_signal_connect (back, "clicked", G_CALLBACK (on_back_today), self);
      gtk_header_bar_pack_start (GTK_HEADER_BAR (header), back);
    }

  /* pack_end runs right to left. */
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), jr_header_sep ());
  GtkWidget *lock = jr_button ("jr-lock-symbolic", "Lock");
  gtk_widget_add_css_class (lock, "flat");
  gtk_widget_set_tooltip_text (lock, "Lock and security");
  g_signal_connect (lock, "clicked", G_CALLBACK (on_security_clicked), self);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), lock);
  GtkWidget *activity = jr_button ("jr-chart-symbolic", "Activity");
  gtk_widget_add_css_class (activity, "flat");
  g_signal_connect (activity, "clicked", G_CALLBACK (on_activity_clicked), self);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), activity);
  return header;
}

static GtkWidget *
build_top (JrDayView *self)
{
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand (left, TRUE);
  char text[96];

  if (self->is_today)
    {
      self->clock = jr_label ("", "clock", NULL);
      self->clock_minute = -1;
      g_snprintf (text, sizeof text, "%s · time comes from your laptop clock",
                  jr_weekday_name (self->day));
    }
  else
    {
      char date[32];
      g_snprintf (date, sizeof date, "%d %s", self->day.day, jr_month_name (self->day.month));
      self->clock = jr_label (date, "date-title", NULL);
      g_snprintf (text, sizeof text, "%s %d · read-only, so the time stamps stay honest",
                  jr_weekday_name (self->day), self->day.year);
    }
  self->sub = jr_label (text, "muted", "mono", NULL);
  gtk_box_append (GTK_BOX (left), self->clock);
  gtk_box_append (GTK_BOX (left), self->sub);
  gtk_box_append (GTK_BOX (top), left);

  if (self->is_today)
    gtk_box_append (GTK_BOX (top), jr_stat_new ("THIS SESSION", &self->session_value));
  gtk_box_append (GTK_BOX (top), jr_stat_new (self->is_today ? "WRITING TODAY" : "TIME WRITING",
                                              &self->today_value));
  if (!self->is_today && self->blocks->len > 0)
    {
      self->edit_button = gtk_button_new_with_label ("Edit");
      gtk_widget_set_valign (self->edit_button, GTK_ALIGN_CENTER);
      g_signal_connect (self->edit_button, "clicked", G_CALLBACK (on_edit_clicked), self);
      gtk_box_append (GTK_BOX (top), self->edit_button);
    }
  return top;
}

static GtkWidget *
build_status (JrDayView *self)
{
  GtkWidget *bar = gtk_center_box_new ();
  gtk_widget_add_css_class (bar, "statusbar");
  self->words_label = jr_label ("", NULL, NULL);
  gtk_center_box_set_start_widget (GTK_CENTER_BOX (bar), self->words_label);

  GtkWidget *saved = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->saved_icon = gtk_image_new_from_icon_name ("jr-check-symbolic");
  gtk_widget_set_visible (self->saved_icon, FALSE);
  self->saved_label = jr_label ("", NULL, NULL);
  gtk_box_append (GTK_BOX (saved), self->saved_icon);
  gtk_box_append (GTK_BOX (saved), self->saved_label);
  gtk_center_box_set_center_widget (GTK_CENTER_BOX (bar), saved);

  gtk_center_box_set_end_widget (GTK_CENTER_BOX (bar),
                                 jr_label ("Ctrl+K day · Ctrl+L lock", NULL, NULL));
  return bar;
}

JrDayView *
jr_day_view_new (JrWindow *win, JrDay day, GtkWidget **header_out)
{
  JrDayView *self = g_object_new (JR_TYPE_DAY_VIEW, NULL);
  self->win = win;
  self->day = day;
  jr_day_to_iso (day, self->day_iso);
  self->is_today = jr_day_compare (day, jr_day_today ()) == 0;
  self->editable = self->is_today;

  /* Column: top (clock or date, stats), rule, blocks, new-entry button. */
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (column, "day-column");
  gtk_widget_set_halign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (column, COLUMN_WIDTH, -1);

  self->blocks_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  jr_journal_foreach_entry (journal_of (self), self->day_iso, on_entry_loaded, self);

  gtk_box_append (GTK_BOX (column), build_top (self));
  GtkWidget *rule = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (rule, "rule");
  gtk_box_append (GTK_BOX (column), rule);
  gtk_box_append (GTK_BOX (column), self->blocks_box);

  if (self->is_today)
    {
      if (self->blocks->len == 0)
        add_block (self, 0, 0, NULL); /* cursor ready on an empty draft */
      self->new_button = gtk_button_new ();
      GtkWidget *nb = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_box_append (GTK_BOX (nb), gtk_image_new_from_icon_name ("jr-plus-symbolic"));
      gtk_box_append (GTK_BOX (nb), gtk_label_new ("New stamped entry"));
      gtk_box_append (GTK_BOX (nb), jr_label ("Ctrl+Enter", "faint", NULL));
      gtk_button_set_child (GTK_BUTTON (self->new_button), nb);
      gtk_widget_add_css_class (self->new_button, "new-entry");
      gtk_widget_set_halign (self->new_button, GTK_ALIGN_START);
      gtk_widget_set_margin_start (self->new_button, 76);
      g_signal_connect (self->new_button, "clicked", G_CALLBACK (on_new_clicked), self);
      gtk_box_append (GTK_BOX (column), self->new_button);
    }
  else if (self->blocks->len == 0)
    {
      self->empty_label = jr_label ("Nothing was written on this day.", "empty-day", "mono", NULL);
      gtk_box_append (GTK_BOX (column), self->empty_label);
    }

  self->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (self->scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), column);
  gtk_box_append (GTK_BOX (self), self->scroller);
  gtk_box_append (GTK_BOX (self), build_status (self));

  update_words (self);
  update_stats (self);
  jr_day_view_tick (self, jr_now ());
  *header_out = build_header (self);
  return self;
}

/* GtkTextView (GTK 4.22) wraps freshly loaded text while it is being
 * allocated, and the resize it asks for at that moment is dropped, so a
 * loaded block can stay one line tall although its text is laid out on
 * several. Each frame, compare every block's height with the bottom of
 * its laid-out text and ask for a resize from outside allocation. Stops
 * once all blocks fit (normally after a frame or two), so the frame clock
 * is not kept running. */
#define REMEASURE_MAX_FRAMES 300

static gboolean
remeasure_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void) clock; (void) data;
  JrDayView *self = JR_DAY_VIEW (widget);
  gboolean pending = FALSE;

  for (guint i = 0; i < self->blocks->len; i++)
    {
      Block *b = g_ptr_array_index (self->blocks, i);
      GtkTextView *tv = GTK_TEXT_VIEW (b->text);
      if (gtk_widget_get_width (b->text) <= 1)
        {
          pending = TRUE; /* not laid out yet */
          continue;
        }
      GtkTextIter end;
      int y = 0, h = 0;
      gtk_text_buffer_get_end_iter (b->buffer, &end);
      gtk_text_view_get_line_yrange (tv, &end, &y, &h);
      int needed = y + h + gtk_text_view_get_top_margin (tv) + gtk_text_view_get_bottom_margin (tv);
      if (gtk_widget_get_height (b->text) < needed)
        {
          gtk_widget_queue_resize (b->text);
          pending = TRUE;
        }
    }

  if (pending && ++self->remeasure_frames < REMEASURE_MAX_FRAMES)
    return G_SOURCE_CONTINUE;
  self->remeasure_id = 0;
  return G_SOURCE_REMOVE;
}

static void
on_view_map (GtkWidget *widget, gpointer data)
{
  (void) data;
  JrDayView *self = JR_DAY_VIEW (widget);
  if (self->remeasure_id == 0)
    self->remeasure_id = gtk_widget_add_tick_callback (widget, remeasure_tick, NULL, NULL);
}

static void
jr_day_view_dispose (GObject *object)
{
  JrDayView *self = JR_DAY_VIEW (object);
  g_clear_handle_id (&self->save_id, g_source_remove);
  g_clear_handle_id (&self->words_id, g_source_remove);
  if (self->remeasure_id != 0)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->remeasure_id);
  self->remeasure_id = 0;
  /* Wipe and free blocks while their buffers still exist. */
  g_clear_pointer (&self->blocks, g_ptr_array_unref);
  G_OBJECT_CLASS (jr_day_view_parent_class)->dispose (object);
}

static void
jr_day_view_class_init (JrDayViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = jr_day_view_dispose;
}

static void
jr_day_view_init (JrDayView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  self->blocks = g_ptr_array_new_with_free_func (block_free);
  g_signal_connect (self, "map", G_CALLBACK (on_view_map), NULL);
}
