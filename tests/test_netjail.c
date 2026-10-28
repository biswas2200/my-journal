/* Tests for the Journal's network jail (src/core/netjail.c), LeetCode
 * style (see cases.h).
 *
 * Entering the jail cannot be undone, so every jailed check runs in a
 * fresh copy of this test (g_test_trap_subprocess); the parent stays free
 * to set up a real TCP listener and look at what reached it.
 *
 * Rule under test: once jailed, a process (all its threads, and every
 * program it starts) can create local AF_UNIX sockets only. Any other
 * socket fails with EACCES before anything is sent, and each attempt is
 * counted and logged on stderr. */
#include <errno.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glib.h>
#include "cases.h"
#include "netjail.h"

#define LOG_LINE "*blocked a network call*"
#define GOT_FD "GOT-FD"

/* Child mode: try one internet socket and say so if it worked. */
static int
try_socket_child (void)
{
  int fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd >= 0)
    {
      g_print (GOT_FD "\n");
      close (fd);
      return 0;
    }
  return 3;
}

static void
enter_or_die (void)
{
  if (!jr_netjail_enter ())
    {
      g_printerr ("could not enter the network jail\n");
      g_assert_not_reached ();
    }
}

/* TRUE if the socket was refused with EACCES. Anything it did get is
 * closed, so a failing check never leaks a descriptor. */
static gboolean
refused (int domain, int type)
{
  errno = 0;
  int fd = socket (domain, type, 0);
  int err = errno;
  if (fd >= 0)
    {
      close (fd);
      return FALSE;
    }
  return err == EACCES;
}

static void
jailed (const char *path)
{
  g_test_trap_subprocess (path, 0, G_TEST_SUBPROCESS_DEFAULT);
}

/* ---- the socket table --------------------------------------------- */

/* Constraints: domain is any value a caller can pass (including garbage
 * and 64-bit values with high bits set); type may carry NONBLOCK/CLOEXEC.
 * Expected: allowed only for exactly AF_UNIX. */
typedef struct {
  const char *name;
  long        domain;
  int         type;
  gboolean    allowed;
} SocketCase;

static const SocketCase socket_cases[] = {
  { "TCP over IPv4",                  AF_INET,    SOCK_STREAM, FALSE },
  { "UDP over IPv4 (DNS)",            AF_INET,    SOCK_DGRAM,  FALSE },
  { "raw IPv4 (ping)",                AF_INET,    SOCK_RAW,    FALSE },
  { "TCP, non-blocking + cloexec",    AF_INET,    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, FALSE },
  { "TCP over IPv6",                  AF_INET6,   SOCK_STREAM, FALSE },
  { "UDP over IPv6",                  AF_INET6,   SOCK_DGRAM,  FALSE },
  { "packet socket (sniffing)",       AF_PACKET,  SOCK_RAW,    FALSE },
  { "netlink",                        AF_NETLINK, SOCK_RAW,    FALSE },
  { "bluetooth",                      AF_BLUETOOTH, SOCK_STREAM, FALSE },
  { "vsock (to a VM host)",           AF_VSOCK,   SOCK_STREAM, FALSE },
  { "AF_UNSPEC",                      AF_UNSPEC,  SOCK_STREAM, FALSE },
  { "beyond AF_MAX",                  AF_MAX + 5, SOCK_STREAM, FALSE },
  { "negative domain",                -1,         SOCK_STREAM, FALSE },
  { "AF_INET with high bits set",     (1L << 32) | AF_INET, SOCK_STREAM, FALSE },
  { "AF_UNIX with high bits set",     (1L << 32) | AF_UNIX, SOCK_STREAM, FALSE },
  { "local stream (Wayland, D-Bus)",  AF_UNIX,    SOCK_STREAM, TRUE },
  { "local datagram",                 AF_UNIX,    SOCK_DGRAM,  TRUE },
  { "local seqpacket, cloexec",       AF_UNIX,    SOCK_SEQPACKET | SOCK_CLOEXEC, TRUE },
};

/* Raw syscall, so 64-bit domains reach the filter unchanged. */
static int
raw_socket (long domain, int type)
{
  return (int) syscall (SYS_socket, domain, (long) type, 0L);
}

static gboolean
check_socket (JrCases *c, const char *name, long domain, int type, gboolean allowed)
{
  errno = 0;
  int fd = raw_socket (domain, type);
  int err = errno;
  if (fd >= 0)
    close (fd);
  gboolean ok = allowed ? fd >= 0 : (fd == -1 && err == EACCES);
  jr_case (c, name, ok, "Input domain %ld type %#x  Expected %s  Got %s", domain, type,
           allowed ? "a socket" : "EACCES", fd >= 0 ? "a socket" : g_strerror (err));
  return !allowed;
}

static void
test_socket_table (void)
{
  if (g_test_subprocess ())
    {
      enter_or_die ();
      JrCases c = { "sockets", 0, 0 };
      guint blocked = 0;
      for (gsize i = 0; i < G_N_ELEMENTS (socket_cases); i++)
        {
          const SocketCase *s = &socket_cases[i];
          blocked += check_socket (&c, s->name, s->domain, s->type, s->allowed);
        }

      int pair[2];
      jr_case (&c, "local socketpair", socketpair (AF_UNIX, SOCK_STREAM, 0, pair) == 0,
               "Expected a pair  Got %s", g_strerror (errno));
      close (pair[0]);
      close (pair[1]);
      errno = 0;
      gboolean inet_pair = socketpair (AF_INET, SOCK_STREAM, 0, pair) == -1 && errno == EACCES;
      jr_case (&c, "internet socketpair", inet_pair, "Expected EACCES  Got %s", g_strerror (errno));
      blocked++;

      jr_case (&c, "every attempt counted", jr_netjail_blocked () == blocked,
               "Expected %u  Got %u", blocked, jr_netjail_blocked ());
      jr_cases_done (&c);
      return;
    }
  jailed (NULL);
  g_test_trap_assert_passed ();
  g_test_trap_assert_stderr (LOG_LINE);
}

/* Hidden test: random domains and types against the obvious reference
 * "allowed only when the domain is exactly AF_UNIX". */
static void
test_socket_hidden (void)
{
  if (g_test_subprocess ())
    {
      enter_or_die ();
      static const int types[] = { SOCK_STREAM, SOCK_DGRAM, SOCK_RAW, SOCK_SEQPACKET };
      JrCases c = { "sockets (hidden)", 0, 0 };
      for (int i = 0; i < 3000; i++)
        {
          long domain;
          switch (g_test_rand_int_range (0, 3))
            {
            case 0:  domain = g_test_rand_int_range (-10, 300); break;
            case 1:  domain = AF_UNIX; break;
            default: domain = (long) (((guint64) g_test_rand_int () << 32) | (guint32) g_test_rand_int ());
            }
          int type = types[g_test_rand_int_range (0, G_N_ELEMENTS (types))];
          if (g_test_rand_bit ())
            type |= SOCK_CLOEXEC;
          if (domain == AF_UNIX && type == (SOCK_RAW | SOCK_CLOEXEC))
            type = SOCK_DGRAM; /* the kernel rejects raw local sockets itself */
          if (domain == AF_UNIX && type == SOCK_RAW)
            type = SOCK_STREAM;
          check_socket (&c, "random", domain, type, domain == AF_UNIX);
        }
      jr_cases_done (&c);
      return;
    }
  jailed (NULL);
  g_test_trap_assert_passed ();
}

/* ---- nothing goes out, nothing comes back -------------------------- */

static void
test_nothing_reaches_a_listener (void)
{
  if (g_test_subprocess ())
    {
      g_assert_nonnull (g_getenv ("JR_PORT")); /* the listener is up */
      enter_or_die ();
      g_assert_true (refused (AF_INET, SOCK_STREAM));
      /* Not even a datagram can be sent. */
      g_assert_true (refused (AF_INET, SOCK_DGRAM));
      return;
    }
  /* A real listener, outside the jail. */
  int srv = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (srv < 0)
    {
      g_printerr ("cannot create the test listener: %s\n", g_strerror (errno));
      g_assert_not_reached ();
    }
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl (INADDR_LOOPBACK) };
  g_assert_cmpint (bind (srv, (struct sockaddr *) &addr, sizeof addr), ==, 0);
  g_assert_cmpint (listen (srv, 4), ==, 0);
  socklen_t len = sizeof addr;
  g_assert_cmpint (getsockname (srv, (struct sockaddr *) &addr, &len), ==, 0);
  g_autofree char *port = g_strdup_printf ("%d", ntohs (addr.sin_port));

  /* Sanity: outside the jail the same connect does arrive. */
  int probe = socket (AF_INET, SOCK_STREAM, 0);
  gboolean connected = probe >= 0 && connect (probe, (struct sockaddr *) &addr, sizeof addr) == 0;
  int got = connected ? accept (srv, NULL, NULL) : -1;
  if (got >= 0)
    close (got);
  if (probe >= 0)
    close (probe);
  g_assert_true (connected);
  g_assert_cmpint (got, >=, 0);

  g_auto (GStrv) env = g_environ_setenv (g_get_environ (), "JR_PORT", port, TRUE);
  g_test_trap_subprocess_with_envp (NULL, (const char *const *) env, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed ();

  /* Nothing reached the listener, so nothing could be answered. */
  errno = 0;
  int late = accept (srv, NULL, NULL);
  int err = errno;
  if (late >= 0)
    close (late);
  close (srv);
  g_assert_cmpint (late, ==, -1);
  g_assert_true (err == EAGAIN || err == EWOULDBLOCK);
}

/* ---- other doors ---------------------------------------------------- */

/* io_uring can open sockets without the socket() syscall. */
static void
test_io_uring_closed (void)
{
  if (g_test_subprocess ())
    {
      enter_or_die ();
      struct io_uring_params p = { 0 };
      errno = 0;
      long r = syscall (SYS_io_uring_setup, 4L, &p);
      g_assert_cmpint (r, ==, -1);
      g_assert_cmpint (errno, ==, EACCES);
      return;
    }
  jailed (NULL);
  g_test_trap_assert_passed ();
}

/* x32 system calls share x86_64's architecture tag; they must not slip
 * past the filter. The process may be killed or get an error, but never
 * a socket. */
static void
test_x32_closed (void)
{
#if defined(__x86_64__)
  if (g_test_subprocess ())
    {
      enter_or_die ();
      long r = syscall (0x40000000L | 41 /* x32 socket */, (long) AF_INET, (long) SOCK_STREAM, 0L);
      if (r >= 0)
        g_print (GOT_FD "\n");
      return;
    }
  jailed (NULL);
  g_test_trap_assert_stdout_unmatched ("*" GOT_FD "*");
#else
  g_test_skip ("x32 exists only on x86_64");
#endif
}

/* Threads that already exist are jailed too. */
typedef struct {
  GMutex   lock;
  GCond    cond;
  gboolean go;
  gboolean refused;
} ThreadTry;

static gpointer
thread_try (gpointer data)
{
  ThreadTry *t = data;
  g_mutex_lock (&t->lock);
  while (!t->go)
    g_cond_wait (&t->cond, &t->lock);
  g_mutex_unlock (&t->lock);
  t->refused = refused (AF_INET6, SOCK_DGRAM);
  return NULL;
}

static void
test_existing_threads (void)
{
  if (g_test_subprocess ())
    {
      ThreadTry t = { .refused = FALSE };
      g_mutex_init (&t.lock);
      g_cond_init (&t.cond);
      GThread *th = g_thread_new ("early", thread_try, &t); /* started before the jail */
      enter_or_die ();
      g_mutex_lock (&t.lock);
      t.go = TRUE;
      g_cond_signal (&t.cond);
      g_mutex_unlock (&t.lock);
      g_thread_join (th);
      g_assert_true (t.refused);
      return;
    }
  jailed (NULL);
  g_test_trap_assert_passed ();
}

/* Programs the Journal starts inherit the jail; they have no handler, so
 * the kernel ends them on their first network call. */
static void
test_children_inherit (void)
{
  if (g_test_subprocess ())
    {
      enter_or_die ();
      g_autofree char *self = g_file_read_link ("/proc/self/exe", NULL);
      g_assert_nonnull (self);
      char *argv[] = { self, (char *) "--try-socket", NULL };
      g_autofree char *out = NULL;
      int status = 0;
      g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, &status, NULL));
      g_assert_null (strstr (out, GOT_FD));
      g_assert_false (WIFEXITED (status) && WEXITSTATUS (status) == 0);

      /* The kernel reports the filter on the process itself. */
      g_autofree char *st = NULL;
      g_assert_true (g_file_get_contents ("/proc/self/status", &st, NULL, NULL));
      g_assert_nonnull (strstr (st, "Seccomp:\t2"));
      g_assert_nonnull (strstr (st, "NoNewPrivs:\t1"));
      return;
    }
  jailed (NULL);
  g_test_trap_assert_passed ();
}

/* Entering twice is harmless; the count starts at zero. */
static void
test_twice (void)
{
  if (g_test_subprocess ())
    {
      enter_or_die ();
      enter_or_die ();
      g_assert_cmpuint (jr_netjail_blocked (), ==, 0);
      g_assert_true (refused (AF_INET, SOCK_STREAM));
      g_assert_cmpuint (jr_netjail_blocked (), ==, 1);
      return;
    }
  jailed (NULL);
  g_test_trap_assert_passed ();
  g_test_trap_assert_stderr (LOG_LINE);
}

int
main (int argc, char **argv)
{
  if (argc == 2 && strcmp (argv[1], "--try-socket") == 0)
    return try_socket_child ();

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/netjail/socket-table", test_socket_table);
  g_test_add_func ("/netjail/socket-hidden", test_socket_hidden);
  g_test_add_func ("/netjail/nothing-reaches-a-listener", test_nothing_reaches_a_listener);
  g_test_add_func ("/netjail/io-uring-closed", test_io_uring_closed);
  g_test_add_func ("/netjail/x32-closed", test_x32_closed);
  g_test_add_func ("/netjail/existing-threads", test_existing_threads);
  g_test_add_func ("/netjail/children-inherit", test_children_inherit);
  g_test_add_func ("/netjail/twice", test_twice);
  return g_test_run ();
}
