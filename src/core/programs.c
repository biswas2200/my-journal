/* programs: see programs.h. */
#include "programs.h"

#include <signal.h>
#include <sys/wait.h>

JrNext
jr_programs_next (int wait_status)
{
  if (WIFEXITED (wait_status))
    switch (WEXITSTATUS (wait_status))
      {
      case JR_EXIT_QUIT:     return JR_NEXT_QUIT;
      case JR_EXIT_HOME:     return JR_NEXT_HOME;
      case JR_EXIT_TO_NOTES: return JR_NEXT_NOTES;
      case JR_EXIT_TO_DIARY: return JR_NEXT_DIARY;
      default:               return JR_NEXT_HOME_AFTER_ERROR;
      }
  if (WIFSIGNALED (wait_status) &&
      (WTERMSIG (wait_status) == SIGTERM || WTERMSIG (wait_status) == SIGINT))
    return JR_NEXT_QUIT;
  return JR_NEXT_HOME_AFTER_ERROR; /* crashed, killed, or not ended */
}

const char *
jr_programs_file (JrNext next)
{
  switch (next)
    {
    case JR_NEXT_DIARY: return "journal-diary";
    case JR_NEXT_NOTES: return "journal-notes";
    default:            return NULL;
    }
}
