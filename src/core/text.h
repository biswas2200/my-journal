/* text: small helpers on entry text. */
#pragma once

#include <glib.h>

/* Words are runs of non-space characters (Unicode aware). */
guint jr_count_words (const char *text);
