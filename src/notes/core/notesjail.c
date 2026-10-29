/* notesjail: see notesjail.h. */
#include "notesjail.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/landlock.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include "noteexport.h"

/* Never on any list, whatever ~/.local/share holds. */
static const char *const NEVER[] = { "journal", "keyrings", NULL };
#define NOTES_DIR "journal-notes"

/* /proc: only Notes' own entry and a few facts about the machine, never
 * other programs' entries (their command lines and environments, where
 * secrets can live). */
static const char *const SYSTEM_READ[] = {
  "/usr", "/etc", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/opt", "/snap",
  "/var/lib", "/var/cache", "/sys", "/run",
  "/proc/self", "/proc/meminfo", "/proc/cpuinfo", "/proc/stat", "/proc/filesystems", NULL,
};
static const char *const SYSTEM_WRITE[] = { "/dev", NULL };

static int
landlock_abi (void)
{
  long v = syscall (SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
  return v < 0 ? 0 : (int) v;
}

gboolean
jn_jail_supported (void)
{
  return landlock_abi () >= 1;
}

static void
add (GPtrArray *a, const char *path)
{
  if (path != NULL && *path == '/')
    g_ptr_array_add (a, g_strdup (path));
}

static void
add_home (GPtrArray *a, const char *rel)
{
  g_autofree char *p = g_build_filename (g_get_home_dir (), rel, NULL);
  add (a, p);
}

/* Coverage builds only: the object folders that receive .gcda files. */
static void
add_gcov_dirs (GPtrArray *w)
{
#ifdef JN_GCOV_ROOT
  static const char *const roots[] = { JN_GCOV_ROOT, JN_GCOV_ROOT "/tests", NULL };
  for (const char *const *root = roots; *root != NULL; root++)
    {
      GDir *objs = g_dir_open (*root, 0, NULL);
      if (objs == NULL)
        continue;
      for (const char *obj = g_dir_read_name (objs); obj != NULL; obj = g_dir_read_name (objs))
        if (g_str_has_suffix (obj, ".p"))
          {
            g_autofree char *p = g_build_filename (*root, obj, NULL);
            add (w, p);
          }
      g_dir_close (objs);
    }
#else
  (void) w;
#endif
}

JnJailPaths *
jn_jail_default_paths (void)
{
  GPtrArray *r = g_ptr_array_new (), *w = g_ptr_array_new ();
  for (const char *const *p = SYSTEM_READ; *p != NULL; p++)
    add (r, *p);
  for (const char *const *p = SYSTEM_WRITE; *p != NULL; p++)
    add (w, *p);
  for (const char *const *d = g_get_system_data_dirs (); *d != NULL; d++)
    add (r, *d);

  /* ~/.local/share item by item, so the journal and keyring stay out. */
  const char *data = g_get_user_data_dir ();
  GDir *dir = g_dir_open (data, 0, NULL);
  if (dir != NULL)
    {
      for (const char *name = g_dir_read_name (dir); name != NULL; name = g_dir_read_name (dir))
        if (!g_strv_contains (NEVER, name) && strcmp (name, NOTES_DIR) != 0)
          {
            g_autofree char *p = g_build_filename (data, name, NULL);
            add (r, p);
          }
      g_dir_close (dir);
    }
  /* A development copy of the AI helper ($JOURNAL_AI_HELPER): that one
   * file may be read and run, nothing else beside it. */
  const char *helper = g_getenv ("JOURNAL_AI_HELPER");
  if (helper != NULL && g_path_is_absolute (helper))
    add (r, helper);
  /* A Google sign-in file kept elsewhere ($GOOGLE_APPLICATION_CREDENTIALS):
   * that one file, read-only. */
  const char *adc = g_getenv ("GOOGLE_APPLICATION_CREDENTIALS");
  if (adc != NULL && g_path_is_absolute (adc))
    add (r, adc);
  add_home (r, ".icons");
  add_home (r, ".fonts");
  add_home (r, ".themes");

  g_autofree char *notes = g_build_filename (data, NOTES_DIR, NULL);
  g_mkdir_with_parents (notes, 0700);
  add (w, notes);
  add (w, g_get_user_config_dir ());
  add (w, g_get_user_cache_dir ());
  add (w, g_get_user_state_dir ());
  add (w, g_get_user_runtime_dir ());
  /* Never a temp folder others can write to (/tmp): Notes needs none, and
   * a shared folder is where another program could leave a trap. A
   * private $TMPDIR (tests, some setups) is fine. */
  struct stat st;
  const char *tmp = g_get_tmp_dir ();
  if (g_stat (tmp, &st) == 0 && S_ISDIR (st.st_mode) && st.st_uid == getuid () && (st.st_mode & 022) == 0)
    add (w, tmp);
  g_autofree char *exports = jn_export_dir ();
  g_mkdir_with_parents (exports, 0700);
  add (w, exports);

  add_gcov_dirs (w);
  g_ptr_array_add (r, NULL);
  g_ptr_array_add (w, NULL);
  JnJailPaths *paths = g_new0 (JnJailPaths, 1);
  paths->read = (char **) g_ptr_array_free (r, FALSE);
  paths->write = (char **) g_ptr_array_free (w, FALSE);
  return paths;
}

static const char *const HELPER_READ[] = {
  "/usr", "/lib", "/lib32", "/lib64",
  "/etc/ssl", "/etc/ca-certificates", "/etc/pki", "/etc/gnutls", "/etc/crypto-policies",
  "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf", "/etc/gai.conf", "/etc/host.conf",
  "/etc/localtime", "/run/systemd/resolve", "/dev/null", "/dev/urandom", NULL,
};

JnJailPaths *
jn_jail_helper_paths (const guint16 *ports, gsize n_ports)
{
  JnJailPaths *paths = g_new0 (JnJailPaths, 1);
  paths->read = g_strdupv ((char **) HELPER_READ);
#if defined(__SANITIZE_ADDRESS__)
  /* Sanitizer builds only: LeakSanitizer reads /proc at exit. A real
   * helper never may (other programs' environments live there). */
  {
    gsize n = g_strv_length (paths->read);
    paths->read = g_renew (char *, paths->read, n + 2);
    paths->read[n] = g_strdup ("/proc");
    paths->read[n + 1] = NULL;
  }
#endif
  GPtrArray *w = g_ptr_array_new ();
  add_gcov_dirs (w); /* nothing outside coverage builds */
  g_ptr_array_add (w, NULL);
  paths->write = (char **) g_ptr_array_free (w, FALSE);
  paths->limit_net = TRUE;
  paths->n_ports = MIN (n_ports, (gsize) JN_JAIL_MAX_PORTS);
  for (gsize i = 0; i < paths->n_ports; i++)
    paths->ports[i] = ports[i];
  return paths;
}

void
jn_jail_paths_free (JnJailPaths *paths)
{
  if (paths == NULL)
    return;
  g_strfreev (paths->read);
  g_strfreev (paths->write);
  g_free (paths);
}

/* Rights a rule on a file (not a folder) may carry. */
#define FILE_RIGHTS (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | \
                     TRUNCATE | IOCTL_DEV)
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
#define TRUNCATE LANDLOCK_ACCESS_FS_TRUNCATE
#else
#define TRUNCATE 0
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define IOCTL_DEV LANDLOCK_ACCESS_FS_IOCTL_DEV
#else
#define IOCTL_DEV 0
#endif

static guint64
handled_rights (int abi)
{
  guint64 h = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
              LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
              LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
              LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
              LANDLOCK_ACCESS_FS_MAKE_SYM;
  if (abi >= 2)
    h |= LANDLOCK_ACCESS_FS_REFER; /* links and renames between folders */
  if (abi >= 3)
    h |= TRUNCATE;
  if (abi >= 5)
    h |= IOCTL_DEV;
  return h;
}

static gboolean
allow (int ruleset, const char *path, guint64 rights, GError **error)
{
  int fd = open (path, O_PATH | O_CLOEXEC);
  if (fd < 0)
    return TRUE; /* not on this system: nothing to allow */
  struct stat st;
  if (fstat (fd, &st) == 0 && !S_ISDIR (st.st_mode))
    rights &= FILE_RIGHTS;
  struct landlock_path_beneath_attr rule = { .allowed_access = rights, .parent_fd = fd };
  long rc = syscall (SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
  int e = errno;
  close (fd);
  if (rc != 0)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Landlock refused %s: %s", path, g_strerror (e));
  return rc == 0;
}

gboolean
jn_jail_enter (const JnJailPaths *paths, GError **error)
{
  int abi = landlock_abi ();
  if (abi < 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This kernel has no Landlock, which keeps Notes away from the journal");
      return FALSE;
    }
  guint64 handled = handled_rights (abi);
  guint64 read = handled & (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR);
  if (paths->limit_net && abi < 4)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This kernel cannot limit network ports (Landlock 4, Linux 6.7, is needed)");
      return FALSE;
    }
  struct landlock_ruleset_attr attr = { .handled_access_fs = handled };
  if (paths->limit_net)
    attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#ifdef LANDLOCK_SCOPE_SIGNAL
  /* Also no signals to, and no abstract sockets of, programs outside
   * the lock (the Journal among them). */
  if (abi >= 6)
    attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
#endif
  size_t size = abi >= 4 ? offsetof (struct landlock_ruleset_attr, handled_access_net) + sizeof attr.handled_access_net
                         : offsetof (struct landlock_ruleset_attr, handled_access_fs) + sizeof attr.handled_access_fs;
#ifdef LANDLOCK_SCOPE_SIGNAL
  if (abi >= 6)
    size = sizeof attr;
#endif
  int ruleset = (int) syscall (SYS_landlock_create_ruleset, &attr, size, 0);
  if (ruleset < 0)
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Landlock: %s", g_strerror (e));
      return FALSE;
    }
  gboolean ok = TRUE;
  for (char **p = paths->read; ok && *p != NULL; p++)
    ok = allow (ruleset, *p, read, error);
  for (char **p = paths->write; ok && *p != NULL; p++)
    ok = allow (ruleset, *p, handled, error);
  for (gsize i = 0; ok && paths->limit_net && i < paths->n_ports; i++)
    {
      struct landlock_net_port_attr port = { .allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP,
                                             .port = paths->ports[i] };
      if (syscall (SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_NET_PORT, &port, 0) != 0)
        {
          int e = errno;
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Landlock refused port %u: %s",
                       paths->ports[i], g_strerror (e));
          ok = FALSE;
        }
    }
  if (ok && (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || syscall (SYS_landlock_restrict_self, ruleset, 0) != 0))
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Landlock: %s", g_strerror (e));
      ok = FALSE;
    }
  close (ruleset);
  return ok;
}
