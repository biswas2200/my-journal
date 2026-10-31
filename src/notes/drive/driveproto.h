/* driveproto: everything Notes and its Drive helper say, as pure
 * functions (no network here; drivesync.c does the talking).
 *
 * The backup on Google Drive mirrors Notes:
 *
 *   My Drive / Notes / <subject> / <topic>      (a Google Doc)
 *
 * One Doc per topic, updated in place: Google keeps every earlier
 * version in the Doc's version history. Notes remembers each folder and
 * Doc by Google's id, never by name, so renaming or moving them in Drive
 * changes nothing; one deleted (or in the bin) is made again.
 *
 *   Notes  --request-->  journal-drive  --answer-->  Notes   (JSON on pipes)
 *
 * The request carries a one-hour access token, never the sign-in; each
 * topic goes as Markdown (noteexport.h), which Drive turns into a Doc
 * with its headings, lists and links. The answer says, per topic, where
 * its Doc is, whether the Doc read back holds the same text, and whether
 * anyone but you can open it. */
#pragma once

#include <glib.h>

#define JN_DRIVE_API         "https://www.googleapis.com/"
#define JN_DRIVE_FOLDER_MIME "application/vnd.google-apps.folder"
#define JN_DRIVE_DOC_MIME    "application/vnd.google-apps.document"
#define JN_DRIVE_ROOT_NAME   "Notes"
#define JN_DRIVE_MAX_PORTS   4
#define JN_DRIVE_MAX_ITEMS   20000

/* ---- Notes -> helper -------------------------------------------------------- */

typedef struct {
  gint64  topic;
  char   *subject, *name; /* the subject's and the topic's names */
  char   *markdown;       /* the topic, as exported */
  char   *doc;            /* its Doc from the last backup, or NULL */
  char   *folder;         /* its subject's folder from before, or NULL */
} JnDriveItem;

typedef struct {
  char        *api_base;  /* JN_DRIVE_API, or a test server */
  char        *token;
  guint16      ports[JN_DRIVE_MAX_PORTS];
  gsize        n_ports;
  char        *root;      /* the "Notes" folder from before, or NULL */
  JnDriveItem *items;
  gsize        n_items;
} JnDriveRequest;

/* As JSON for the helper's stdin, and back: anything that is not a whole,
 * valid request is refused (and nothing is left allocated). */
char    *jn_drive_request_to_json   (const JnDriveRequest *r);
/* Strings in `out` are newly allocated: free with jn_drive_request_clear. */
gboolean jn_drive_request_from_json (const char *json, gssize len, JnDriveRequest *out, GError **error);
void     jn_drive_request_clear     (JnDriveRequest *r);

/* ---- helper -> Notes -------------------------------------------------------- */

typedef struct {
  gint64    topic;
  char     *doc, *folder; /* where it is now (also when not verified) */
  char     *link;         /* the Doc's address, to open it */
  gboolean  verified;     /* read back: the same text as the note */
  gboolean  shared;       /* someone besides you can open it */
  char     *error;        /* why it was not backed up, in plain words */
} JnDriveResult;

typedef struct {
  char          *root;    /* the "Notes" folder */
  char          *error;   /* the whole backup failed (sign-in, network) */
  JnDriveResult *results;
  gsize          n_results;
} JnDriveAnswer;

/* The helper's answer as JSON for its stdout, and back. */
char    *jn_drive_answer_to_json   (const JnDriveAnswer *a);
gboolean jn_drive_answer_from_json (const char *json, gssize len, JnDriveAnswer *out, GError **error);
void     jn_drive_answer_clear     (JnDriveAnswer *a);

/* ---- Drive's own messages ------------------------------------------------------ */

/* {"name": …, "mimeType": …, "parents": [parent]} for a new file. */
char    *jn_drive_metadata          (const char *name, const char *mime, const char *parent);
/* A multipart/related upload body: the metadata, then the Markdown.
 * *content_type gets the header value (with the boundary). */
GBytes  *jn_drive_multipart         (const char *metadata, const char *markdown, char **content_type);
/* A file's id, whether it is in the bin, and its address, from Drive's
 * answer (fields id, trashed, webViewLink). FALSE if it holds no id. */
gboolean jn_drive_parse_file        (const char *json, gssize len, char **id, gboolean *trashed, char **link);
/* TRUE (and *shared set) if the permissions list could be read: shared
 * is TRUE when anyone besides the owner, or "anyone with the link", has
 * a role on the file. */
gboolean jn_drive_parse_permissions (const char *json, gssize len, gboolean *shared);
/* Drive's error answer (HTTP `status`) in plain words; never NULL. */
char    *jn_drive_error             (const char *json, gssize len, int status);

/* ---- checking the copy ---------------------------------------------------------- */

/* Does the Doc, read back as Markdown, hold the note's text? Words are
 * compared in order, after both are read as Markdown (markdown.h), so
 * how Google writes the same thing ("*" or "-" bullets, escapes, link
 * forms, blank lines) does not matter, but a missing, changed or extra
 * word does. */
gboolean jn_drive_same_text         (const char *note, const char *doc);
