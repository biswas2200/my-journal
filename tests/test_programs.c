/* Tests for what the launcher does when the Journal or Notes program
 * ends (src/core/programs.c), LeetCode style (see cases.h).
 *
 * Constraints: the input is any status waitpid() can report: a normal
 * exit with a code 0..255, or death by a signal 1..64. The launcher only
 * trusts the agreed codes; anything else means the program failed. */
#include <signal.h>
#include <sys/wait.h>
#include <glib.h>
#include "cases.h"
#include "programs.h"

static const char *
next_name (JrNext n)
{
  switch (n)
    {
    case JR_NEXT_QUIT:  return "quit";
    case JR_NEXT_HOME:  return "home";
    case JR_NEXT_DIARY: return "journal";
    case JR_NEXT_NOTES: return "notes";
    case JR_NEXT_HOME_AFTER_ERROR: return "home with an error";
    }
  return "?";
}

typedef struct {
  const char *name;
  int         status; /* as waitpid() reports it */
  JrNext      expected;
} NextCase;

static const NextCase next_cases[] = {
  { "window closed",                 W_EXITCODE (JR_EXIT_QUIT, 0),      JR_NEXT_QUIT },
  { "Home button",                   W_EXITCODE (JR_EXIT_HOME, 0),      JR_NEXT_HOME },
  { "Journal → Notes",               W_EXITCODE (JR_EXIT_TO_NOTES, 0),  JR_NEXT_NOTES },
  { "Notes → Journal",               W_EXITCODE (JR_EXIT_TO_DIARY, 0),  JR_NEXT_DIARY },
  { "could not open (exit 1)",       W_EXITCODE (1, 0),                 JR_NEXT_HOME_AFTER_ERROR },
  { "unknown code 13",               W_EXITCODE (13, 0),                JR_NEXT_HOME_AFTER_ERROR },
  { "unknown code 255",              W_EXITCODE (255, 0),               JR_NEXT_HOME_AFTER_ERROR },
  { "logout (SIGTERM, unhandled)",   W_EXITCODE (0, SIGTERM),           JR_NEXT_QUIT },
  { "Ctrl+C in a terminal",          W_EXITCODE (0, SIGINT),            JR_NEXT_QUIT },
  { "crash (SIGSEGV)",               W_EXITCODE (0, SIGSEGV),           JR_NEXT_HOME_AFTER_ERROR },
  { "assertion (SIGABRT)",           W_EXITCODE (0, SIGABRT),           JR_NEXT_HOME_AFTER_ERROR },
  { "killed (SIGKILL)",              W_EXITCODE (0, SIGKILL),           JR_NEXT_HOME_AFTER_ERROR },
  { "ended by the network jail",     W_EXITCODE (0, SIGSYS),            JR_NEXT_HOME_AFTER_ERROR },
  { "stopped, not ended",            W_STOPCODE (SIGSTOP),              JR_NEXT_HOME_AFTER_ERROR },
};

static void
test_next_table (void)
{
  JrCases c = { "next", 0, 0 };
  for (gsize i = 0; i < G_N_ELEMENTS (next_cases); i++)
    {
      const NextCase *k = &next_cases[i];
      JrNext got = jr_programs_next (k->status);
      jr_case (&c, k->name, got == k->expected, "Input status %#x  Expected %s  Got %s", k->status,
               next_name (k->expected), next_name (got));
    }
  jr_cases_done (&c);
}

/* Slow reference, written from the rule in programs.h. */
static JrNext
reference (int status)
{
  if (WIFEXITED (status))
    {
      int code = WEXITSTATUS (status);
      return code == 0 ? JR_NEXT_QUIT : code == 10 ? JR_NEXT_HOME : code == 11 ? JR_NEXT_NOTES
           : code == 12 ? JR_NEXT_DIARY : JR_NEXT_HOME_AFTER_ERROR;
    }
  if (WIFSIGNALED (status) && (WTERMSIG (status) == SIGTERM || WTERMSIG (status) == SIGINT))
    return JR_NEXT_QUIT;
  return JR_NEXT_HOME_AFTER_ERROR;
}

/* Hidden test: every exit code and every signal. */
static void
test_next_all (void)
{
  JrCases c = { "next (all statuses)", 0, 0 };
  for (int code = 0; code < 256; code++)
    {
      int s = W_EXITCODE (code, 0);
      jr_case (&c, "exit", jr_programs_next (s) == reference (s), "Input exit %d  Expected %s  Got %s",
               code, next_name (reference (s)), next_name (jr_programs_next (s)));
    }
  for (int sig = 1; sig < 65; sig++)
    {
      int s = W_EXITCODE (0, sig);
      jr_case (&c, "signal", jr_programs_next (s) == reference (s), "Input signal %d  Expected %s  Got %s",
               sig, next_name (reference (s)), next_name (jr_programs_next (s)));
    }
  jr_cases_done (&c);
}

/* Which program file the launcher may start: only the two fixed names. */
static void
test_program_names (void)
{
  JrCases c = { "program", 0, 0 };
  jr_case (&c, "journal", g_strcmp0 (jr_programs_file (JR_NEXT_DIARY), "journal-diary") == 0,
           "Expected journal-diary  Got %s", jr_programs_file (JR_NEXT_DIARY));
  jr_case (&c, "notes", g_strcmp0 (jr_programs_file (JR_NEXT_NOTES), "journal-notes") == 0,
           "Expected journal-notes  Got %s", jr_programs_file (JR_NEXT_NOTES));
  const JrNext none[] = { JR_NEXT_QUIT, JR_NEXT_HOME, JR_NEXT_HOME_AFTER_ERROR, (JrNext) 99, (JrNext) -1 };
  for (gsize i = 0; i < G_N_ELEMENTS (none); i++)
    jr_case (&c, "no program", jr_programs_file (none[i]) == NULL, "Input %d  Expected NULL  Got %s",
             (int) none[i], jr_programs_file (none[i]));
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/programs/next", test_next_table);
  g_test_add_func ("/programs/next-all", test_next_all);
  g_test_add_func ("/programs/names", test_program_names);
  return g_test_run ();
}
