/* Tests for the Notes file lock (src/notes/core/notesjail.c), LeetCode
 * style (see cases.h).
 *
 * Notes may use the network and AI, so it must never be able to read the
 * Journal's files, even if a bug or a bad web page took it over. Landlock
 * (a kernel allow-list) leaves Notes only the folders it needs; the
 * journal folder and the login keyring are never on the list.
 *
 * Each jailed check runs in a fresh copy of this test (entering cannot
 * be undone). The parent builds a fake home with a journal in it:
 *
 *   home/.local/share/journal/journal.db      SECRET     never reachable
 *   home/.local/share/keyrings/login.keyring  SECRET     never reachable
 *   home/.local/share/journal-notes/          notes      read and write
 *   home/.local/share/fonts/f.txt             readable, not writable
 *   home/.config/  home/.cache/               read and write
 *   home/Documents/Notes/                     exports    read and write
 *   home/elsewhere.txt                        not on the list */
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "cases.h"
#include "notesjail.h"

#define SECRET "dear diary, the private part"

static char *home; /* the fake home, from the parent */

static char *
at (const char *rel)
{
  return g_build_filename (home, rel, NULL);
}

static void
put (const char *rel, const char *text)
{
  g_autofree char *path = at (rel);
  g_autofree char *dir = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
}

static void
make_home (void)
{
  put (".local/share/journal/journal.db", SECRET);
  put (".local/share/keyrings/login.keyring", SECRET);
  put (".local/share/journal-notes/notes.db", "notes");
  put (".local/share/fonts/f.txt", "font");
  put (".local/share/recently-used.xbel", "recent");
  put (".config/gtk-4.0/settings.ini", "[Settings]");
  put (".cache/x", "cache");
  put ("Documents/Notes/old.md", "# old");
  put ("elsewhere.txt", "elsewhere");
}

static void
remove_tree (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  if (dir == NULL)
    {
      g_remove (path);
      return;
    }
  const char *name;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *child = g_build_filename (path, name, NULL);
      remove_tree (child);
    }
  g_dir_close (dir);
  g_rmdir (path);
}

/* The environment Notes would see with this home. */
static char **
fake_env (void)
{
  char **env = g_get_environ ();
  env = g_environ_setenv (env, "HOME", home, TRUE);
  env = g_environ_unsetenv (env, "XDG_DATA_HOME");
  env = g_environ_unsetenv (env, "XDG_CONFIG_HOME");
  env = g_environ_unsetenv (env, "XDG_CACHE_HOME");
  env = g_environ_unsetenv (env, "XDG_STATE_HOME");
  env = g_environ_setenv (env, "JR_FAKE_HOME", home, TRUE);
  return env;
}

static void
in_jail (const char *path)
{
  g_auto (GStrv) env = fake_env ();
  g_test_trap_subprocess_with_envp (path, (const char *const *) env, 0, G_TEST_SUBPROCESS_DEFAULT);
}

static void
enter_or_die (void)
{
  home = g_strdup (g_getenv ("JR_FAKE_HOME"));
  g_autoptr (GError) error = NULL;
  JnJailPaths *p = jn_jail_default_paths ();
  if (!jn_jail_enter (p, &error))
    {
      g_printerr ("could not enter the file lock: %s\n", error->message);
      g_assert_not_reached ();
    }
  jn_jail_paths_free (p);
}

/* ---- which folders are on the list ------------------------------------ */

static gboolean
listed (char **list, const char *path)
{
  for (char **p = list; p != NULL && *p != NULL; p++)
    if (strcmp (*p, path) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
under_any (char **list, const char *path)
{
  for (char **p = list; p != NULL && *p != NULL; p++)
    if (strcmp (*p, path) == 0 || (g_str_has_prefix (path, *p) && path[strlen (*p)] == '/'))
      return TRUE;
  return FALSE;
}

static void
test_default_paths (void)
{
  if (g_test_subprocess ())
    {
      home = g_strdup (g_getenv ("JR_FAKE_HOME"));
      JnJailPaths *p = jn_jail_default_paths ();
      JrCases c = { "default paths", 0, 0 };
      g_autofree char *journal = at (".local/share/journal");
      g_autofree char *keyrings = at (".local/share/keyrings");
      g_autofree char *notes = at (".local/share/journal-notes");
      g_autofree char *fonts = at (".local/share/fonts");
      g_autofree char *config = at (".config");
      g_autofree char *docs = at ("Documents/Notes");
      jr_case (&c, "journal never readable", !under_any (p->read, journal) && !under_any (p->write, journal),
               "Expected %s on no list", journal);
      jr_case (&c, "keyring never readable", !under_any (p->read, keyrings) && !under_any (p->write, keyrings),
               "Expected %s on no list", keyrings);
      jr_case (&c, "home itself not listed", !listed (p->read, home) && !listed (p->write, home),
               "Expected %s on no list", home);
      jr_case (&c, "notes writable", listed (p->write, notes), "Expected %s writable", notes);
      jr_case (&c, "fonts readable only", listed (p->read, fonts) && !under_any (p->write, fonts),
               "Expected %s readable, not writable", fonts);
      jr_case (&c, "settings writable", listed (p->write, config), "Expected %s writable", config);
      jr_case (&c, "exports writable", listed (p->write, docs), "Expected %s writable", docs);
      jr_case (&c, "system readable", listed (p->read, "/usr") && listed (p->read, "/etc"),
               "Expected /usr and /etc readable");
      jr_case (&c, "no write to the system", !under_any (p->write, "/usr") && !under_any (p->write, "/etc"),
               "Expected /usr and /etc not writable");
      jr_case (&c, "no shared temp folder", !listed (p->write, "/tmp") && !listed (p->write, "/var/tmp"),
               "Expected /tmp and /var/tmp not writable");
      /* No listed folder holds the journal (no "/" or ~/.local/share). */
      for (int w = 0; w < 2; w++)
        for (char **l = w ? p->write : p->read; *l != NULL; l++)
          jr_case (&c, *l, !(g_str_has_prefix (journal, *l) && journal[strlen (*l)] == '/'),
                   "Listed %s holds the journal", *l);
      jr_cases_done (&c);
      jn_jail_paths_free (p);
      return;
    }
  in_jail (NULL);
  g_test_trap_assert_passed ();
}

/* ---- what a jailed Notes can do ------------------------------------- */

typedef enum { READ, LIST, WRITE, CREATE } Access;

typedef struct {
  const char *name;
  Access      access;
  const char *rel;     /* inside the fake home, or absolute */
  gboolean    allowed;
} AccessCase;

static const AccessCase access_cases[] = {
  { "read the journal",            READ,   ".local/share/journal/journal.db", FALSE },
  { "list the journal folder",     LIST,   ".local/share/journal",            FALSE },
  { "write into the journal",      CREATE, ".local/share/journal/planted",    FALSE },
  { "read the keyring",            READ,   ".local/share/keyrings/login.keyring", FALSE },
  { "read the journal via ..",     READ,   ".local/share/journal-notes/../journal/journal.db", FALSE },
  { "list home",                   LIST,   "",                                FALSE },
  { "read a file in home",         READ,   "elsewhere.txt",                   FALSE },
  { "read the notes",              READ,   ".local/share/journal-notes/notes.db", TRUE },
  { "write the notes",             WRITE,  ".local/share/journal-notes/notes.db", TRUE },
  { "new file next to the notes",  CREATE, ".local/share/journal-notes/notes.db-journal", TRUE },
  { "read fonts",                  READ,   ".local/share/fonts/f.txt",        TRUE },
  { "write fonts",                 WRITE,  ".local/share/fonts/f.txt",        FALSE },
  { "read recent files",           READ,   ".local/share/recently-used.xbel", TRUE },
  { "read settings",               READ,   ".config/gtk-4.0/settings.ini",    TRUE },
  { "write the cache",             CREATE, ".cache/y",                        TRUE },
  { "export a note",               CREATE, "Documents/Notes/prefix-sum.md",   TRUE },
  { "read a system file",          READ,   "/etc/hostname",                   TRUE },
  { "write a system folder",       CREATE, "/etc/planted-by-notes",           FALSE },
  { "write the shared /tmp",       CREATE, "/tmp/planted-by-notes",           FALSE },
};

static gboolean
try_access (Access a, const char *path)
{
  switch (a)
    {
    case READ:
      {
        int fd = open (path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
          return FALSE;
        char buf[8];
        gboolean ok = read (fd, buf, sizeof buf) >= 0;
        close (fd);
        return ok;
      }
    case LIST:
      {
        GDir *d = g_dir_open (path, 0, NULL);
        if (d != NULL)
          g_dir_close (d);
        return d != NULL;
      }
    case WRITE:
      {
        int fd = open (path, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0)
          return FALSE;
        gboolean ok = write (fd, "", 0) >= 0;
        close (fd);
        return ok;
      }
    case CREATE:
    default:
      {
        int fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
          return FALSE;
        close (fd);
        unlink (path);
        return TRUE;
      }
    }
}

static void
test_access_table (void)
{
  if (g_test_subprocess ())
    {
      enter_or_die ();
      JrCases c = { "access", 0, 0 };
      for (gsize i = 0; i < G_N_ELEMENTS (access_cases); i++)
        {
          const AccessCase *k = &access_cases[i];
          if (k->rel[0] == '/' && !g_file_test (k->rel, G_FILE_TEST_EXISTS) && k->access == READ)
            continue; /* e.g. no /etc/hostname in a container */
          g_autofree char *path = k->rel[0] == '/' ? g_strdup (k->rel) : at (k->rel);
          gboolean got = try_access (k->access, path);
          jr_case (&c, k->name, got == k->allowed, "Input %s  Expected %s  Got %s", k->rel,
                   k->allowed ? "allowed" : "refused", got ? "allowed" : "refused");
        }
      /* /proc: its own entry yes, another program's (the parent test) no. */
      g_autofree char *other = g_strdup_printf ("/proc/%d/environ", (int) getppid ());
      jr_case (&c, "its own /proc entry", try_access (READ, "/proc/self/status"), "Expected readable");
      jr_case (&c, "another program's environment", !try_access (READ, other), "Expected refused: %s", other);
      jr_case (&c, "the list of programs", !try_access (LIST, "/proc"), "Expected refused");
      jr_cases_done (&c);
      return;
    }
  in_jail (NULL);
  g_test_trap_assert_passed ();
}

/* Tricks that point at the journal from an allowed folder. */
static void
test_tricks (void)
{
  if (g_test_subprocess ())
    {
      g_autofree char *journal = g_build_filename (g_getenv ("JR_FAKE_HOME"), ".local/share/journal/journal.db", NULL);
      g_autofree char *sym = g_build_filename (g_getenv ("JR_FAKE_HOME"), ".local/share/journal-notes/link.db", NULL);
      g_assert_cmpint (symlink (journal, sym), ==, 0); /* planted before the lock, as an attacker would */
      enter_or_die ();
      JrCases c = { "tricks", 0, 0 };
      /* A symlink in an allowed folder still reaches a refused file. */
      jr_case (&c, "symlink to the journal", !try_access (READ, sym), "Expected refused  Got the journal");
      /* A hard link or a move would need the journal folder. */
      g_autofree char *hard = at (".local/share/journal-notes/hard.db");
      jr_case (&c, "hard link of the journal", link (journal, hard) != 0, "Expected refused  Got a link");
      g_autofree char *moved = at (".local/share/journal-notes/moved.db");
      jr_case (&c, "move the journal out", rename (journal, moved) != 0, "Expected refused  Got moved");
      /* Not even its size or a copy through /proc. */
      g_autofree char *proc = g_strdup_printf ("/proc/self/root%s", journal);
      jr_case (&c, "through /proc/self/root", !try_access (READ, proc), "Expected refused");
      jr_cases_done (&c);
      return;
    }
  in_jail (NULL);
  g_test_trap_assert_passed ();
  /* The journal is still there, untouched. */
  g_autofree char *journal = at (".local/share/journal/journal.db");
  g_autofree char *text = NULL;
  g_assert_true (g_file_get_contents (journal, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, SECRET);
  g_autofree char *sym = at (".local/share/journal-notes/link.db");
  g_remove (sym);
}

/* Programs Notes starts are locked the same way. */
static void
test_children (void)
{
  if (g_test_subprocess ())
    {
      enter_or_die ();
      g_autofree char *journal = at (".local/share/journal/journal.db");
      char *argv[] = { (char *) "/bin/cat", journal, NULL };
      g_autofree char *out = NULL;
      int status = 0;
      g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status,
                                   NULL));
      g_assert_null (strstr (out, SECRET));
      g_assert_false (g_spawn_check_wait_status (status, NULL));
      return;
    }
  in_jail (NULL);
  g_test_trap_assert_passed ();
}


/* ---- the AI helper's lock: nothing on disk, only the allowed ports ---- */

static int
listener (int *port)
{
  int fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      g_printerr ("cannot create a test listener: %s\n", g_strerror (errno));
      g_assert_not_reached ();
    }
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl (INADDR_LOOPBACK) };
  g_assert_cmpint (bind (fd, (struct sockaddr *) &a, sizeof a), ==, 0);
  g_assert_cmpint (listen (fd, 4), ==, 0);
  socklen_t len = sizeof a;
  g_assert_cmpint (getsockname (fd, (struct sockaddr *) &a, &len), ==, 0);
  *port = ntohs (a.sin_port);
  return fd;
}

static int
connect_to (int port)
{
  int fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -errno;
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons ((guint16) port),
                           .sin_addr.s_addr = htonl (INADDR_LOOPBACK) };
  int rc = connect (fd, (struct sockaddr *) &a, sizeof a) == 0 ? 0 : -errno;
  close (fd);
  return rc;
}

static void
test_helper_lock (void)
{
  if (g_test_subprocess ())
    {
      home = g_strdup (g_getenv ("JR_FAKE_HOME"));
      int allowed = atoi (g_getenv ("JR_PORT_OK")), other = atoi (g_getenv ("JR_PORT_NO"));
      guint16 ports[] = { (guint16) allowed };
      JnJailPaths *p = jn_jail_helper_paths (ports, 1);
      g_autoptr (GError) error = NULL;
      if (!jn_jail_enter (p, &error))
        {
          g_printerr ("could not enter the helper lock: %s\n", error->message);
          g_assert_not_reached ();
        }
      jn_jail_paths_free (p);
      JrCases c = { "helper lock", 0, 0 };
      jr_case (&c, "the allowed port", connect_to (allowed) == 0, "Expected connected  Got %s",
               g_strerror (-connect_to (allowed)));
      jr_case (&c, "any other port", connect_to (other) == -EACCES, "Expected EACCES  Got %s",
               g_strerror (-connect_to (other)));
      int s = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl (INADDR_LOOPBACK) };
      jr_case (&c, "no listening", bind (s, (struct sockaddr *) &a, sizeof a) != 0, "Expected bind refused");
      close (s);
      g_autofree char *journal = at (".local/share/journal/journal.db");
      g_autofree char *notes = at (".local/share/journal-notes/notes.db");
      g_autofree char *config = at (".config/gtk-4.0/settings.ini");
      jr_case (&c, "the journal", !try_access (READ, journal), "Expected refused");
      jr_case (&c, "the notes", !try_access (READ, notes), "Expected refused (the helper needs no notes on disk)");
      jr_case (&c, "settings", !try_access (READ, config), "Expected refused");
      g_autofree char *tmp = g_build_filename (g_get_tmp_dir (), "planted-by-helper", NULL);
      jr_case (&c, "write anywhere", !try_access (CREATE, tmp), "Expected refused");
      jr_case (&c, "network settings", try_access (READ, "/etc/resolv.conf") || !g_file_test ("/etc/resolv.conf",
               G_FILE_TEST_EXISTS), "Expected readable");
      jr_case (&c, "website certificates", !g_file_test ("/etc/ssl/certs/ca-certificates.crt", G_FILE_TEST_EXISTS) ||
               try_access (READ, "/etc/ssl/certs/ca-certificates.crt"), "Expected readable");
      jr_case (&c, "system libraries", try_access (LIST, "/usr/lib"), "Expected readable");
      jr_cases_done (&c);
      return;
    }
  int ok_port, no_port;
  int a = listener (&ok_port), b = listener (&no_port);
  g_autofree char *ok_s = g_strdup_printf ("%d", ok_port), *no_s = g_strdup_printf ("%d", no_port);
  g_auto (GStrv) env = fake_env ();
  env = g_environ_setenv (env, "JR_PORT_OK", ok_s, TRUE);
  env = g_environ_setenv (env, "JR_PORT_NO", no_s, TRUE);
  g_test_trap_subprocess_with_envp (NULL, (const char *const *) env, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed ();
  close (a);
  close (b);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!g_test_subprocess ())
    {
      /* Not under TMPDIR: that is the private runtime folder or /tmp,
       * both on Notes' list, and the journal must sit where no listed
       * folder holds it, as ~/.local/share/journal does. The test runs in
       * the build folder; the tree is removed afterwards. */
      g_autofree char *cwd = g_get_current_dir ();
      g_autofree char *base = g_build_filename (cwd, "notesjail-XXXXXX", NULL);
      g_assert_nonnull (g_mkdtemp (base));
      home = g_build_filename (base, "home", NULL);
      make_home ();
      g_test_add_func ("/notesjail/default-paths", test_default_paths);
      g_test_add_func ("/notesjail/access", test_access_table);
      g_test_add_func ("/notesjail/tricks", test_tricks);
      g_test_add_func ("/notesjail/children", test_children);
      g_test_add_func ("/notesjail/helper-lock", test_helper_lock);
      int rc = g_test_run ();
      remove_tree (base);
      g_free (home);
      return rc;
    }
  g_test_add_func ("/notesjail/default-paths", test_default_paths);
  g_test_add_func ("/notesjail/access", test_access_table);
  g_test_add_func ("/notesjail/tricks", test_tricks);
  g_test_add_func ("/notesjail/children", test_children);
  g_test_add_func ("/notesjail/helper-lock", test_helper_lock);
  return g_test_run ();
}
