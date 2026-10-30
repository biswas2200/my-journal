/* editor: writing one note, with five formats and an optional Markdown
 * mode.
 *
 * Formats (noteline.h): heading, subheading, bullet ●, sub-bullet ◦,
 * arrow →, and plain text. They are set from the toolbar, with Ctrl+1,
 * Ctrl+2, Ctrl+0 (text), Ctrl+. (bullet), Ctrl+- (arrow), or by typing a
 * marker at the start of a line ("- ", "* ", "# ", "## ", "> ", "-> ").
 * Enter continues a list, Enter on an empty item ends it, Tab and
 * Shift+Tab indent and outdent, Backspace right after a bullet turns the
 * line back into text. No syntax is ever shown.
 *
 * Markdown mode shows the stored text itself, to type Markdown by hand,
 * with the same note shown formatted below it (a live preview). Both
 * modes give the same stored lines (jn_editor_body).
 *
 * In the buffer, each formatted line starts with a short tagged prefix:
 * the visible glyph ("● ", "◦ ", "→ ") or, for headings, one invisible
 * marker character. Only tagged prefixes count, so text that merely
 * starts with "●" stays text. */
#pragma once

#include <gtk/gtk.h>
#include "noteline.h"
#include "md_view.h"

#define JN_TYPE_EDITOR (jn_editor_get_type ())
G_DECLARE_FINAL_TYPE (JnEditor, jn_editor, JN, EDITOR, GtkBox)

GtkWidget   *jn_editor_new           (void);
/* Replaces everything; not undoable, emits no "changed". */
void         jn_editor_load          (JnEditor *e, const char *body, const char *sources);
/* The stored lines and the links (one per line). */
char        *jn_editor_body          (JnEditor *e);
char        *jn_editor_sources       (JnEditor *e);
void         jn_editor_set_markdown  (JnEditor *e, gboolean on);
gboolean     jn_editor_get_markdown  (JnEditor *e);
/* Markdown mode's preview (hidden outside it). */
JnMdView    *jn_editor_preview       (JnEditor *e);
/* Sets the format of the cursor's line, or of every selected line. */
void         jn_editor_set_kind      (JnEditor *e, JnKind kind);
JnKind       jn_editor_kind_at_cursor (JnEditor *e);
void         jn_editor_set_editable  (JnEditor *e, gboolean editable);
/* To go on writing where the note ends. */
void         jn_editor_cursor_to_end (JnEditor *e);
void         jn_editor_focus         (JnEditor *e);
/* A key in the writing area (also used by tests): Return, BackSpace,
 * Tab, Shift+Tab and space at a marker. TRUE if the editor handled it. */
gboolean     jn_editor_key           (JnEditor *e, guint keyval, GdkModifierType state);
GtkTextView *jn_editor_view          (JnEditor *e);
GtkTextView *jn_editor_sources_view  (JnEditor *e);
/* Signal "changed": the person edited the note or its links. */
