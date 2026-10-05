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
  gint64 m = (seconds + 30) / 60;
  return m < 1 ? 1 : (int) m;
}

void
jr_format_minutes (int minutes, char *buf, gsize len)
{
  int h = minutes / 60, m = minutes % 60;
  if (h == 0)
    g_snprintf (buf, len, "%d min", m);
  else if (m == 0)
    g_snprintf (buf, len, "%d h", h);
  else
    g_snprintf (buf, len, "%d h %d min", h, m);
}
