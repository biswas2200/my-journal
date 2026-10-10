/* text: small helpers on entry text. */
#pragma once

#include <glib.h>

/* Words are runs of non-space characters (Unicode aware). Invalid UTF-8
 * is safe: each broken byte counts as a non-space character. */
guint jr_count_words (const char *text);
