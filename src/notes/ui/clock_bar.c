/* clock_bar: see clock_bar.h. */
#include "clock_bar.h"

#define TICK_MS 250

struct _JnClockBar {
  GtkBox     parent_instance;
  JnClock    clock;
  int        minutes;
  JnNowFunc  now;
  gpointer   now_user;
  guint      tick_id;
  GtkWidget *stopwatch, *timer, *spin, *time, *start, *reset;
};

G_DEFINE_FINAL_TYPE (JnClockBar, jn_clock_bar, GTK_TYPE_BOX)

enum { STUDY, FINISHED, N_SIGNALS };
static guint signals[N_SIGNALS];

static gint64
monotonic (gpointer user)
{
  (void) user;
  return g_get_monotonic_time ();
}

static gint64
now (JnClockBar *b)
{
  return b->now (b->now_user);
}

static void
report (JnClockBar *b)
{
  gint64 seconds = jn_clock_take_unlogged (&b->clock, now (b));
  if (seconds > 0)
    g_signal_emit (b, signals[STUDY], 0, seconds);
}

/* What is on show follows the clock: the time, Start or Pause, and the
 * mode and minutes only while stopped. */
static void
update (JnClockBar *b)
{
  gint64 t = now (b);
  g_autofree char *text = jn_clock_format (jn_clock_shown (&b->clock, t));
  gtk_label_set_text (GTK_LABEL (b->time), text);
  gboolean running = b->clock.running;
  const char *label = "Start";
  if (running)
    label = "Pause";
  else if (jn_clock_is_finished (&b->clock, t))
    label = "Start again";
  gtk_button_set_label (GTK_BUTTON (b->start), label);
  gboolean timer = b->clock.mode == JN_CLOCK_TIMER;
  gtk_widget_set_visible (b->spin, timer);
  gtk_widget_set_sensitive (b->spin, !running);
  gtk_widget_set_sensitive (b->stopwatch, !running);
  gtk_widget_set_sensitive (b->timer, !running);
  if (running)
    gtk_widget_add_css_class (b->time, "running");
  else
    gtk_widget_remove_css_class (b->time, "running");
}

void
jn_clock_bar_tick (JnClockBar *b)
{
  gint64 t = now (b);
  if (jn_clock_check_finished (&b->clock, t))
    {
      report (b);
      update (b);
      g_signal_emit (b, signals[FINISHED], 0);
      return;
    }
  /* A minute or more not yet logged: log it, so a crash loses little. */
  if (jn_clock_elapsed (&b->clock, t) - b->clock.logged >= 60 * G_USEC_PER_SEC)
    report (b);
  update (b);
}

static gboolean
on_tick (gpointer data)
{
  JnClockBar *b = data;
  jn_clock_bar_tick (b);
  if (b->clock.running)
    return G_SOURCE_CONTINUE;
  b->tick_id = 0;
  return G_SOURCE_REMOVE;
}

static void
start_ticking (JnClockBar *b)
{
  if (b->tick_id == 0)
    b->tick_id = g_timeout_add (TICK_MS, on_tick, b);
}

void
jn_clock_bar_pause (JnClockBar *b)
{
  jn_clock_pause (&b->clock, now (b));
  report (b);
  update (b);
}

void
jn_clock_bar_toggle (JnClockBar *b)
{
  if (b->clock.running)
    {
      jn_clock_bar_pause (b);
      return;
    }
  gint64 t = now (b);
  if (jn_clock_is_finished (&b->clock, t))
    jn_clock_reset (&b->clock);
  if (jn_clock_start (&b->clock, t))
    start_ticking (b);
  update (b);
}

void
jn_clock_bar_reset (JnClockBar *b)
{
  jn_clock_pause (&b->clock, now (b));
  report (b);
  jn_clock_reset (&b->clock);
  update (b);
}

static void
fresh_clock (JnClockBar *b, JnClockMode mode)
{
  if (mode == JN_CLOCK_TIMER)
    jn_clock_set_timer (&b->clock, (gint64) b->minutes * 60);
  else
    jn_clock_init (&b->clock, JN_CLOCK_STOPWATCH);
}

void
jn_clock_bar_set_mode (JnClockBar *b, JnClockMode mode)
{
  jn_clock_pause (&b->clock, now (b));
  report (b);
  fresh_clock (b, mode);
  /* The buttons follow without calling back in here. */
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (mode == JN_CLOCK_TIMER ? b->timer : b->stopwatch), TRUE);
  update (b);
}

void
jn_clock_bar_set_minutes (JnClockBar *b, int minutes)
{
  if (b->clock.running)
    return;
  b->minutes = CLAMP (minutes, 1, JN_TIMER_MAX_MINUTES);
  if (gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (b->spin)) != b->minutes)
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (b->spin), b->minutes);
  if (b->clock.mode == JN_CLOCK_TIMER)
    {
      report (b);
      fresh_clock (b, JN_CLOCK_TIMER);
    }
  update (b);
}

void
jn_clock_bar_flush (JnClockBar *b)
{
  report (b);
}

gboolean
jn_clock_bar_running (JnClockBar *b)
{
  return b->clock.running;
}

JnClockMode
jn_clock_bar_mode (JnClockBar *b)
{
  return b->clock.mode;
}

const char *
jn_clock_bar_text (JnClockBar *b)
{
  return gtk_label_get_text (GTK_LABEL (b->time));
}

void
jn_clock_bar_set_now_func (JnClockBar *b, JnNowFunc fn, gpointer user)
{
  b->now = fn != NULL ? fn : monotonic;
  b->now_user = user;
  update (b);
}

/* ---- the widget -------------------------------------------------------- */

static void
on_mode_toggled (GtkToggleButton *t, gpointer data)
{
  JnClockBar *b = data;
  JnClockMode mode = GTK_WIDGET (t) == b->timer ? JN_CLOCK_TIMER : JN_CLOCK_STOPWATCH;
  if (gtk_toggle_button_get_active (t) && b->clock.mode != mode)
    jn_clock_bar_set_mode (b, mode);
}

static void
on_minutes (GtkSpinButton *s, gpointer data)
{
  jn_clock_bar_set_minutes (data, gtk_spin_button_get_value_as_int (s));
}

static void
on_start (GtkButton *button, gpointer data)
{
  (void) button;
  jn_clock_bar_toggle (data);
}

static void
on_reset (GtkButton *button, gpointer data)
{
  (void) button;
  jn_clock_bar_reset (data);
}

static GtkWidget *
small_button (GtkWidget *b, const char *tip)
{
  gtk_widget_add_css_class (b, "nav");
  gtk_widget_set_tooltip_text (b, tip);
  gtk_widget_set_focus_on_click (b, FALSE);
  return b;
}

static void
jn_clock_bar_dispose (GObject *object)
{
  JnClockBar *b = JN_CLOCK_BAR (object);
  g_clear_handle_id (&b->tick_id, g_source_remove);
  G_OBJECT_CLASS (jn_clock_bar_parent_class)->dispose (object);
}

static void
jn_clock_bar_class_init (JnClockBarClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = jn_clock_bar_dispose;
  signals[STUDY] = g_signal_new ("study", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 1, G_TYPE_INT64);
  signals[FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);
}

static void
jn_clock_bar_init (JnClockBar *b)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (b), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing (GTK_BOX (b), 6);
  gtk_widget_add_css_class (GTK_WIDGET (b), "clock-bar");
  b->now = monotonic;
  b->minutes = 25;
  jn_clock_init (&b->clock, JN_CLOCK_STOPWATCH);

  GtkWidget *modes = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (modes, "linked");
  b->stopwatch = small_button (gtk_toggle_button_new_with_label ("Stopwatch"), "Count up while you study");
  b->timer = small_button (gtk_toggle_button_new_with_label ("Timer"), "Count down from a set time");
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b->timer), GTK_TOGGLE_BUTTON (b->stopwatch));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b->stopwatch), TRUE);
  g_signal_connect (b->stopwatch, "toggled", G_CALLBACK (on_mode_toggled), b);
  g_signal_connect (b->timer, "toggled", G_CALLBACK (on_mode_toggled), b);
  gtk_box_append (GTK_BOX (modes), b->stopwatch);
  gtk_box_append (GTK_BOX (modes), b->timer);
  gtk_box_append (GTK_BOX (b), modes);

  b->spin = gtk_spin_button_new_with_range (1, JN_TIMER_MAX_MINUTES, 5);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (b->spin), b->minutes);
  gtk_editable_set_width_chars (GTK_EDITABLE (b->spin), 3);
  gtk_widget_add_css_class (b->spin, "clock-minutes");
  gtk_widget_set_tooltip_text (b->spin, "Timer length in minutes");
  gtk_accessible_update_property (GTK_ACCESSIBLE (b->spin), GTK_ACCESSIBLE_PROPERTY_LABEL, "Timer minutes", -1);
  g_signal_connect (b->spin, "value-changed", G_CALLBACK (on_minutes), b);
  gtk_box_append (GTK_BOX (b), b->spin);

  b->time = gtk_label_new ("0:00");
  gtk_widget_add_css_class (b->time, "clock-time");
  gtk_label_set_width_chars (GTK_LABEL (b->time), 7);
  gtk_box_append (GTK_BOX (b), b->time);

  b->start = small_button (gtk_button_new_with_label ("Start"), "Start or pause (Ctrl+T)");
  g_signal_connect (b->start, "clicked", G_CALLBACK (on_start), b);
  gtk_box_append (GTK_BOX (b), b->start);
  b->reset = small_button (gtk_button_new_with_label ("Reset"), "Back to the start; the time so far is kept");
  g_signal_connect (b->reset, "clicked", G_CALLBACK (on_reset), b);
  gtk_box_append (GTK_BOX (b), b->reset);
  update (b);
}

GtkWidget *
jn_clock_bar_new (void)
{
  return g_object_new (JN_TYPE_CLOCK_BAR, NULL);
}
