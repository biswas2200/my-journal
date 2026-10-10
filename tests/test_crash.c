/* Crash safety: a writer process is killed with SIGKILL at random moments
 * while it saves. Afterwards the file must pass SQLite's integrity check,
 * every save the writer reported as finished must be there and decrypt,
 * and a lock change cut off half-way must leave the journal openable with
 * the old or the new PIN (never neither). */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "journal.h"

#define ROUNDS 25
#define DAY "2026-10-06"
#define T0 1791316800

static const JrKdfCost FAST = { JR_KDF_OPS_MIN, JR_KDF_MEM_MIN };

/* What the writer reports after each finished save. */
typedef struct {
  gint64 id;      /* entry id, or 0 for a lock change */
  gint64 version; /* text version written, or index of the PIN now set */
} Ack;

static const char *const PINS[2] = { "111111", "222222" };

static JrJournal *
open_journal (const char *path)
{
  GError *error = NULL;
  JrJournal *j = jr_journal_open (path, &error);
  g_assert_no_error (error);
  jr_journal_set_kdf_cost (j, FAST);
  return j;
}

/* Child: add and rewrite entries forever, acknowledging each save. */
static void G_GNUC_NORETURN
write_entries (const char *path, int fd)
{
  JrJournal *j = open_journal (path);
  GRand *rand = g_rand_new ();
  for (gint64 n = 1;; n++)
    {
      /* Random sizes so pages split and overflow. */
      GString *text = g_string_new (NULL);
      int words = g_rand_int_range (rand, 1, 400);
      for (int w = 0; w < words; w++)
        g_string_append (text, "loop ");
      g_string_append_printf (text, "v%" G_GINT64_FORMAT, n);
      Ack ack = { jr_journal_add_entry (j, DAY, T0 + n, text->str, T0 + n), n };
      g_assert_cmpint (ack.id, >, 0);
      if (write (fd, &ack, sizeof ack) != sizeof ack)
        _exit (2);
      if (n % 3 == 0) /* rewrite an earlier entry too */
        {
          g_string_append (text, " edited");
          if (jr_journal_update_entry (j, ack.id, text->str, T0 + n))
            if (write (fd, &ack, sizeof ack) != sizeof ack)
              _exit (2);
        }
      jr_journal_add_active_seconds (j, DAY, 1);
      g_string_free (text, TRUE);
    }
}

/* Child: change the PIN back and forth forever, acknowledging each one. */
static void G_GNUC_NORETURN
change_pins (const char *path, int fd)
{
  JrJournal *j = open_journal (path);
  g_assert_cmpint (jr_journal_unlock (j, PINS[0], T0), ==, JR_UNLOCK_OK);
  for (int cur = 0;; cur = 1 - cur)
    {
      g_assert_cmpint (jr_journal_change_lock (j, PINS[cur], JR_LOCK_PIN, PINS[1 - cur], T0), ==,
                       JR_JOB_OK);
      Ack ack = { 0, 1 - cur };
      if (write (fd, &ack, sizeof ack) != sizeof ack)
        _exit (2);
    }
}

/* Runs `child` in a forked process and kills it after a random moment.
 * Returns everything it acknowledged before dying. */
static GArray *
run_and_kill (void (*child) (const char *, int), const char *path)
{
  int fds[2] = { -1, -1 };
  if (pipe (fds) != 0)
    {
      g_printerr ("pipe: %s\n", g_strerror (errno));
      abort ();
    }
  pid_t pid = fork ();
  g_assert_cmpint (pid, >=, 0);
  if (pid == 0)
    {
      close (fds[0]);
      child (path, fds[1]);
      _exit (3); /* never reached: the child runs until it is killed */
    }
  close (fds[1]);
  g_usleep ((gulong) g_random_int_range (5, 80) * 1000);
  kill (pid, SIGKILL);
  int status = 0;
  g_assert_cmpint (waitpid (pid, &status, 0), ==, pid);
  g_assert_true (WIFSIGNALED (status) && WTERMSIG (status) == SIGKILL);

  GArray *acks = g_array_new (FALSE, FALSE, sizeof (Ack));
  Ack ack;
  ssize_t n;
  while ((n = read (fds[0], &ack, sizeof ack)) == sizeof ack)
    g_array_append_val (acks, ack);
  g_assert_cmpint (n, ==, 0); /* clean end: no torn acknowledgement */
  close (fds[0]);
  return acks;
}

static void
assert_file_intact (const char *path)
{
  sqlite3 *db;
  sqlite3_stmt *st;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_prepare_v2 (db, "PRAGMA integrity_check", -1, &st, NULL), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_step (st), ==, SQLITE_ROW);
  g_assert_cmpstr ((const char *) sqlite3_column_text (st, 0), ==, "ok");
  sqlite3_finalize (st);
  sqlite3_close (db);
}

typedef struct {
  GHashTable *versions; /* id -> highest version found in the text */
} Found;

static void
collect (gint64 id, gint64 stamped_at, const char *text, gpointer user)
{
  (void) stamped_at;
  Found *f = user;
  const char *v = strrchr (text, 'v');
  g_assert_nonnull (v); /* decrypted to the text that was written */
  g_hash_table_insert (f->versions, g_memdup2 (&id, sizeof id),
                       GINT_TO_POINTER ((int) g_ascii_strtoll (v + 1, NULL, 10)));
}

static void
test_entries_survive_kill (void)
{
  g_autofree char *dir = g_dir_make_tmp ("journal-crash-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  gsize total = 0;

  for (int round = 0; round < ROUNDS; round++)
    {
      GArray *acks = run_and_kill (write_entries, path);
      total += acks->len;
      assert_file_intact (path);

      JrJournal *j = open_journal (path);
      Found f = { g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL) };
      g_assert_true (jr_journal_foreach_entry (j, DAY, collect, &f));
      for (guint i = 0; i < acks->len; i++)
        {
          Ack *a = &g_array_index (acks, Ack, i);
          gpointer v;
          g_assert_true (g_hash_table_lookup_extended (f.versions, &a->id, NULL, &v));
          g_assert_cmpint (GPOINTER_TO_INT (v), >=, a->version);
        }
      g_hash_table_unref (f.versions);
      jr_journal_close (j);
      g_array_unref (acks);
    }
  g_test_message ("%" G_GSIZE_FORMAT " acknowledged saves, all survived %d kills", total, ROUNDS);
  g_assert_cmpuint (total, >, 0);

  g_remove (path);
  g_rmdir (dir);
}

static void
test_lock_change_survives_kill (void)
{
  g_autofree char *dir = g_dir_make_tmp ("journal-crash-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "journal.db", NULL);
  JrJournal *j = open_journal (path);
  jr_journal_add_entry (j, DAY, T0, "still here v1", T0);
  char *recovery = jr_journal_enable_lock (j, JR_LOCK_PIN, PINS[0]);
  g_assert_nonnull (recovery);
  jr_journal_close (j);

  int current = 0;
  gsize changes = 0;
  for (int round = 0; round < ROUNDS; round++)
    {
      /* The child always starts from PINS[0]; put it back if needed. */
      if (current != 0)
        {
          j = open_journal (path); /* opens locked: unlock before changing */
          g_assert_cmpint (jr_journal_unlock (j, PINS[current], T0), ==, JR_UNLOCK_OK);
          g_assert_cmpint (jr_journal_change_lock (j, PINS[current], JR_LOCK_PIN, PINS[0], T0), ==,
                           JR_JOB_OK);
          jr_journal_close (j);
          current = 0;
        }
      GArray *acks = run_and_kill (change_pins, path);
      int acked = acks->len > 0 ? (int) g_array_index (acks, Ack, acks->len - 1).version : 0;
      changes += acks->len;
      g_array_unref (acks);
      assert_file_intact (path);

      /* The acknowledged PIN, or the one being set when it died, opens it. */
      j = open_journal (path);
      if (jr_journal_unlock (j, PINS[acked], T0) == JR_UNLOCK_OK)
        current = acked;
      else
        {
          g_assert_cmpint (jr_journal_unlock (j, PINS[1 - acked], T0), ==, JR_UNLOCK_OK);
          current = 1 - acked;
        }
      Found f = { g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL) };
      jr_journal_foreach_entry (j, DAY, collect, &f);
      g_assert_cmpuint (g_hash_table_size (f.versions), ==, 1);
      g_hash_table_unref (f.versions);
      jr_journal_lock (j);
      /* The recovery key always works too. */
      g_assert_cmpint (jr_journal_unlock_recovery (j, recovery, T0), ==, JR_UNLOCK_OK);
      jr_journal_close (j);
    }
  g_test_message ("%" G_GSIZE_FORMAT " PIN changes; the journal opened after all %d kills",
                  changes, ROUNDS);
  jr_secret_free (recovery);
  g_remove (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_true (jr_crypto_init ());
  g_test_add_func ("/crash/entries-survive-kill", test_entries_survive_kill);
  g_test_add_func ("/crash/lock-change-survives-kill", test_lock_change_survives_kill);
  return g_test_run ();
}
