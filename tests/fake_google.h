/* A fake Google on this machine, for the Drive tests: Drive itself (files
 * with ids, names, parents, a bin, Docs made from uploaded Markdown and
 * read back as Markdown in Google's own style, permissions) and the
 * sign-in service (/oauth/token, /oauth/revoke). Faults can be switched
 * on. Everything runs on its own thread; hold fg_lock to look at it. */
#pragma once

#include <libsoup/soup.h>

#define FG_ACCESS  "ya29.test-token"
#define FG_REFRESH "1//refresh-test"
#define FG_CODE    "4/test-code"
#define FG_SECRET  "GOCSPX-test-secret"

typedef struct {
  char    *id, *name, *mime, *parent, *content;
  gboolean trashed, shared;
} FgFile;

extern char       *fg_base;           /* "http://127.0.0.1:PORT/" */
extern GMutex      fg_lock;
extern GHashTable *fg_files;          /* id -> FgFile */
extern int         fg_requests;       /* Drive requests seen */
extern gboolean    fg_fail_auth;      /* every Drive request: 401 */
extern char       *fg_full_name;      /* making a Doc with this name: Drive is full */
extern char       *fg_lossy_id;       /* reading this Doc back drops its last line */
extern gboolean    fg_bad_token;      /* a Drive request came without FG_ACCESS */
extern char       *fg_last_verifier;  /* the code_verifier of the last code traded */
extern char       *fg_last_redirect;  /* and its redirect_uri */
extern char       *fg_revoked;        /* the last token revoked */
extern int         fg_token_calls;    /* calls to /oauth/token */
extern gboolean    fg_refresh_revoked; /* FG_REFRESH no longer works */

void    fg_start       (void);
int     fg_port        (void);
FgFile *fg_file        (const char *id);
/* Deleting a folder deletes what is in it, as in Drive. */
void    fg_delete_tree (const char *id);
int     fg_count       (void);
