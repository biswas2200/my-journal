/* lockout: see lockout.h. */
#include "lockout.h"

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

gboolean
jr_lockout_pin_blocked (const JrLockout *l)
{
  return l->pin_failed >= JR_PIN_MAX_TRIES;
}

int
jr_lockout_pin_tries_left (const JrLockout *l)
{
  return JR_PIN_MAX_TRIES - l->pin_failed;
}

void
jr_lockout_pin_fail (JrLockout *l)
{
  if (!jr_lockout_pin_blocked (l))
    l->pin_failed++;
}

void
jr_lockout_pin_success (JrLockout *l)
{
  *l = (JrLockout){ 0 };
}

void
jr_lockout_to_string (const JrLockout *l, char *buf, gsize len)
{
  g_snprintf (buf, len, "%d:%" G_GINT64_FORMAT ":%d", l->failed, l->until, l->pin_failed);
}

/* Plain decimal digits in [0, max]; no sign, spaces or overflow. */
static gboolean
read_number (const char *s, gint64 max, gint64 *out)
{
  if (s == NULL || !g_ascii_isdigit (*s))
    return FALSE;
  return g_ascii_string_to_signed (s, 10, 0, max, out, NULL);
}

gboolean
jr_lockout_from_string (const char *s, JrLockout *out)
{
  if (s == NULL)
    return FALSE;
  g_auto (GStrv) parts = g_strsplit (s, ":", 0);
  guint n = g_strv_length (parts);
  gint64 failed, until, pin_failed = 0;
  if ((n != 2 && n != 3) || !read_number (parts[0], JR_LOCKOUT_MAX_TRIES - 1, &failed) ||
      !read_number (parts[1], G_MAXINT64, &until) ||
      (n == 3 && !read_number (parts[2], JR_PIN_MAX_TRIES, &pin_failed)))
    return FALSE;
  *out = (JrLockout){ (int) failed, until, (int) pin_failed };
  return TRUE;
}
