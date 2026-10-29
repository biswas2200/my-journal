/* md_view: Markdown shown formatted, read-only (the AI's enhanced copy,
 * a topic reference).
 *
 * Headings, bullets at their depth, numbered items, arrows, bold,
 * italic, inline code, code blocks with their language, citations [1]
 * set small and raised, http(s) links that open in the browser, and
 * diagrams: a ```svg block is drawn only after svgsafe.h cleaned it,
 * else a short note says why it is not shown. No markup characters are
 * left on screen. */
#pragma once

#include <gtk/gtk.h>

#define JN_TYPE_MD_VIEW (jn_md_view_get_type ())
G_DECLARE_FINAL_TYPE (JnMdView, jn_md_view, JN, MD_VIEW, GtkTextView)

GtkWidget  *jn_md_view_new          (void);
void        jn_md_view_set_markdown (JnMdView *v, const char *markdown);
const char *jn_md_view_get_markdown (JnMdView *v);
/* Diagrams drawn, and refused, in the last set_markdown (for tests). */
int         jn_md_view_diagrams     (JnMdView *v, int *refused);
/* The address of the link at a buffer offset, or NULL (for tests). */
const char *jn_md_view_link_at      (JnMdView *v, int offset);
