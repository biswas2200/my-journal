/* notes_window: see notes_window.h. */
#include "notes_window.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include "noteexport.h"
#include "programs.h"

#define SAVE_DELAY_MS 1000

struct _JnWindow {
  GtkApplicationWindow parent_instance;

  JnDb        *db;
  char        *export_dir;
  gint64       subject, topic, session;
  guint        save_id;
  gboolean     dirty;
  JnLeaveFunc  leave;
  gpointer     leave_user;

  GtkWidget   *nav, *title, *markdown, *export_button;
  GtkWidget   *list, *add_topic;
  GtkWidget   *span, *new_session, *status, *empty;
  JnEditor    *editor;
};

G_DEFINE_FINAL_TYPE (JnWindow, jn_window, GTK_TYPE_APPLICATION_WINDOW)

typedef enum { ROW_SUBJECT, ROW_TOPIC, ROW_SESSION } RowKind;

static gint64
now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static void rebuild (JnWindow *win);

/* ---- header, span and status ---------------------------------------- */

static void
set_status (JnWindow *win, const char *text)
{
  gtk_label_set_text (GTK_LABEL (win->status), text != NULL ? text : "");
}

static void
update_title (JnWindow *win)
{
  gint64 subject = 0;
  g_autofree char *sn = NULL, *tn = NULL;
  if (win->topic != 0 && jn_db_get_topic (win->db, win->topic, &subject, &sn, &tn))
    {
      g_autofree char *t = g_strdup_printf ("Notes \xc2\xb7 %s \xe2\x80\xba %s", sn, tn);
      gtk_label_set_text (GTK_LABEL (win->title), t);
    }
  else
    gtk_label_set_text (GTK_LABEL (win->title), "Notes");
}

static void
update_span (JnWindow *win)
{
  JnSession s = { 0 };
  if (win->session != 0 && jn_db_get_session (win->db, win->session, &s))
    {
      g_autofree char *span = jn_format_span (s.started, s.ended);
      gtk_label_set_text (GTK_LABEL (win->span), span);
      jn_session_clear (&s);
    }
  else if (win->topic != 0)
    gtk_label_set_text (GTK_LABEL (win->span), "New session \xc2\xb7 starts when you type");
  else
    gtk_label_set_text (GTK_LABEL (win->span), "");
}

/* Without a topic there is nothing to write in. */
static void
update_page (JnWindow *win)
{
  gboolean open = win->topic != 0;
  gtk_widget_set_visible (win->empty, !open);
  gtk_widget_set_visible (GTK_WIDGET (win->editor), open);
  gtk_widget_set_sensitive (win->new_session, open && win->session != 0);
  gtk_widget_set_sensitive (win->export_button, open);
  gtk_widget_set_sensitive (win->add_topic, win->subject != 0);
  jn_editor_set_editable (win->editor, open);
  update_title (win);
  update_span (win);
}

/* ---- saving ------------------------------------------------------------ */

void
jn_window_save (JnWindow *win)
{
  g_clear_handle_id (&win->save_id, g_source_remove);
  if (!win->dirty || win->session == 0)
    return;
  g_autofree char *body = jn_editor_body (win->editor);
  g_autofree char *sources = jn_editor_sources (win->editor);
  if (jn_db_save_session (win->db, win->session, body, sources, now ()))
    {
      win->dirty = FALSE;
      g_autoptr (GDateTime) t = g_date_time_new_now_local ();
      g_autofree char *at = g_date_time_format (t, "Saved %H:%M");
      set_status (win, at);
    }
  else
    set_status (win, "Could not save");
  update_span (win);
}

/* Before the page shows something else: save, and forget an empty
 * session. */
static void
close_page (JnWindow *win)
{
  jn_window_save (win);
  if (win->session != 0 && jn_db_drop_if_empty (win->db, win->session))
    win->session = 0;
}

static gboolean
on_save_timeout (gpointer data)
{
  JnWindow *win = data;
  win->save_id = 0;
  jn_window_save (win);
  rebuild (win); /* the session's time in the list */
  return G_SOURCE_REMOVE;
}

/* The first keystroke on a new page starts the session. */
static void
on_editor_changed (JnEditor *editor, gpointer data)
{
  (void) editor;
  JnWindow *win = data;
  if (win->topic == 0)
    return;
  win->dirty = TRUE;
  if (win->session == 0)
    {
      win->session = jn_db_start_session (win->db, win->topic, now ());
      rebuild (win);
      update_page (win);
    }
  set_status (win, "Saving\xe2\x80\xa6");
  g_clear_handle_id (&win->save_id, g_source_remove);
  win->save_id = g_timeout_add (SAVE_DELAY_MS, on_save_timeout, win);
}

/* ---- opening things ------------------------------------------------------- */

static void
show_page (JnWindow *win, const char *body, const char *sources)
{
  jn_editor_load (win->editor, body, sources);
  win->dirty = FALSE;
  update_page (win);
  rebuild (win);
  if (win->topic != 0)
    jn_editor_focus (win->editor);
}

void
jn_window_select_subject (JnWindow *win, gint64 subject)
{
  win->subject = subject;
  update_page (win);
}

void
jn_window_open_topic (JnWindow *win, gint64 topic)
{
  close_page (win);
  gint64 subject = 0;
  g_autofree char *sn = NULL, *tn = NULL;
  if (!jn_db_get_topic (win->db, topic, &subject, &sn, &tn))
    topic = subject = 0;
  win->topic = topic;
  win->subject = subject;
  win->session = 0;
  show_page (win, "", "");
}

void
jn_window_open_session (JnWindow *win, gint64 session)
{
  close_page (win);
  JnSession s = { 0 };
  if (!jn_db_get_session (win->db, session, &s))
    return;
  win->session = session;
  if (s.topic != win->topic)
    {
      gint64 subject = 0;
      g_autofree char *sn = NULL, *tn = NULL;
      jn_db_get_topic (win->db, s.topic, &subject, &sn, &tn);
      win->topic = s.topic;
      win->subject = subject;
    }
  show_page (win, s.body, s.sources);
  jn_session_clear (&s);
  jn_editor_cursor_to_end (win->editor); /* go on where you stopped */
}

void
jn_window_new_session (JnWindow *win)
{
  if (win->topic == 0)
    return;
  close_page (win);
  win->session = 0;
  show_page (win, "", "");
}

gint64
jn_window_add_subject (JnWindow *win, const char *name)
{
  gint64 id = jn_db_add_subject (win->db, name);
  if (id != 0)
    {
      win->subject = id;
      rebuild (win);
      update_page (win);
    }
  return id;
}

gint64
jn_window_add_topic (JnWindow *win, const char *name)
{
  if (win->subject == 0)
    return 0;
  gint64 id = jn_db_add_topic (win->db, win->subject, name);
  if (id != 0)
    jn_window_open_topic (win, id);
  return id;
}

/* ---- export ---------------------------------------------------------------- */

static void
collect_ids (gint64 id, gint64 started, gint64 ended, gpointer user)
{
  (void) started; (void) ended;
  g_array_prepend_val ((GArray *) user, id); /* newest first in, oldest first out */
}

char *
jn_window_export (JnWindow *win, GError **error)
{
  gint64 subject = 0;
  g_autofree char *sn = NULL, *tn = NULL;
  if (win->topic == 0 || !jn_db_get_topic (win->db, win->topic, &subject, &sn, &tn))
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "Open a topic to export it");
      return NULL;
    }
  jn_window_save (win);
  JnExport *e = jn_export_new (sn, tn);
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  jn_db_foreach_session (win->db, win->topic, collect_ids, ids);
  for (guint i = 0; i < ids->len; i++)
    {
      JnSession s = { 0 };
      if (jn_db_get_session (win->db, g_array_index (ids, gint64, i), &s))
        jn_export_add_session (e, s.started, s.ended, s.sources, s.body);
      jn_session_clear (&s);
    }
  g_array_unref (ids);
  g_autofree char *md = jn_export_finish (e);

  g_autofree char *folder_file = jn_export_file_name (sn);
  folder_file[strlen (folder_file) - 3] = '\0'; /* the subject's folder: same rules, no ".md" */
  g_autofree char *dir = g_build_filename (win->export_dir, folder_file, NULL);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      int e2 = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e2), "Cannot create %s: %s", dir, g_strerror (e2));
      return NULL;
    }
  g_autofree char *file = jn_export_file_name (tn);
  char *path = g_build_filename (dir, file, NULL);
  if (!g_file_set_contents_full (path, md, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
    g_clear_pointer (&path, g_free);
  return path;
}

static void
on_export (GtkButton *b, gpointer data)
{
  (void) b;
  JnWindow *win = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = jn_window_export (win, &error);
  if (path == NULL)
    {
      set_status (win, error->message);
      return;
    }
  g_autofree char *home = g_strdup (g_get_home_dir ());
  g_autofree char *shown = g_str_has_prefix (path, home) ? g_strconcat ("~", path + strlen (home), NULL)
                                                         : g_strdup (path);
  g_autofree char *msg = g_strdup_printf ("Exported to %s", shown);
  set_status (win, msg);
}

/* ---- the list on the left ---------------------------------------------------- */

static GtkWidget *
row (JnWindow *win, RowKind kind, gint64 id, const char *text, gboolean current)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  GtkWidget *r = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (r), label);
  static const char *css[] = { "subject-row", "topic-row", "session-row" };
  gtk_widget_add_css_class (r, css[kind]);
  if (current)
    gtk_widget_add_css_class (r, "current");
  g_object_set_data (G_OBJECT (r), "jn-kind", GINT_TO_POINTER (kind));
  g_object_set_data_full (G_OBJECT (r), "jn-id", g_memdup2 (&id, sizeof id), g_free);
  gtk_list_box_append (GTK_LIST_BOX (win->list), r);
  return r;
}

static void
add_session_row (gint64 id, gint64 started, gint64 ended, gpointer user)
{
  JnWindow *win = user;
  g_autofree char *span = jn_format_span (started, ended);
  row (win, ROW_SESSION, id, span, id == win->session);
}

static void
add_topic_row (gint64 id, const char *name, gpointer user)
{
  JnWindow *win = user;
  row (win, ROW_TOPIC, id, name, id == win->topic);
  if (id == win->topic)
    {
      jn_db_foreach_session (win->db, id, add_session_row, win);
      if (win->session == 0)
        row (win, ROW_SESSION, 0, "new session", TRUE);
    }
}

static void
add_subject_row (gint64 id, const char *name, gpointer user)
{
  JnWindow *win = user;
  row (win, ROW_SUBJECT, id, name, id == win->subject && win->topic == 0);
  jn_db_foreach_topic (win->db, id, add_topic_row, win);
}

static void
rebuild (JnWindow *win)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (win->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (win->list), child);
  jn_db_foreach_subject (win->db, add_subject_row, win);
}

static void
on_row_activated (GtkListBox *box, GtkListBoxRow *r, gpointer data)
{
  (void) box;
  JnWindow *win = data;
  RowKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (r), "jn-kind"));
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (r), "jn-id");
  switch (kind)
    {
    case ROW_SUBJECT: jn_window_select_subject (win, id); rebuild (win); break;
    case ROW_TOPIC:   jn_window_open_topic (win, id); break;
    case ROW_SESSION: if (id != 0) jn_window_open_session (win, id); break;
    }
}

/* "+ Subject" / "+ Topic": a small popover with a name field. */
typedef struct {
  JnWindow  *win;
  gboolean   topic;
  GtkWidget *entry, *popover, *error;
} Adder;

static gboolean
focus_editor_idle (gpointer data)
{
  JnWindow *win = data;
  if (win->topic != 0)
    jn_editor_focus (win->editor);
  return G_SOURCE_REMOVE;
}

static void
on_add_activate (GtkEntry *entry, gpointer data)
{
  Adder *a = data;
  g_autofree char *name = g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry)));
  g_autofree char *trimmed = g_strstrip (g_strdup (name));
  gboolean taken = *trimmed != '\0';
  gint64 id = a->topic ? jn_window_add_topic (a->win, name) : jn_window_add_subject (a->win, name);
  if (id == 0)
    {
      gtk_label_set_text (GTK_LABEL (a->error), taken ? "That name is taken." : "Type a name first.");
      return;
    }
  gtk_editable_set_text (GTK_EDITABLE (entry), "");
  gtk_label_set_text (GTK_LABEL (a->error), "");
  gtk_popover_popdown (GTK_POPOVER (a->popover));
  /* After the popover gave focus back: straight into the new page. */
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, focus_editor_idle, g_object_ref (a->win), g_object_unref);
}

/* The name field has the keyboard as soon as the popover shows. */
static void
on_add_show (GtkWidget *popover, gpointer data)
{
  (void) popover;
  Adder *a = data;
  gtk_label_set_text (GTK_LABEL (a->error), "");
  gtk_widget_grab_focus (a->entry);
}

static GtkWidget *
adder (JnWindow *win, const char *label, gboolean topic)
{
  Adder *a = g_new0 (Adder, 1);
  a->win = win;
  a->topic = topic;
  GtkWidget *button = gtk_menu_button_new ();
  gtk_menu_button_set_label (GTK_MENU_BUTTON (button), label);
  gtk_widget_add_css_class (button, "add");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  a->entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (a->entry), topic ? "Topic, e.g. Prefix sum" : "Subject, e.g. DSA");
  g_signal_connect (a->entry, "activate", G_CALLBACK (on_add_activate), a);
  a->error = gtk_label_new ("");
  gtk_widget_add_css_class (a->error, "faint");
  gtk_box_append (GTK_BOX (box), a->entry);
  gtk_box_append (GTK_BOX (box), a->error);
  a->popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (a->popover), box);
  g_signal_connect (a->popover, "show", G_CALLBACK (on_add_show), a);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), a->popover);
  g_object_set_data_full (G_OBJECT (button), "jn-adder", a, g_free);
  return button;
}

/* ---- leaving ---------------------------------------------------------------- */

static void
on_home (GtkButton *b, gpointer win)
{
  (void) b;
  jn_window_leave (win, JR_EXIT_HOME);
}

static void
on_journal (GtkButton *b, gpointer win)
{
  (void) b;
  jn_window_leave (win, JR_EXIT_TO_DIARY);
}

void
jn_window_set_navigation (JnWindow *win, JnLeaveFunc leave, gpointer user)
{
  win->leave = leave;
  win->leave_user = user;
  gtk_widget_set_visible (win->nav, leave != NULL);
}

void
jn_window_leave (JnWindow *win, int code)
{
  JnLeaveFunc leave = win->leave;
  win->leave = NULL;
  if (leave != NULL)
    leave (code, win->leave_user);
  gtk_window_close (GTK_WINDOW (win));
}

static gboolean
on_close_request (GtkWindow *window)
{
  close_page (JN_WINDOW (window));
  return FALSE;
}

/* ---- shortcuts ------------------------------------------------------------- */

static gboolean
sc_kind (GtkWidget *w, GVariant *args, gpointer data)
{
  (void) data;
  JnWindow *win = JN_WINDOW (w);
  if (win->topic == 0)
    return FALSE;
  jn_editor_set_kind (win->editor, (JnKind) g_variant_get_int32 (args));
  return TRUE;
}

static gboolean
sc_markdown (GtkWidget *w, GVariant *args, gpointer data)
{
  (void) args; (void) data;
  JnWindow *win = JN_WINDOW (w);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (win->markdown),
                                !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (win->markdown)));
  return TRUE;
}

static gboolean
sc_call (GtkWidget *w, GVariant *args, gpointer data)
{
  (void) args;
  JnWindow *win = JN_WINDOW (w);
  switch (GPOINTER_TO_INT (data))
    {
    case 's': jn_window_save (win); break;
    case 'n': jn_window_new_session (win); break;
    case 'e': on_export (NULL, win); break;
    case 'q': gtk_window_close (GTK_WINDOW (win)); break;
    default: break;
    }
  return TRUE;
}

static void
shortcut (GtkShortcutController *c, const char *accel, GtkShortcutFunc fn, gpointer data, GVariant *args)
{
  GtkShortcut *s = gtk_shortcut_new (gtk_shortcut_trigger_parse_string (accel), gtk_callback_action_new (fn, data, NULL));
  if (args != NULL)
    gtk_shortcut_set_arguments (s, args);
  gtk_shortcut_controller_add_shortcut (c, s);
}

static void
install_shortcuts (JnWindow *win)
{
  GtkEventController *ec = gtk_shortcut_controller_new ();
  gtk_event_controller_set_propagation_phase (ec, GTK_PHASE_CAPTURE);
  GtkShortcutController *c = GTK_SHORTCUT_CONTROLLER (ec);
  shortcut (c, "<Control>1", sc_kind, NULL, g_variant_new_int32 (JN_HEADING));
  shortcut (c, "<Control>2", sc_kind, NULL, g_variant_new_int32 (JN_SUBHEADING));
  shortcut (c, "<Control>0", sc_kind, NULL, g_variant_new_int32 (JN_TEXT));
  shortcut (c, "<Control>period", sc_kind, NULL, g_variant_new_int32 (JN_BULLET));
  shortcut (c, "<Control>minus", sc_kind, NULL, g_variant_new_int32 (JN_ARROW));
  shortcut (c, "<Control>m", sc_markdown, NULL, NULL);
  shortcut (c, "<Control>s", sc_call, GINT_TO_POINTER ('s'), NULL);
  shortcut (c, "<Control>n", sc_call, GINT_TO_POINTER ('n'), NULL);
  shortcut (c, "<Control>e", sc_call, GINT_TO_POINTER ('e'), NULL);
  shortcut (c, "<Control>q", sc_call, GINT_TO_POINTER ('q'), NULL);
  gtk_widget_add_controller (GTK_WIDGET (win), ec);
}

/* ---- building ----------------------------------------------------------------- */

static void
on_markdown_toggled (GtkToggleButton *b, gpointer data)
{
  JnWindow *win = data;
  jn_window_save (win);
  jn_editor_set_markdown (win->editor, gtk_toggle_button_get_active (b));
}

static void
on_new_session (GtkButton *b, gpointer data)
{
  (void) b;
  jn_window_new_session (data);
}

static GtkWidget *
header_button (const char *label, const char *tip, GCallback cb, gpointer data)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, "nav");
  gtk_widget_set_tooltip_text (b, tip);
  g_signal_connect (b, "clicked", cb, data);
  return b;
}

static void
jn_window_dispose (GObject *object)
{
  JnWindow *win = JN_WINDOW (object);
  g_clear_handle_id (&win->save_id, g_source_remove);
  g_clear_pointer (&win->db, jn_db_close);
  g_clear_pointer (&win->export_dir, g_free);
  G_OBJECT_CLASS (jn_window_parent_class)->dispose (object);
}

static void
jn_window_class_init (JnWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = jn_window_dispose;
  GTK_WINDOW_CLASS (klass)->close_request = on_close_request;
}

static void
jn_window_init (JnWindow *win)
{
  gtk_window_set_title (GTK_WINDOW (win), "Notes");
  gtk_window_set_default_size (GTK_WINDOW (win), 1080, 680);
  gtk_widget_add_css_class (GTK_WIDGET (win), "notes");

  GtkWidget *header = gtk_header_bar_new ();
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  win->nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (win->nav), header_button ("Home", "Back to the start screen", G_CALLBACK (on_home), win));
  gtk_box_append (GTK_BOX (win->nav),
                  header_button ("Journal", "Switch to the Journal. It asks for your PIN.", G_CALLBACK (on_journal), win));
  gtk_widget_set_visible (win->nav, FALSE);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), win->nav);
  win->title = gtk_label_new ("Notes");
  gtk_widget_add_css_class (win->title, "title-text");
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), win->title);
  win->export_button = header_button ("Export .md", "Save the whole topic as Markdown (Ctrl+E)",
                                      G_CALLBACK (on_export), win);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), win->export_button);
  win->markdown = gtk_toggle_button_new_with_label ("Markdown");
  gtk_widget_add_css_class (win->markdown, "nav");
  gtk_widget_set_tooltip_text (win->markdown, "Type Markdown by hand (Ctrl+M)");
  g_signal_connect (win->markdown, "toggled", G_CALLBACK (on_markdown_toggled), win);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), win->markdown);
  gtk_window_set_titlebar (GTK_WINDOW (win), header);

  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position (GTK_PANED (paned), 250);
  gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);

  GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (side, "sidebar");
  GtkWidget *side_title = gtk_label_new ("SUBJECTS");
  gtk_label_set_xalign (GTK_LABEL (side_title), 0.0f);
  gtk_widget_add_css_class (side_title, "section-title");
  gtk_box_append (GTK_BOX (side), side_title);
  win->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (win->list), GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (win->list), TRUE);
  g_signal_connect (win->list, "row-activated", G_CALLBACK (on_row_activated), win);
  GtkWidget *side_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (side_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (side_scroll), win->list);
  gtk_widget_set_vexpand (side_scroll, TRUE);
  gtk_box_append (GTK_BOX (side), side_scroll);
  GtkWidget *adds = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (adds, "adds");
  gtk_box_append (GTK_BOX (adds), adder (win, "+ Subject", FALSE));
  win->add_topic = adder (win, "+ Topic", TRUE);
  gtk_box_append (GTK_BOX (adds), win->add_topic);
  gtk_box_append (GTK_BOX (side), adds);
  gtk_paned_set_start_child (GTK_PANED (paned), side);

  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (info, "page-info");
  win->span = gtk_label_new ("");
  gtk_widget_add_css_class (win->span, "faint");
  gtk_widget_set_hexpand (win->span, TRUE);
  gtk_label_set_xalign (GTK_LABEL (win->span), 0.0f);
  gtk_box_append (GTK_BOX (info), win->span);
  win->new_session = header_button ("+ New session", "Start another session on this topic (Ctrl+N)",
                                    G_CALLBACK (on_new_session), win);
  gtk_box_append (GTK_BOX (info), win->new_session);
  gtk_box_append (GTK_BOX (page), info);
  win->empty = gtk_label_new ("Add a subject and a topic on the left to start.");
  gtk_widget_add_css_class (win->empty, "empty-page");
  gtk_widget_set_vexpand (win->empty, TRUE);
  gtk_box_append (GTK_BOX (page), win->empty);
  win->editor = JN_EDITOR (jn_editor_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (win->editor), TRUE);
  g_signal_connect (win->editor, "changed", G_CALLBACK (on_editor_changed), win);
  gtk_box_append (GTK_BOX (page), GTK_WIDGET (win->editor));
  win->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (win->status), 0.0f);
  gtk_widget_add_css_class (win->status, "statusbar");
  gtk_box_append (GTK_BOX (page), win->status);
  gtk_paned_set_end_child (GTK_PANED (paned), page);
  gtk_window_set_child (GTK_WINDOW (win), paned);

  install_shortcuts (win);
}

JnWindow *
jn_window_new (GtkApplication *app, JnDb *db, const char *export_dir)
{
  JnWindow *win = g_object_new (JN_TYPE_WINDOW, "application", app, NULL);
  win->db = db;
  win->export_dir = g_strdup (export_dir);
  rebuild (win);
  update_page (win);
  return win;
}

gint64
jn_window_topic (JnWindow *win)
{
  return win->topic;
}

gint64
jn_window_session (JnWindow *win)
{
  return win->session;
}

JnEditor *
jn_window_editor (JnWindow *win)
{
  return win->editor;
}

JnDb *
jn_window_db (JnWindow *win)
{
  return win->db;
}
