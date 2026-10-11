/* shade: see shade.h. */
#include "shade.h"

int
jr_shade_level (int minutes, gboolean has_entries)
{
  if (minutes > 20)
    return 3;
  if (minutes > 10)
    return 2;
  if (minutes > 0 || has_entries)
    return 1;
  return 0;
}

int
jr_minutes_from_seconds (gint64 seconds)
{
  if (seconds <= 0)
    return 0;
  /* Half a minute rounds up; split so that nothing overflows. */
  gint64 m = seconds / 60 + (seconds % 60 >= 30);
  if (m > G_MAXINT) /* only from a damaged file; never wrap to negative */
    return G_MAXINT;
  return m < 1 ? 1 : (int) m;
}

void
jr_format_minutes (int minutes, char *buf, gsize len)
{
  if (minutes < 0) /* never from the app; don't print "-1 h -1 min" */
    minutes = 0;
  int h = minutes / 60, m = minutes % 60;
  if (h == 0)
    g_snprintf (buf, len, "%d min", m);
  else if (m == 0)
    g_snprintf (buf, len, "%d h", h);
  else
    g_snprintf (buf, len, "%d h %d min", h, m);
}
