/* netjail: see netjail.h. */
#include "netjail.h"

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <ucontext.h>
#include <unistd.h>
#include <seccomp.h>

static volatile sig_atomic_t blocked;
static gboolean entered;

/* SIGSYS from the filter: the call did not happen. Make it return
 * -EACCES as if the kernel had refused it, count it, and say so.
 * Only async-signal-safe calls in here. */
static void
on_sigsys (int sig, siginfo_t *info, void *context)
{
  (void) sig; (void) info;
  int saved = errno;
  blocked++;
  static const char msg[] = "journal: blocked a network call (the journal has no network)\n";
  ssize_t w = write (STDERR_FILENO, msg, sizeof msg - 1);
  (void) w;
  ucontext_t *uc = context;
#if defined(__x86_64__)
  uc->uc_mcontext.gregs[REG_RAX] = -EACCES;
#elif defined(__aarch64__)
  uc->uc_mcontext.regs[0] = (unsigned long long) -EACCES;
#else
#error "netjail: set the system call return register for this architecture"
#endif
  errno = saved;
}

static gboolean
add (scmp_filter_ctx f, int syscall, unsigned n, const struct scmp_arg_cmp *arg)
{
  return seccomp_rule_add_array (f, SCMP_ACT_TRAP, syscall, n, arg) == 0;
}

gboolean
jr_netjail_enter (void)
{
  if (entered)
    return TRUE;

  struct sigaction sa = { 0 };
  sa.sa_sigaction = on_sigsys;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset (&sa.sa_mask);
  if (sigaction (SIGSYS, &sa, NULL) != 0)
    return FALSE;

  /* Everything not listed is allowed; system calls of another
   * architecture (i386, x32) kill the process. */
  scmp_filter_ctx f = seccomp_init (SCMP_ACT_ALLOW);
  if (f == NULL)
    return FALSE;
  gboolean ok = seccomp_attr_set (f, SCMP_FLTATR_ACT_BADARCH, SCMP_ACT_KILL_PROCESS) == 0;
  /* All threads, including ones started before this call. */
  ok = ok && seccomp_attr_set (f, SCMP_FLTATR_CTL_TSYNC, 1) == 0;

  /* The full 64-bit domain must be exactly AF_UNIX; anything else,
   * including AF_UNIX with high bits set, is refused. */
  const struct scmp_arg_cmp not_local[] = { SCMP_A0_64 (SCMP_CMP_NE, AF_UNIX) };
  ok = ok && add (f, SCMP_SYS (socket), 1, not_local);
  ok = ok && add (f, SCMP_SYS (socketpair), 1, not_local);
  /* io_uring can open and use sockets without these system calls. */
  ok = ok && add (f, SCMP_SYS (io_uring_setup), 0, NULL);
  ok = ok && add (f, SCMP_SYS (io_uring_enter), 0, NULL);
  ok = ok && add (f, SCMP_SYS (io_uring_register), 0, NULL);

  ok = ok && seccomp_load (f) == 0; /* also sets no_new_privs */
  seccomp_release (f);
  if (ok)
    {
      entered = TRUE;
      blocked = 0;
    }
  return ok;
}

guint
jr_netjail_blocked (void)
{
  return (guint) blocked;
}
