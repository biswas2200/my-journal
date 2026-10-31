/* driveclient: Notes' side of the Google Drive backup.
 *
 *   Sign in   the browser opens Google's page; Google sends it back to a
 *             one-time address on this laptop (127.0.0.1, a random port,
 *             open only during the sign-in); Notes trades the code for
 *             tokens and keeps the refresh token in your login keyring.
 *   Back up   Notes trades the refresh token for a one-hour access token
 *             and starts the Drive helper (journal-drive) with an empty
 *             environment, giving it the token and the topics. The
 *             refresh token and the client secret never leave Notes.
 *   Sign out  forgets the refresh token here and asks Google to revoke
 *             it, so it stops working everywhere.
 *
 * A JnBackup is what the window calls, so tests can put a stand-in in
 * its place. */
#pragma once

#include <gio/gio.h>
#include "driveproto.h"

typedef void (*JnSignInDone) (gboolean ok, const char *error, gpointer user);
/* `answer` when the helper answered (its results, even if some failed),
 * else `error`; both only for the call. */
typedef void (*JnBackupDone) (const JnDriveAnswer *answer, const char *error, gpointer user);

typedef struct JnBackup JnBackup;
struct JnBackup {
  /* Is there a client file to sign in with? (see JnBackupConfig) */
  gboolean (*can_sign_in) (JnBackup *self);
  gboolean (*signed_in)   (JnBackup *self);
  void     (*sign_in)     (JnBackup *self, GCancellable *cancel, JnSignInDone done, gpointer user);
  void     (*sign_out)    (JnBackup *self);
  /* Backs up `items` (copied) into the Notes folder `root` (NULL: none
   * yet). */
  void     (*run)         (JnBackup *self, const char *root, const JnDriveItem *items, gsize n_items,
                           GCancellable *cancel, JnBackupDone done, gpointer user);
  void     (*free)        (JnBackup *self);
};

/* Where the refresh token lives. */
typedef struct {
  char    *(*load)  (gpointer user);                      /* NULL if none */
  gboolean (*save)  (const char *token, gpointer user);
  void     (*clear) (gpointer user);
  gpointer user;
} JnSecretStore;

/* The GNOME login keyring ("Journal Notes: Google Drive"). */
JnSecretStore jn_keyring_store (void);

typedef struct {
  const char   *helper;      /* journal-drive */
  const char   *client_file; /* NULL: ~/.config/journal-notes/google-client.json */
  const char   *auth_url, *token_url, *revoke_url, *api_base; /* NULL: Google's */
  guint16       ports[JN_DRIVE_MAX_PORTS];                    /* none given: 443 */
  gsize         n_ports;
  JnSecretStore store;       /* all NULL: the login keyring */
  /* Opens the sign-in page; NULL: the default browser. */
  void        (*open_uri) (const char *uri, gpointer user);
  gpointer      open_user;
} JnBackupConfig;

/* The real backup: Google's sign-in and Drive, through journal-drive. */
JnBackup *jn_google_backup_new (const JnBackupConfig *config);

/* ~/.config/journal-notes/google-client.json: where the "Desktop app"
 * client file goes. */
char     *jn_backup_client_path (void);
