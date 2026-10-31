/* notes_window: see notes_window.h. */
#include "notes_window.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include "clock_bar.h"
#include "md_view.h"
#include "notebackup.h"
#include "noteexport.h"
#include "programs.h"

#define SAVE_DELAY_MS 1000
#define RETRY_DELAY_MS 5000 /* after a failed save */

struct _JnWindow {
  GtkApplicationWindow parent_instance;

  JnDb        *db;
  char        *export_dir;
  gint64       subject, topic, session;
  gboolean     subject_picked; /* the subject was clicked last (for + Topic) */
  guint        save_id;
  gboolean     dirty;
  gboolean     rescued;        /* the unsaved text is in rescue_path */
  gint64       rescue_session; /* the session rescue_path belongs to */
  char        *rescue_path;
  char        *backup_dir;
  JnLeaveFunc  leave;
  gpointer     leave_user;

  GtkWidget   *nav, *title, *markdown, *export_button;
  GtkWidget   *list, *add_topic;
  GtkWidget   *span, *new_session, *status, *empty, *studied;
  JnClockBar  *clock;          /* study stopwatch and timer */
  JnEditor    *editor;

  JnEnhancer   *enhancer;
  GCancellable *ai_cancel;
  gint64        enhancing;     /* the session being enhanced, or 0 */
  GtkWidget    *enhance_button, *split, *ai_pane, *ai_title;
  JnMdView     *ai_view;
  gboolean      reference;     /* the topic reference is showing */
};

G_DEFINE_FINAL_TYPE (JnWindow, jn_window, GTK_TYPE_APPLICATION_WINDOW)

typedef enum { ROW_SUBJECT, ROW_TOPIC, ROW_SESSION, ROW_REFERENCE } RowKind;

static gint64
now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static void rebuild (JnWindow *win);
static int copies_of (JnWindow *win, gint64 topic);
static char *reference_markdown (JnWindow *win, gint64 topic);

/* ---- header, span and status ---------------------------------------- */

static void
set_status (JnWindow *win, const char *text)
{
  gtk_label_set_text (GTK_LABEL (win->status), text != NULL ? text : "");
  gtk_widget_set_tooltip_text (win->status, text); /* the whole message, when it is cut */
  gtk_widget_remove_css_class (win->status, "error");
}

/* A message that must not be missed: in red, with the bell. */
static void
set_alarm (JnWindow *win, const char *text)
{
  set_status (win, text);
  gtk_widget_add_css_class (win->status, "error");
  gtk_widget_error_bell (win->status);
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
  else if (win->reference)
    gtk_label_set_text (GTK_LABEL (win->span), "Topic reference \xc2\xb7 every enhanced copy, oldest first \xc2\xb7 read-only");
  else if (win->topic != 0)
    gtk_label_set_text (GTK_LABEL (win->span), "New session \xc2\xb7 starts when you type");
  else
    gtk_label_set_text (GTK_LABEL (win->span), "");
}

static gint64
midnight (void)
{
  g_autoptr (GDateTime) n = g_date_time_new_now_local ();
  g_autoptr (GDateTime) m = g_date_time_new_local (g_date_time_get_year (n), g_date_time_get_month (n),
                                                   g_date_time_get_day_of_month (n), 0, 0, 0);
  return g_date_time_to_unix (m);
}

/* "Today 25 min · 3 h 10 min in all" for the open topic. */
static void
update_studied (JnWindow *win)
{
  gint64 all = win->topic != 0 ? jn_db_studied (win->db, win->topic, 0) : 0;
  g_autofree char *text = NULL;
  if (win->topic == 0)
    text = g_strdup ("");
  else if (all == 0)
    text = g_strdup ("Studied: no time yet");
  else
    {
      g_autofree char *today = jn_format_studied (jn_db_studied (win->db, win->topic, midnight ()));
      g_autofree char *total = jn_format_studied (all);
      text = g_strdup_printf ("Today %s \xc2\xb7 %s in all", today, total);
    }
  gtk_label_set_text (GTK_LABEL (win->studied), text);
  gtk_widget_set_tooltip_text (win->studied, text);
}

/* Without a topic there is nothing to write in. */
static void
update_page (JnWindow *win)
{
  gboolean open = win->topic != 0;
  if (!open && jn_clock_bar_running (win->clock))
    jn_clock_bar_pause (win->clock);
  gtk_widget_set_sensitive (GTK_WIDGET (win->clock), open);
  update_studied (win);
  gtk_widget_set_visible (win->empty, !open);
  gtk_widget_set_visible (GTK_WIDGET (win->editor), open && !win->reference);
  gtk_widget_set_sensitive (win->new_session, open && (win->session != 0 || win->reference));
  gtk_widget_set_sensitive (win->export_button, open);
  gtk_widget_set_sensitive (win->enhance_button, open && !win->reference && win->session != 0 &&
                                                 win->enhancer != NULL && win->enhancing == 0);
  gtk_widget_set_sensitive (win->add_topic, win->subject != 0);
  jn_editor_set_editable (win->editor, open);
  update_title (win);
  update_span (win);
}

/* ---- saving ------------------------------------------------------------ */

/* The open note as Markdown in ~/Documents/Notes/Unsaved, one file per
 * session, rewritten in full each time: so a failed save still leaves
 * the text on disk. Reads the store only for names and times. */
static gboolean
rescue (JnWindow *win, const char *body, const char *sources, GError **error)
{
  gint64 subject = 0, t = now ();
  g_autofree char *sn = NULL, *tn = NULL;
  if (!jn_db_get_topic (win->db, win->topic, &subject, &sn, &tn))
    {
      sn = g_strdup ("Notes");
      tn = g_strdup ("Untitled");
    }
  JnSession s = { 0 };
  gint64 started = jn_db_get_session (win->db, win->session, &s) ? s.started : t;
  jn_session_clear (&s);
  JnExport *e = jn_export_new (sn, tn);
  jn_export_add_session (e, started, t, sources, body);
  g_autofree char *md = jn_export_finish (e);
  if (win->rescue_session != win->session)
    {
      g_clear_pointer (&win->rescue_path, g_free);
      win->rescue_session = win->session;
    }
  g_autofree char *dir = g_build_filename (win->export_dir, "Unsaved", NULL);
  g_autofree char *name = win->rescue_path != NULL ? g_path_get_basename (win->rescue_path)
                                                   : jn_rescue_file_name (sn, tn, t);
  if (!jn_rescue_write (dir, name, md, error))
    return FALSE;
  if (win->rescue_path == NULL)
    win->rescue_path = g_build_filename (dir, name, NULL);
  return TRUE;
}

static gboolean on_save_timeout (gpointer data);

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
      win->rescued = FALSE;
      g_autoptr (GDateTime) t = g_date_time_new_now_local ();
      g_autofree char *at = g_date_time_format (t, "Saved %H:%M");
      if (win->rescue_path != NULL && win->rescue_session == win->session)
        {
          g_autofree char *msg = g_strdup_printf ("%s. Saving works again; the copy in Documents/Notes/Unsaved is "
                                                  "no longer needed.", at);
          set_status (win, msg);
        }
      else
        set_status (win, at);
    }
  else
    {
      /* The text must be on disk somewhere, now; then keep trying. */
      g_autoptr (GError) error = NULL;
      win->rescued = rescue (win, body, sources, &error);
      if (win->rescued)
        {
          g_autofree char *name = g_path_get_basename (win->rescue_path);
          g_autofree char *msg = g_strdup_printf ("Could not save to the notes file. Your text is safe in "
                                                  "Documents/Notes/Unsaved/%s; Notes keeps trying.", name);
          set_alarm (win, msg);
        }
      else
        {
          g_autofree char *msg = g_strdup_printf ("Could not save, and no copy could be written (%s): your latest "
                                                  "text is not saved. Keep Notes open and copy the text somewhere "
                                                  "safe.", error->message);
          set_alarm (win, msg);
        }
      win->save_id = g_timeout_add (RETRY_DELAY_MS, on_save_timeout, win);
    }
  update_span (win);
}

/* Before the page shows something else: save, and forget an empty
 * session. */
static void
close_page (JnWindow *win)
{
  jn_clock_bar_flush (win->clock); /* study time so far, to this topic */
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

static void show_enhanced (JnWindow *win);

static void
show_page (JnWindow *win, const char *body, const char *sources)
{
  win->reference = FALSE;
  jn_editor_load (win->editor, body, sources);
  win->dirty = FALSE;
  update_page (win);
  show_enhanced (win);
  rebuild (win);
  if (win->topic != 0)
    jn_editor_focus (win->editor);
}

void
jn_window_select_subject (JnWindow *win, gint64 subject)
{
  win->subject = subject;
  win->subject_picked = TRUE;
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
  win->subject_picked = FALSE;
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
  /* The page's own topic and subject (a subject picked for + Topic may
   * have been another). */
  gint64 subject = 0;
  g_autofree char *sn = NULL, *tn = NULL;
  jn_db_get_topic (win->db, s.topic, &subject, &sn, &tn);
  win->topic = s.topic;
  win->subject = subject;
  win->subject_picked = FALSE;
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
  win->subject_picked = FALSE;
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
  if (win->reference)
    {
      g_autofree char *ref = reference_markdown (win, win->topic);
      g_autofree char *folder_file = jn_export_file_name (sn);
      folder_file[strlen (folder_file) - 3] = '\0';
      g_autofree char *dir = g_build_filename (win->export_dir, folder_file, NULL);
      if (g_mkdir_with_parents (dir, 0700) != 0)
        {
          int e2 = errno;
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e2), "Cannot create %s: %s", dir, g_strerror (e2));
          return NULL;
        }
      g_autofree char *named = g_strdup_printf ("%s (reference)", tn);
      g_autofree char *file = jn_export_file_name (named);
      char *path = g_build_filename (dir, file, NULL);
      if (!g_file_set_contents_full (path, ref, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
        g_clear_pointer (&path, g_free);
      return path;
    }
  JnExport *e = jn_export_new (sn, tn);
  jn_export_set_studied (e, jn_db_studied (win->db, win->topic, 0));
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

/* Row marks: "current" is the one row highlighted, the thing clicked
 * last; "open" marks the subject and topic of the page on the right. */
typedef enum { PLAIN, OPEN, CURRENT } RowMark;

static GtkWidget *
row (JnWindow *win, RowKind kind, gint64 id, const char *text, RowMark mark)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  GtkWidget *r = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (r), label);
  static const char *css[] = { "subject-row", "topic-row", "session-row", "reference-row" };
  gtk_widget_add_css_class (r, css[kind]);
  if (mark == CURRENT)
    gtk_widget_add_css_class (r, "current");
  else if (mark == OPEN)
    gtk_widget_add_css_class (r, "open");
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
  gboolean shown = id == win->session && !win->reference;
  row (win, ROW_SESSION, id, span, shown && !win->subject_picked ? CURRENT : PLAIN);
}

static void
add_topic_row (gint64 id, const char *name, gpointer user)
{
  JnWindow *win = user;
  row (win, ROW_TOPIC, id, name, id == win->topic ? OPEN : PLAIN);
  if (id == win->topic)
    {
      RowMark page = win->subject_picked ? PLAIN : CURRENT; /* what the page shows */
      if (copies_of (win, id) > 0)
        row (win, ROW_REFERENCE, id, "\xe2\x98\x85 Topic reference", win->reference ? page : PLAIN);
      jn_db_foreach_session (win->db, id, add_session_row, win);
      if (win->session == 0 && !win->reference)
        row (win, ROW_SESSION, 0, "new session", page);
    }
}

static void
add_subject_row (gint64 id, const char *name, gpointer user)
{
  JnWindow *win = user;
  RowMark mark = PLAIN;
  if (id == win->subject && (win->subject_picked || win->topic == 0))
    mark = CURRENT;
  else if (win->topic != 0 && id == win->subject)
    mark = OPEN;
  row (win, ROW_SUBJECT, id, name, mark);
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
    case ROW_REFERENCE: jn_window_open_reference (win, id); break;
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

/* ---- the AI enhanced copy ---------------------------------------------------- */

/* The AI pane takes the room your note does not need (the divider can be
 * moved). */
static void
open_ai_pane (JnWindow *win)
{
  gtk_widget_set_visible (win->ai_pane, TRUE);
}

static void
show_enhanced (JnWindow *win)
{
  g_autofree char *model = NULL;
  gint64 at = 0;
  g_autofree char *md = win->session != 0 ? jn_db_get_enhanced (win->db, win->session, &model, &at) : NULL;
  if (md == NULL)
    {
      gtk_widget_set_visible (win->ai_pane, FALSE);
      return;
    }
  open_ai_pane (win);
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (at);
  g_autofree char *when = t != NULL ? g_date_time_format (t, "%-d %b, %H:%M") : g_strdup ("");
  g_autofree char *title = g_strdup_printf ("\xe2\x9c\xa6 Enhanced copy \xc2\xb7 read-only \xc2\xb7 %s \xc2\xb7 %s",
                                            model, when);
  gtk_label_set_text (GTK_LABEL (win->ai_title), title);
  jn_md_view_set_markdown (win->ai_view, md);
}

char *
jn_window_enhanced_text (JnWindow *win)
{
  if (!gtk_widget_get_visible (win->ai_pane))
    return NULL;
  return g_strdup (jn_md_view_get_markdown (win->ai_view));
}

typedef struct {
  JnExport *e;
  int       n;
} Ref;

static void
add_copy (gint64 session, gint64 started, gint64 ended, const char *markdown, gpointer user)
{
  (void) session;
  Ref *r = user;
  r->n++;
  if (r->e != NULL)
    jn_export_reference_add (r->e, started, ended, markdown);
}

static int
copies_of (JnWindow *win, gint64 topic)
{
  Ref r = { NULL, 0 };
  jn_db_foreach_enhanced (win->db, topic, add_copy, &r);
  return r.n;
}

static char *
reference_markdown (JnWindow *win, gint64 topic)
{
  gint64 subject = 0;
  g_autofree char *sn = NULL, *tn = NULL;
  if (!jn_db_get_topic (win->db, topic, &subject, &sn, &tn))
    return NULL;
  Ref r = { jn_export_reference_new (sn, tn), 0 };
  jn_db_foreach_enhanced (win->db, topic, add_copy, &r);
  return jn_export_finish (r.e);
}

void
jn_window_open_reference (JnWindow *win, gint64 topic)
{
  close_page (win);
  g_autofree char *md = reference_markdown (win, topic);
  if (md == NULL)
    return;
  gint64 subject = 0;
  g_autofree char *sn = NULL, *tn = NULL;
  jn_db_get_topic (win->db, topic, &subject, &sn, &tn);
  win->topic = topic;
  win->subject = subject;
  win->session = 0;
  win->subject_picked = FALSE;
  win->reference = TRUE;
  g_autofree char *title = g_strdup_printf ("\xe2\x98\x85 Topic reference \xc2\xb7 %s \xc2\xb7 read-only", tn);
  gtk_label_set_text (GTK_LABEL (win->ai_title), title);
  jn_md_view_set_markdown (win->ai_view, md);
  open_ai_pane (win);
  update_page (win);
  rebuild (win);
}

gboolean
jn_window_in_reference (JnWindow *win)
{
  return win->reference;
}

gboolean
jn_window_is_enhancing (JnWindow *win)
{
  return win->enhancing != 0;
}

void
jn_window_set_enhancer (JnWindow *win, JnEnhancer *enhancer)
{
  if (win->enhancer != NULL)
    win->enhancer->free (win->enhancer);
  win->enhancer = enhancer;
  update_page (win);
}

typedef struct {
  JnWindow *win;   /* a reference */
  gint64    session, topic;
  char     *span;
} Pending;

static void
on_enhanced (const JnAiAnswer *answer, const char *error, gpointer data)
{
  Pending *p = data;
  JnWindow *win = p->win;
  if (win->db != NULL)
    {
      win->enhancing = 0;
      if (error != NULL)
        {
          g_autofree char *msg = g_strdup_printf ("Not enhanced: %s", error);
          set_status (win, msg);
        }
      else
        {
          gint64 subject = 0;
          g_autofree char *sn = NULL, *tn = NULL;
          jn_db_get_topic (win->db, p->topic, &subject, &sn, &tn);
          g_autofree char *md = jn_ai_enhanced_copy (tn != NULL ? tn : "", sn != NULL ? sn : "", p->span,
                                                    answer->model != NULL ? answer->model : JN_AI_MODEL,
                                                    answer->sources, answer->n_sources, answer->markdown);
          gint64 now_s = g_get_real_time () / G_USEC_PER_SEC;
          if (jn_db_set_enhanced (win->db, p->session, md, answer->model != NULL ? answer->model : JN_AI_MODEL, now_s))
            set_status (win, "Enhanced copy saved next to your note (your note is unchanged).");
          else
            set_status (win, "The enhanced copy could not be saved (the session was deleted?).");
          if (p->session == win->session)
            show_enhanced (win);
          rebuild (win); /* the topic now has a reference */
        }
      gtk_button_set_label (GTK_BUTTON (win->enhance_button), "\xe2\x9c\xa6 Enhance");
      update_page (win);
    }
  g_free (p->span);
  g_object_unref (p->win);
  g_free (p);
}

static void
start_enhancing (JnWindow *win)
{
  jn_window_save (win);
  JnSession s = { 0 };
  gint64 subject = 0;
  g_autofree char *sn = NULL, *tn = NULL;
  if (win->session == 0 || !jn_db_get_session (win->db, win->session, &s) ||
      !jn_db_get_topic (win->db, s.topic, &subject, &sn, &tn))
    {
      jn_session_clear (&s);
      return;
    }
  g_autofree char *span = jn_format_span (s.started, s.ended);
  g_auto (GStrv) links = g_strsplit (s.sources, "\n", -1);
  g_auto (GStrv) style = jn_db_style_samples (win->db, s.topic, 3, 3000);
  JnAiInput in = { sn, tn, span, s.body, NULL, 0, (const char *const *) style };
  Pending *p = g_new0 (Pending, 1);
  p->win = g_object_ref (win);
  p->session = win->session;
  p->topic = s.topic;
  p->span = g_strdup (span);
  win->enhancing = win->session;
  g_clear_object (&win->ai_cancel);
  win->ai_cancel = g_cancellable_new ();
  gtk_button_set_label (GTK_BUTTON (win->enhance_button), "Enhancing\xe2\x80\xa6");
  gboolean has_links = FALSE;
  for (char **l = links; *l != NULL && !has_links; l++)
    has_links = *g_strstrip (*l) != '\0';
  set_status (win, has_links ? "Enhancing: reading your sources, then asking the AI. Keep writing meanwhile."
                             : "Enhancing: asking the AI (no sources, so it works from what is well known). "
                               "Keep writing meanwhile.");
  update_page (win);
  win->enhancer->start (win->enhancer, &in, links, win->ai_cancel, on_enhanced, p);
  jn_session_clear (&s);
}

static void
on_consent (GObject *src, GAsyncResult *res, gpointer data)
{
  JnWindow *win = data;
  int choice = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (src), res, NULL);
  if (choice == 1 && win->db != NULL)
    {
      jn_db_set_setting (win->db, "ai_consent", "yes");
      start_enhancing (win);
    }
  g_object_unref (win);
}

JnClockBar *
jn_window_clock_bar (JnWindow *win)
{
  return win->clock;
}

const char *
jn_window_studied (JnWindow *win)
{
  return gtk_label_get_text (GTK_LABEL (win->studied));
}

const char *
jn_window_status (JnWindow *win)
{
  return gtk_label_get_text (GTK_LABEL (win->status));
}

gboolean
jn_window_enhance (JnWindow *win)
{
  if (win->enhancer == NULL || win->enhancing != 0 || win->topic == 0)
    return FALSE;
  g_autofree char *body = jn_editor_body (win->editor);
  if (win->session == 0 || *g_strstrip (body) == '\0')
    {
      set_status (win, "Write something first: the AI works from your own note.");
      return FALSE;
    }
  g_autofree char *consent = jn_db_get_setting (win->db, "ai_consent");
  if (g_strcmp0 (consent, "yes") != 0)
    {
      g_autofree char *where = win->enhancer->describe (win->enhancer);
      GtkAlertDialog *d = gtk_alert_dialog_new ("Send this note to the AI?");
      g_autofree char *detail = g_strdup_printf (
        "To write the enhanced copy, Notes sends this session's text, its subject and topic, the pages in Sources "
        "(if you added any), "
        "and a few of your other notes (so it can write in your style) to Google: %s, using your Google sign-in.\n\n"
        "Your journal is never sent: Notes cannot even read it. Your note itself is never changed.", where);
      gtk_alert_dialog_set_detail (d, detail);
      const char *buttons[] = { "Cancel", "Send to the AI", NULL };
      gtk_alert_dialog_set_buttons (d, buttons);
      gtk_alert_dialog_set_cancel_button (d, 0);
      gtk_alert_dialog_set_default_button (d, 1);
      gtk_alert_dialog_choose (d, GTK_WINDOW (win), NULL, on_consent, g_object_ref (win));
      g_object_unref (d);
      return FALSE;
    }
  start_enhancing (win);
  return TRUE;
}

static void
on_enhance (GtkButton *b, gpointer data)
{
  (void) b;
  jn_window_enhance (data);
}

static void
on_hide_ai (GtkButton *b, gpointer data)
{
  (void) b;
  JnWindow *win = data;
  gtk_widget_set_visible (win->ai_pane, FALSE);
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
  JnWindow *win = JN_WINDOW (window);
  close_page (win);
  /* Text that is neither saved nor rescued: Notes stays open. */
  if (win->dirty && !win->rescued)
    return TRUE;
  if (win->ai_cancel != NULL)
    g_cancellable_cancel (win->ai_cancel);
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
    case 'a': jn_window_enhance (win); break;
    case 'q': gtk_window_close (GTK_WINDOW (win)); break;
    case 't':
      if (win->topic != 0)
        jn_clock_bar_toggle (win->clock);
      break;
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
  shortcut (c, "<Control><Shift>e", sc_call, GINT_TO_POINTER ('a'), NULL);
  shortcut (c, "<Control>q", sc_call, GINT_TO_POINTER ('q'), NULL);
  shortcut (c, "<Control>t", sc_call, GINT_TO_POINTER ('t'), NULL);
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

/* Study time from the stopwatch or timer goes to the open topic. */
static void
on_study (JnClockBar *bar, gint64 seconds, gpointer data)
{
  (void) bar;
  JnWindow *win = data;
  if (win->db != NULL && win->topic != 0)
    jn_db_add_study (win->db, win->topic, now (), seconds);
  update_studied (win);
}

static void
on_timer_finished (JnClockBar *bar, gpointer data)
{
  JnWindow *win = data;
  gtk_widget_error_bell (GTK_WIDGET (bar));
  set_status (win, "Time's up. The time is logged; take a short break, or Start again.");
  GApplication *app = G_APPLICATION (gtk_window_get_application (GTK_WINDOW (win)));
  if (app != NULL)
    {
      GNotification *n = g_notification_new ("Time's up");
      g_notification_set_body (n, "Your study timer has finished. The time is logged in Notes.");
      g_application_send_notification (app, "study-timer", n);
      g_object_unref (n);
    }
}

static void
jn_window_dispose (GObject *object)
{
  JnWindow *win = JN_WINDOW (object);
  if (win->clock != NULL && win->db != NULL)
    jn_clock_bar_flush (win->clock);
  /* The day's backup, as Notes closes. */
  if (win->backup_dir != NULL && win->db != NULL)
    jn_backup_now (win->db, win->backup_dir, now (), JN_BACKUP_KEEP, NULL);
  g_clear_pointer (&win->backup_dir, g_free);
  g_clear_pointer (&win->rescue_path, g_free);
  g_clear_handle_id (&win->save_id, g_source_remove);
  if (win->ai_cancel != NULL)
    g_cancellable_cancel (win->ai_cancel);
  g_clear_object (&win->ai_cancel);
  if (win->enhancer != NULL)
    win->enhancer->free (win->enhancer);
  win->enhancer = NULL;
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
  win->enhance_button = header_button ("\xe2\x9c\xa6 Enhance", "Write an enhanced copy from your note and its sources",
                                       G_CALLBACK (on_enhance), win);
  gtk_widget_add_css_class (win->enhance_button, "enhance");
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), win->enhance_button);
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
  gtk_label_set_ellipsize (GTK_LABEL (win->span), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (win->span, "faint");
  gtk_widget_set_hexpand (win->span, TRUE);
  gtk_label_set_xalign (GTK_LABEL (win->span), 0.0f);
  gtk_box_append (GTK_BOX (info), win->span);
  win->studied = gtk_label_new (""); /* the time studied on this topic */
  gtk_label_set_ellipsize (GTK_LABEL (win->studied), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (win->studied, "faint");
  gtk_box_append (GTK_BOX (info), win->studied);
  win->new_session = header_button ("+ New session", "Start another session on this topic (Ctrl+N)",
                                    G_CALLBACK (on_new_session), win);
  gtk_box_append (GTK_BOX (info), win->new_session);
  gtk_box_append (GTK_BOX (page), info);
  /* The study clock on its own row. */
  win->clock = JN_CLOCK_BAR (jn_clock_bar_new ());
  gtk_widget_add_css_class (GTK_WIDGET (win->clock), "study-row");
  g_signal_connect (win->clock, "study", G_CALLBACK (on_study), win);
  g_signal_connect (win->clock, "finished", G_CALLBACK (on_timer_finished), win);
  gtk_box_append (GTK_BOX (page), GTK_WIDGET (win->clock));
  win->empty = gtk_label_new ("Add a subject and a topic on the left to start.");
  gtk_widget_add_css_class (win->empty, "empty-page");
  gtk_widget_set_vexpand (win->empty, TRUE);
  gtk_box_append (GTK_BOX (page), win->empty);
  win->editor = JN_EDITOR (jn_editor_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (win->editor), TRUE);
  g_signal_connect (win->editor, "changed", G_CALLBACK (on_editor_changed), win);
  /* Your note on the left, the AI's copy on the right (when there is one). */
  win->split = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child (GTK_PANED (win->split), GTK_WIDGET (win->editor));
  gtk_paned_set_resize_start_child (GTK_PANED (win->split), FALSE);
  gtk_paned_set_resize_end_child (GTK_PANED (win->split), TRUE);
  gtk_paned_set_shrink_start_child (GTK_PANED (win->split), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (win->split), TRUE); /* the copy wraps to any width */
  gtk_widget_set_size_request (GTK_WIDGET (win->editor), 380, -1); /* your note never gets squashed */
  win->ai_pane = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (win->ai_pane, "ai-pane");
  GtkWidget *ai_head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (ai_head, "ai-head");
  win->ai_title = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (win->ai_title), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (win->ai_title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (win->ai_title, TRUE);
  gtk_box_append (GTK_BOX (ai_head), win->ai_title);
  gtk_box_append (GTK_BOX (ai_head), header_button ("Hide", "Hide the enhanced copy", G_CALLBACK (on_hide_ai), win));
  gtk_box_append (GTK_BOX (win->ai_pane), ai_head);
  win->ai_view = JN_MD_VIEW (jn_md_view_new ());
  gtk_widget_add_css_class (GTK_WIDGET (win->ai_view), "ai-text");
  GtkWidget *ai_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (ai_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (ai_scroll), GTK_WIDGET (win->ai_view));
  gtk_widget_set_vexpand (ai_scroll, TRUE);
  gtk_box_append (GTK_BOX (win->ai_pane), ai_scroll);
  gtk_widget_set_visible (win->ai_pane, FALSE);
  gtk_paned_set_end_child (GTK_PANED (win->split), win->ai_pane);
  gtk_widget_set_vexpand (win->split, TRUE);
  gtk_box_append (GTK_BOX (page), win->split);
  win->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (win->status), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (win->status), PANGO_ELLIPSIZE_END); /* a long message never widens the page */
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

void
jn_window_set_backup_dir (JnWindow *win, const char *dir)
{
  g_free (win->backup_dir);
  win->backup_dir = g_strdup (dir);
  g_autoptr (GError) error = NULL;
  if (!jn_backup_now (win->db, dir, now (), JN_BACKUP_KEEP, &error))
    {
      g_autofree char *msg = g_strdup_printf ("No backup was made today: %s", error->message);
      set_alarm (win, msg);
    }
}

const char *
jn_window_rescue_path (JnWindow *win)
{
  return win->rescue_path;
}
