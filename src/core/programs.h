/* programs: how the launcher and its two programs hand over.
 *
 * The launcher (`journal`) shows the start screen and runs the Journal
 * (`journal-diary`) or Notes (`journal-notes`) as a separate process. It
 * never opens either one's data. The only thing a program tells it is
 * its exit status:
 *
 *   0 QUIT       window closed: everything closes
 *   10 HOME      back to the start screen
 *   11 TO_NOTES  switch to Notes
 *   12 TO_DIARY  switch to the Journal
 *
 * Ending the Journal's process is what locks it: its keys and text go
 * with the process, and the next visit starts locked. Death by SIGTERM
 * or SIGINT (logout, Ctrl+C) quits; any other code or signal returns to
 * the start screen with an error. */
#pragma once

#include <glib.h>

enum {
  JR_EXIT_QUIT     = 0,
  JR_EXIT_HOME     = 10,
  JR_EXIT_TO_NOTES = 11,
  JR_EXIT_TO_DIARY = 12,
};

typedef enum {
  JR_NEXT_QUIT,
  JR_NEXT_HOME,
  JR_NEXT_DIARY,
  JR_NEXT_NOTES,
  JR_NEXT_HOME_AFTER_ERROR,
} JrNext;

/* What to do after a program ended with `wait_status` (from waitpid). */
JrNext      jr_programs_next (int wait_status);
/* The program file to start for DIARY or NOTES, else NULL. */
const char *jr_programs_file (JrNext next);
