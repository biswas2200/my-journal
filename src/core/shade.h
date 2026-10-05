/* shade: calendar shading levels and minute formatting. */
#pragma once

#include <glib.h>

/* 0 = nothing written (outline only), 1 = 1-10 min, 2 = 11-20, 3 = 21+.
 * A day with entries is at least level 1 even under a minute. */
int  jr_shade_level          (int minutes, gboolean has_entries);

/* Rounds to the nearest minute, but any writing at all shows as 1. */
int  jr_minutes_from_seconds (gint64 seconds);

/* "17 min", "1 h 27 min", "2 h". */
void jr_format_minutes       (int minutes, char *buf, gsize len);
