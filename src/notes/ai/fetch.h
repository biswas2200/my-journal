/* fetch: the AI helper fetching the links in Sources, carefully.
 *
 * Each link is checked (netguard.h) and its host looked up first; every
 * connection, redirects included, is checked again once made and cut off
 * before a byte is sent if it reached a private address. At most
 * JN_FETCH_MAX_BYTES are read per page, within JN_FETCH_TIMEOUT seconds;
 * HTML and plain text become text (textify.h), other kinds of file are
 * skipped with a reason. Each link gets text or a problem, never both. */
#pragma once

#include <libsoup/soup.h>
#include "aiproto.h"

#define JN_FETCH_MAX_LINKS 8
#define JN_FETCH_MAX_BYTES (3 * 1024 * 1024)
#define JN_FETCH_MAX_TEXT  60000   /* characters of text kept per page */
#define JN_FETCH_TIMEOUT   20

/* One JnAiSource per non-empty line of `links` (up to the maximum).
 * `allow_private` exists for the tests' local server only; the helper
 * always passes FALSE. */
JnAiSource *jn_fetch_sources (SoupSession *session, char **links, gboolean allow_private, gsize *n_sources);
void        jn_fetch_free    (JnAiSource *sources, gsize n_sources);

/* One already-checked link. Tests only may switch the two guards off one
 * at a time: the host lookup before connecting, and the address check
 * on every connection made. */
void        jn_fetch_url     (SoupSession *session, const char *url, gboolean check_host,
                              gboolean check_connection, JnAiSource *out);
