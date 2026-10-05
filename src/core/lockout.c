/* lockout: see lockout.h. */
#include "lockout.h"

#include <stdio.h>

gint64
jr_lockout_remaining (const JrLockout *l, gint64 now)
{
  if (l->until <= now)
    return 0;
  /* Never wait longer than one lockout, even if the clock moved back. */
  return MIN (l->until - now, (gint64) JR_LOCKOUT_SECS);
}

gboolean
jr_lockout_active (const JrLockout *l, gint64 now)
{
  return jr_lockout_remaining (l, now) > 0;
}

int
jr_lockout_tries_left (const JrLockout *l)
{
  return JR_LOCKOUT_MAX_TRIES - l->failed;
}

void
jr_lockout_fail (JrLockout *l, gint64 now)
{
  if (jr_lockout_active (l, now))
    return;
  if (++l->failed >= JR_LOCKOUT_MAX_TRIES)
    {
      l->failed = 0;
      l->until = now + JR_LOCKOUT_SECS;
    }
}

void
jr_lockout_success (JrLockout *l)
{
  l->failed = 0;
  l->until = 0;
}

void
jr_lockout_to_string (const JrLockout *l, char *buf, gsize len)
{
  g_snprintf (buf, len, "%d:%" G_GINT64_FORMAT, l->failed, l->until);
}

gboolean
jr_lockout_from_string (const char *s, JrLockout *out)
{
  int failed, consumed = 0;
  gint64 until;
  if (s == NULL ||
      sscanf (s, "%d:%" G_GINT64_FORMAT "%n", &failed, &until, &consumed) != 2 ||
      s[consumed] != '\0' || failed < 0 || failed >= JR_LOCKOUT_MAX_TRIES || until < 0)
    return FALSE;
  out->failed = failed;
  out->until = until;
  return TRUE;
}
