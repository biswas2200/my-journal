/* notebackup: see notebackup.h. */
#include "notebackup.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include "noteexport.h"

char *
jn_backup_name (gint64 now)
{
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (now);
  return g_date_time_format (t, "notes-%Y-%m-%d.db");
}

/* notes-YYYY-MM-DD.db exactly; its date sorts as text. */
static gboolean
is_backup (const char *name)
{
  static const char form[] = "notes-0000-00-00.db";
  if (strlen (name) != sizeof form - 1)
    return FALSE;
  for (gsize i = 0; form[i] != '\0'; i++)
    if (form[i] == '0' ? !g_ascii_isdigit (name[i]) : name[i] != form[i])
      return FALSE;
  return TRUE;
}

static int
newest_first (gconstpointer a, gconstpointer b)
{
  return strcmp (*(char *const *) b, *(char *const *) a);
}

char **
jn_backup_prune (const char *const *names, guint keep)
{
  GPtrArray *found = g_ptr_array_new ();
  for (const char *const *n = names; *n != NULL; n++)
    if (is_backup (*n))
      g_ptr_array_add (found, (gpointer) *n);
  g_ptr_array_sort (found, newest_first);
  keep = MAX (keep, 1); /* the copy just made always stays */
  GPtrArray *gone = g_ptr_array_new ();
  for (guint i = found->len; i > keep; i--) /* oldest first */
    g_ptr_array_add (gone, g_strdup (found->pdata[i - 1]));
  g_ptr_array_add (gone, NULL);
  g_ptr_array_unref (found);
  return (char **) g_ptr_array_free (gone, FALSE);
}

static gboolean
make_dir (const char *dir, GError **error)
{
  if (g_mkdir_with_parents (dir, 0700) == 0)
    return TRUE;
  int e = errno;
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e), "Cannot create %s: %s", dir, g_strerror (e));
  return FALSE;
}

static char **
list_dir (const char *dir)
{
  GPtrArray *names = g_ptr_array_new ();
  GDir *d = g_dir_open (dir, 0, NULL);
  for (const char *n = d != NULL ? g_dir_read_name (d) : NULL; n != NULL; n = g_dir_read_name (d))
    g_ptr_array_add (names, g_strdup (n));
  if (d != NULL)
    g_dir_close (d);
  g_ptr_array_add (names, NULL);
  return (char **) g_ptr_array_free (names, FALSE);
}

gboolean
jn_backup_now (JnDb *db, const char *dir, gint64 now, guint keep, GError **error)
{
  if (!make_dir (dir, error))
    return FALSE;
  g_autofree char *name = jn_backup_name (now);
  g_autofree char *path = g_build_filename (dir, name, NULL);
  if (!jn_db_copy_to (db, path, error))
    return FALSE;
  g_auto (GStrv) names = list_dir (dir);
  g_auto (GStrv) gone = jn_backup_prune ((const char *const *) names, keep);
  for (char **g = gone; *g != NULL; g++)
    {
      g_autofree char *old = g_build_filename (dir, *g, NULL);
      g_remove (old);
    }
  return TRUE;
}

char *
jn_backup_latest (const char *dir)
{
  g_auto (GStrv) names = list_dir (dir);
  const char *best = NULL;
  for (char **n = names; *n != NULL; n++)
    if (is_backup (*n) && (best == NULL || strcmp (*n, best) > 0))
      best = *n;
  return best != NULL ? g_build_filename (dir, best, NULL) : NULL;
}

char *
jn_rescue_file_name (const char *subject, const char *topic, gint64 now)
{
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (now);
  g_autofree char *when = g_date_time_format (t, "%Y-%m-%d %H.%M.%S");
  g_autofree char *title = g_strdup_printf ("%s - %s %s", subject, topic, when);
  return jn_export_file_name (title);
}

gboolean
jn_rescue_write (const char *dir, const char *name, const char *markdown, GError **error)
{
  if (!make_dir (dir, error))
    return FALSE;
  g_autofree char *path = g_build_filename (dir, name, NULL);
  return g_file_set_contents_full (path, markdown, -1,
                                   G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}
