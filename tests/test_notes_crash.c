/* Crash safety for Notes: a writer process is killed with SIGKILL at a
 * random moment while it saves a note, logs study time, makes the day's
 * backup and writes rescue copies, over and over.
 *
 * After every kill:
 *   - notes.db opens (it passes SQLite's check: never half-written);
 *   - the note holds at least the last version the writer reported as
 *     saved (a reported save is never lost), and never a version it had
 *     not started;
 *   - the study time is at least what was reported, never more than was
 *     started;
 *   - every backup present opens, passes its check and holds a version
 *     that was really written (a copy cut off half-way never takes a
 *     backup's place);
 *   - every rescue copy is whole: one of the versions written in full.
 *
 * The writer reports over a pipe only after a call has returned TRUE, as
 * the window does before it says "Saved". */
#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "notebackup.h"
#include "notesdb.h"

#define ROUNDS 25

/* A broken rule: says which and stops (g_error's own endless loop after
 * the message trips GCC's analyzer). */
static void G_GNUC_NORETURN G_GNUC_PRINTF (1, 2)
die (const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  g_autofree char *msg = g_strdup_vprintf (format, ap);
  va_end (ap);
  g_printerr ("notes-crash: %s\n", msg);
  abort ();
}
#define T0     G_GINT64_CONSTANT (1793514600) /* 1 Nov 2026, noon in Kolkata */

/* Version n of the note: n lines, so a half-written one would show. */
static char *
note_version (int n)
{
  GString *s = g_string_new ("# Heaps");
  for (int i = 1; i <= n; i++)
    g_string_append_printf (s, "\n- point %d: a line long enough to span pages when there are many of them", i);
  return g_string_free (s, FALSE);
}

/* The version a note body is, or -1 if it is not one of them. */
static int
version_of (const char *body)
{
  if (body == NULL || !g_str_has_prefix (body, "# Heaps"))
    return -1;
  int n = 0;
  for (const char *p = body; (p = strchr (p, '\n')) != NULL; p++)
    n++;
  g_autofree char *want = note_version (n);
  return strcmp (want, body) == 0 ? n : -1;
}

typedef enum { ACK_SAVE = 1, ACK_STUDY, ACK_RESCUE } AckKind;

typedef struct {
  gint32 kind;
  gint32 value; /* version saved, total seconds logged, version rescued */
} Ack;

static void
ack (int fd, AckKind kind, int value)
{
  Ack a = { kind, value };
  if (write (fd, &a, sizeof a) != sizeof a)
    _exit (3);
}

/* The writer: killed by the parent long before it runs out of versions
 * (each save is synced to disk, so 100 000 take minutes). */
static void G_GNUC_NORETURN
writer (const char *path, const char *backups, const char *unsaved, gint64 session, gint64 topic, int start,
        int studied, int fd)
{
  JnDb *db = jn_db_open (path, NULL);
  if (db == NULL)
    _exit (2);
  for (int v = start + 1; v <= start + 100000; v++)
    {
      g_autofree char *body = note_version (v);
      if (jn_db_save_session (db, session, body, "", T0 + v))
        ack (fd, ACK_SAVE, v);
      if (jn_db_add_study (db, topic, T0 + v, 7))
        {
          studied += 7;
          ack (fd, ACK_STUDY, studied);
        }
      if (v % 4 == 0) /* a new day every 4 versions; 3 days kept */
        jn_backup_now (db, backups, T0 + (v / 4) * 86400, 3, NULL);
      if (v % 3 == 0 && jn_rescue_write (unsaved, "Heaps.md", body, NULL))
        ack (fd, ACK_RESCUE, v);
    }
  _exit (4);
}

/* Reads every report the writer sent before it died. */
static void
read_acks (int fd, int *saved, int *study, int *rescued)
{
  Ack a;
  while (read (fd, &a, sizeof a) == sizeof a)
    switch (a.kind)
      {
      case ACK_SAVE:  *saved = a.value; break;
      case ACK_STUDY: *study = a.value; break;
      case ACK_RESCUE: *rescued = a.value; break;
      default: break;
      }
}

static void
check_backups (const char *dir, gint64 session, int ceiling)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (d == NULL)
    return;
  for (const char *name = g_dir_read_name (d); name != NULL; name = g_dir_read_name (d))
    {
      /* Only backups count: a copy cut off half-way ("….db.tmp" and its
       * "-journal") is never renamed into place, and the next backup
       * clears it. */
      gboolean is_backup = strlen (name) == strlen ("notes-2026-11-01.db") && g_str_has_prefix (name, "notes-") &&
                           g_str_has_suffix (name, ".db");
      if (!is_backup)
        {
          g_assert_true (g_str_has_suffix (name, ".db.tmp") || g_str_has_suffix (name, ".db.tmp-journal"));
          continue;
        }
      g_autofree char *file = g_build_filename (dir, name, NULL);
      g_autoptr (GError) error = NULL;
      JnDb *db = jn_db_open (file, &error);
      if (db == NULL)
        die ("backup %s does not open: %s", name, error->message);
      JnSession s = { 0 };
      g_assert_true (jn_db_get_session (db, session, &s));
      int v = version_of (s.body);
      if (v < 0 || v > ceiling)
        die ("backup %s holds a note that was never written (version %d, at most %d)", name, v, ceiling);
      jn_session_clear (&s);
      jn_db_close (db);
    }
  g_dir_close (d);
}

static void
test_kill_while_saving (void)
{
  g_autofree char *dir = g_dir_make_tmp ("notes-crash-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "notes.db", NULL);
  g_autofree char *backups = g_build_filename (dir, "backups", NULL);
  g_autofree char *unsaved = g_build_filename (dir, "Unsaved", NULL);
  g_autofree char *rescue = g_build_filename (unsaved, "Heaps.md", NULL);
  JnDb *db = jn_db_open (path, NULL);
  gint64 subject = jn_db_add_subject (db, "DSA");
  gint64 topic = jn_db_add_topic (db, subject, "Heaps");
  gint64 session = jn_db_start_session (db, topic, T0);
  jn_db_close (db);

  int saved = 0, study = 0, max_rescued = 0;
  for (int round = 0; round < ROUNDS; round++)
    {
      int fds[2] = { -1, -1 };
      if (pipe (fds) != 0)
        die ("pipe: %s", g_strerror (errno));
      pid_t pid = fork ();
      g_assert_cmpint (pid, >=, 0);
      if (pid == 0)
        {
          close (fds[0]);
          writer (path, backups, unsaved, session, topic, saved, study, fds[1]);
        }
      close (fds[1]);
      /* Let it get going, then kill it somewhere in the middle of a write. */
      g_usleep ((gulong) g_test_rand_int_range (20, 250) * 1000);
      kill (pid, SIGKILL);
      int status;
      g_assert_cmpint (waitpid (pid, &status, 0), ==, pid);
      g_assert_true (WIFSIGNALED (status));

      int round_saved = saved, round_study = study, rescued = 0;
      read_acks (fds[0], &round_saved, &round_study, &rescued);
      close (fds[0]);

      /* The store opens and holds every reported save, nothing unstarted. */
      g_autoptr (GError) error = NULL;
      db = jn_db_open (path, &error);
      if (db == NULL)
        die ("round %d: notes.db does not open after the kill: %s", round, error->message);
      JnSession s = { 0 };
      g_assert_true (jn_db_get_session (db, session, &s));
      int v = version_of (s.body);
      if (v < round_saved || v > round_saved + 1)
        die ("round %d: the note is version %d; saved %d were reported", round, v, round_saved);
      jn_session_clear (&s);
      gint64 got = jn_db_studied (db, topic, 0);
      if (got < round_study || got > round_study + 7)
        die ("round %d: %" G_GINT64_FORMAT " s of study time; %d s were reported", round, got, round_study);
      jn_db_close (db);

      /* Backups and the rescue copy: whole, and only real versions. */
      check_backups (backups, session, v);
      g_autofree char *text = NULL;
      if (g_file_get_contents (rescue, &text, NULL, NULL))
        {
          max_rescued = MAX (max_rescued, rescued);
          int rv = version_of (text);
          if (rv < max_rescued || rv > v)
            die ("round %d: the rescue copy is not a whole version (%d; last written %d)", round, rv,
                     max_rescued);
        }
      g_test_message ("round %d: saved %d, study %d s", round, v, (int) got);
      saved = v;
      study = (int) got;
    }
  g_assert_cmpint (saved, >, ROUNDS); /* it really got work done */

  /* Clean up. */
  const char *subdirs[] = { backups, unsaved };
  for (gsize i = 0; i < G_N_ELEMENTS (subdirs); i++)
    {
      GDir *d = g_dir_open (subdirs[i], 0, NULL);
      for (const char *n = d != NULL ? g_dir_read_name (d) : NULL; n != NULL; n = g_dir_read_name (d))
        {
          g_autofree char *f = g_build_filename (subdirs[i], n, NULL);
          g_remove (f);
        }
      if (d != NULL)
        g_dir_close (d);
      g_rmdir (subdirs[i]);
    }
  g_autofree char *journal = g_strconcat (path, "-journal", NULL);
  g_remove (journal);
  g_remove (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Asia/Kolkata", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/notes-crash/kill-while-saving", test_kill_while_saving);
  return g_test_run ();
}
