/* harden: see harden.h. */
#include "harden.h"

#include <sys/prctl.h>
#include <sys/resource.h>

gboolean
jr_harden_process (void)
{
  struct rlimit none = { 0, 0 };
  gboolean ok = setrlimit (RLIMIT_CORE, &none) == 0;
  ok = prctl (PR_SET_DUMPABLE, 0, 0, 0, 0) == 0 && ok;
  return ok;
}
