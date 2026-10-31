/* journal-drive: Notes' Google Drive helper. One run per backup.
 *
 * Reads one request (driveproto.h) on stdin, locks itself (notesjail.h:
 * no files at all, TCP only to the ports Notes allows, no listening),
 * does the backup (drivesync.h) and writes one answer on stdout. It
 * holds a one-hour access token, never the sign-in itself, and reads no
 * web pages: it only talks to Google Drive. Without its lock it does
 * nothing. */
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <libsoup/soup.h>
#include "driveproto.h"
#include "drivesync.h"
#include "notesjail.h"

/* Every topic's text in one go: room for years of notes. */
#define MAX_REQUEST (512 * 1024 * 1024)
#define DRIVE_TIMEOUT 120

static int
answer (const JnDriveAnswer *a)
{
  g_autofree char *json = jn_drive_answer_to_json (a);
  fputs (json, stdout);
  fflush (stdout);
  return a->error != NULL ? 1 : 0;
}

static int
fail (const char *error)
{
  JnDriveAnswer a = { NULL, (char *) error, NULL, 0 };
  return answer (&a);
}

static char *
read_request (gsize *len)
{
  GString *s = g_string_new (NULL);
  char buf[65536];
  size_t n;
  while ((n = fread (buf, 1, sizeof buf, stdin)) > 0 && s->len < MAX_REQUEST)
    g_string_append_len (s, buf, (gssize) n);
  *len = s->len;
  return g_string_free (s, FALSE);
}

int
main (void)
{
  /* A crash must not write the token to disk, and nothing may read it. */
  struct rlimit none = { 0, 0 };
  setrlimit (RLIMIT_CORE, &none);
  prctl (PR_SET_DUMPABLE, 0, 0, 0, 0);
  /* No desktop services: no settings store, no D-Bus, no virtual file
   * systems. The helper talks to Google Drive, directly. */
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  g_setenv ("GIO_USE_VOLUME_MONITOR", "unix", TRUE);
  g_unsetenv ("DBUS_SESSION_BUS_ADDRESS");

  gsize len = 0;
  g_autofree char *raw = read_request (&len);
  JnDriveRequest req = { 0 };
  if (len >= MAX_REQUEST || !jn_drive_request_from_json (raw, (gssize) len, &req, NULL))
    return fail ("the request to the Drive helper was not valid");
  memset (raw, 0, len); /* it held the token */

  JnJailPaths *paths = jn_jail_helper_paths (req.ports, req.n_ports);
  g_autoptr (GError) error = NULL;
  gboolean locked = jn_jail_enter (paths, &error);
  jn_jail_paths_free (paths);
  if (!locked)
    {
      g_autofree char *msg = g_strdup_printf ("the Drive helper could not lock itself (%s), so it did nothing",
                                              error->message);
      jn_drive_request_clear (&req);
      return fail (msg);
    }

  SoupSession *session = soup_session_new_with_options ("timeout", DRIVE_TIMEOUT, "user-agent",
                                                        "Journal-Notes/1.0 (backup)", "proxy-resolver", NULL, NULL);
  JnDriveAnswer a = jn_drive_sync (session, &req);
  int rc = answer (&a);
  jn_drive_answer_clear (&a);
  jn_drive_request_clear (&req);
  g_object_unref (session);
  return rc;
}
