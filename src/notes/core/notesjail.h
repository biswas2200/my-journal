/* notesjail: the folders Notes may touch, enforced by the kernel.
 *
 * Notes is the part with network and AI, so it must never be able to
 * read the Journal, even if a bug or a hostile page took it over. With
 * Landlock (an allow-list that a program puts on itself, no root needed)
 * Notes keeps only:
 *   read        system folders (/usr, /etc, ...), fonts, icons, themes,
 *               and what is in ~/.local/share, except the journal and
 *               the login keyring (and, for a build tree, the one file
 *               $JOURNAL_AI_HELPER names)
 *   read/write  its own data (~/.local/share/journal-notes), settings and
 *               caches (~/.config, ~/.cache, ~/.local/state), the runtime
 *               folder, /dev, and ~/Documents/Notes for exports; never a
 *               temp folder others can write to (/tmp)
 * Everything else on disk, the journal first, fails with EACCES, also
 * through symlinks, hard links, renames or /proc, and for every program
 * Notes starts. It cannot be undone. */
#pragma once

#include <glib.h>

#define JN_JAIL_MAX_PORTS 8

typedef struct {
  char   **read;  /* NULL-terminated absolute paths */
  char   **write;
  /* With limit_net, TCP may only connect to these ports and never
   * listen (needs Landlock 4, kernel 6.7; refused on older kernels). */
  gboolean limit_net;
  guint16  ports[JN_JAIL_MAX_PORTS];
  gsize    n_ports;
} JnJailPaths;

/* The lists above for this user (from $HOME and the XDG variables);
 * creates Notes' own folders if missing. */
JnJailPaths *jn_jail_default_paths (void);
/* The AI helper's lock: reads only system code, website certificates
 * and network settings, writes nowhere, connects only to `ports`. */
JnJailPaths *jn_jail_helper_paths  (const guint16 *ports, gsize n_ports);
void         jn_jail_paths_free    (JnJailPaths *paths);
/* TRUE if this kernel can do it (Landlock enabled). */
gboolean     jn_jail_supported     (void);
/* Locks this process (and threads started later, and programs it runs)
 * to the paths; folders that do not exist are skipped. */
gboolean     jn_jail_enter         (const JnJailPaths *paths, GError **error);
