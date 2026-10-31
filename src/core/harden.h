/* harden: process-level protections for a program that holds secrets.
 *
 * Marks the process "not dumpable": no core file is ever written (a crash
 * cannot spill entry text or the key onto disk), and other processes of
 * the same user cannot attach a debugger or read /proc/<pid>/mem. Call it
 * first thing in main(). */
#pragma once

#include <glib.h>

/* Not dumpable, no core files. FALSE if the kernel refused (the caller
 * decides whether to go on). */
gboolean jr_harden_process (void);
