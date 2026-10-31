/* drivesync: the Drive helper's work, one backup at a time.
 *
 * For each topic of the request, in order:
 *   1. the "Notes" folder in My Drive: the one from before if it is still
 *      there (not deleted, not in the bin), else a new one (once per run);
 *   2. the subject's folder in it: the one from before, renamed to the
 *      subject's name now (so a renamed subject is followed), else a new
 *      one (once per subject per run);
 *   3. the topic's Doc: the one from before, its text and name replaced
 *      in place (Google keeps the old text in the Doc's version history),
 *      else a new Doc in the subject's folder;
 *   4. the Doc read back as Markdown and compared with the note
 *      (jn_drive_same_text): only then is it "verified";
 *   5. who can open it: anyone besides you makes it "shared".
 *
 * An expired sign-in or no connection stops the whole run with one plain
 * error; anything else (a full Drive, a refused file) fails only that
 * topic, and the others go on. Each topic's result is handed to `each`
 * as soon as it is done (the helper writes it out at once), so a run
 * that is stopped still tells Notes about every Doc it made. */
#pragma once

#include <libsoup/soup.h>
#include "driveproto.h"

/* `root` is the Notes folder once known (else NULL). */
typedef void (*JnDriveEach) (const JnDriveResult *result, const char *root, gpointer user);

/* Runs the backup; the answer has one result per item done, in order. */
JnDriveAnswer jn_drive_sync (SoupSession *session, const JnDriveRequest *request, JnDriveEach each, gpointer user);
