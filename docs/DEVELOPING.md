# Developing Journal and Notes

How the code fits together, the rules it keeps, and how to change it
without breaking them. The user-facing side is in the README; every
test is listed in TESTING.md.

## The programs

```
journal                     the start screen (src/launcher). Starts one of
 │                          the two below at a time; learns only its exit code
 ├── libexec/journal-diary  the Journal (src/diary + src/core + src/ui)
 │                          seccomp network jail + AppArmor: no network at all
 └── libexec/journal-notes  Notes (src/notes/main.c + core + ui)
      │                     Landlock: cannot open the Journal's files or the keyring
      ├── libexec/journal-ai     one run per Enhance (src/notes/ai/helper.c)
      │                          Landlock: no files, TCP 443/80 only
      └── libexec/journal-drive  one run per backup (src/notes/drive/helper.c)
                                 Landlock: no files, TCP 443 only
```

The two helpers get one JSON request on stdin and write their answer on
stdout: journal-ai one JSON object, journal-drive one line per topic as
soon as it is done, then an end line (so a backup cut short still tells
Notes about every Doc it made). They hold a one-hour access token and never the sign-in
itself. They start with an empty environment and refuse to work without
their lock.

## Where things live

| What | Where | Notes |
| --- | --- | --- |
| Journal | `~/.local/share/journal/journal.db` | encrypted entries (src/core/vault.c); never read by Notes or by tests |
| Notes | `~/.local/share/journal-notes/notes.db` | plain SQLite, 0600; `synchronous=FULL` |
| Notes' daily backups | `…/journal-notes/backups/notes-YYYY-MM-DD.db` | 14 kept (src/notes/core/notebackup.c) |
| Rescue copies | `~/Documents/Notes/Unsaved/` | written when a save fails |
| Exports | `~/Documents/Notes/<subject>/<topic>.md` | |
| Google client file | `~/.config/journal-notes/google-client.json` | the user's own Desktop OAuth client |
| Drive sign-in | GNOME login keyring | "Journal Notes: Google Drive" |
| AI sign-in | `~/.config/gcloud/application_default_credentials.json` | gcloud's ADC |

### Changing a store's schema

Both stores are at `user_version = 1`. An older or newer build opens the
same file, so add things in place, never in a way the last release
cannot read:

- `CREATE TABLE IF NOT EXISTS` / `CREATE TRIGGER IF NOT EXISTS` in
  `SCHEMA` (see `study_log`, `topic_changes`, `drive_docs`);
- no renamed or dropped columns, no new `NOT NULL` column without a
  default on an existing table;
- `tools/sanity.sh` checks it: notes from the last release open in this
  tree intact, and open in the last release again.

A topic's `version` (table `topic_changes`) is kept by triggers on
every table that feeds its export. If you add something to the export,
add a trigger, or the backup will not notice the change.

## Rules the code keeps

- **Nothing written is ever lost.**
  - Every save is one transaction.
  - A failed save leaves a rescue copy and keeps retrying.
  - A backup replaces nothing until the new copy is whole and checked.
  - A damaged file is refused, never written to.
- **Secrets stay put.**
  - Tokens and keys are wiped from memory after use (`wipe_free`,
    `memset` before `g_free`).
  - They are never written to a file or put in an error message, and
    never cross into a helper except as the one-hour access token.
- **Outside input is not trusted.** This covers web pages, Google's
  answers, the AI's Markdown and SVG, and a damaged store.
  - Read JSON only through `jsonsafe.h`.
  - Clean SVG through `svgsafe.h`.
  - Turn a page into text through `textify.h`.
  - Each has a fuzz target.
- **Plain words.**
  - Every message a person sees says what happened and what to do, in
    everyday words: no codes, no jargon.
  - The AI's own rules ban corporate jargon too (`RULES` in
    `aiproto.c`).
- **Time is injected** (`gint64 now` arguments, `jn_clock_*` with a
  given clock), so logic is tested without waiting or sleeping.
- **The Journal stays offline.** Nothing network-related is ever linked
  into `journal-diary`.
- **Notes never starts a program to show something.** A child of Notes
  inherits its Landlock lock, so a browser started from Notes would run
  locked (and stay so). Open addresses with `jr_open_uri`
  (`src/shared/openuri.h`): it asks the desktop's portal over D-Bus,
  and starts the browser directly only when there is no portal.
- **Closing work happens in `close-request`, not only in `dispose`.**
  GTK 4 frees a window only when its last reference goes; a callback
  still out (an Enhance, a sign-in) can hold one past the end of the
  program. Callbacks the window's own clients make need no reference:
  the clients are freed in `dispose` and end what they do then.

## Code style

- C11 with GLib/GTK 4, GNU style: 2-space indents, a space before `(`,
  functions start on their own line.
- Names: `jr_` for the Journal, `jn_` for Notes, `JR_`/`JN_` for macros.
- Every file starts with a comment saying what it is for, its rules, and
  why. Headers say what each public function does, what it returns, and
  who owns what. A comment in a function body explains *why*, never
  *what*.
- `-Wall -Wextra -Werror`. GCC's `-fanalyzer` must find nothing
  (`tools/analyze.sh`), and that includes CI's older GCC 13.
- In tests, use `die()` or `abort()` instead of `g_error()`: its endless
  loop trips the analyzer.
- In a condition, a function call must not sit on the right of `&&` or
  `||` (Sonar rule S912). Compute it first.

## Tests

| Kind | Where | Run |
| --- | --- | --- |
| Tables and hidden random cases (LeetCode style, `tests/cases.h`) | `tests/test_*.c` | `meson test -C build` |
| UI on a private headless GNOME Shell | `tests/run-headless.sh` | part of the above |
| AddressSanitizer + UBSan | | `meson setup build-asan -Db_sanitize=address,undefined` |
| Valgrind | | `JR_TEST_WRAPPER='valgrind …' meson test --suite core` |
| Fuzzing | `fuzz/` | `fuzz/README.md` |
| Crash (SIGKILL while saving) | `test_crash.c`, `test_notes_crash.c` | core suite |
| Memory growth | `/ui/no-growth`, `/notes-ui/no-growth` | ui suite |
| Load (10 000 topics) | `test_notes_load.c` | `meson test --suite load` |
| Sanity against the last release | `tools/sanity.sh` | before every release |

To add a feature:
1. Write its test first, as a table of cases plus a hidden random one
   where it fits.
2. Watch the test fail, then implement until it passes.
3. To prove a test can catch something, plant the bug and see it fail
   (memory tests: plant a leak).

Google is faked on this machine (`tests/fake_google.c`: Drive and the
sign-in service). Gemini is faked in `test_ai_helper.c`. Tests never
reach the internet, never touch the real keyring, and never show a
window on your desktop.

## CI

`.github/workflows/ci.yml` runs on every push:
- build, all tests with coverage, Valgrind and the analyzer;
- sanitizers;
- `tools/sanity.sh` against the newest tag;
- each of the 11 fuzz targets for 2 minutes;
- SonarQube Cloud, whose quality gate must pass with 0 open issues.

CI runs Ubuntu 24.04, which is older than a current laptop: json-glib
1.8 and GCC 13. When CI alone fails, reproduce it in a throwaway
`ubuntu:24.04` container, then remove the container. Fuzz builds there
need `libclang-rt-18-dev`. CI machines are slower too, so the load test
runs with `JN_LOAD_SLACK=4`.

## Releasing

1. Bump `version` in `meson.build`, and add its section to
   `CHANGELOG.md`.
2. Run `tools/sanity.sh` and wait for CI and Sonar to pass.
3. Run `git tag -a vX.Y.Z` and push the tag. `.github/workflows/release.yml`
   publishes the source archive and that version's notes.

## Lessons already paid for

- `MAX (a, f ())` calls `f` twice: it is a macro.
- `pkill -f PATTERN` can match your own shell command.
- A backgrounded shell function is a subshell, so `$!` is not the
  program you started.
- GTK layout and styles in tests:
  - Layout tests must load `notes.css` from the resource, or GTK's
    defaults hide overflow.
  - A rule like `.sidebar list > row` beats `row.current`, so scope
    style rules the same way.
- A blocking read in a main-loop callback freezes the window for as long
  as the other side likes. The sign-in's return address once waited 5 s
  on a connection that said nothing; read it asynchronously, with a
  time limit.
- SQLite leaves `….tmp-journal` beside a copy cut off half-way. Remove
  it before the next copy (`remove_partial` in notesdb.c).
- json-glib 1.8 reads past the first JSON value, leaks memory on bad
  `\u` escapes, and passes lone surrogates through. `jsonsafe.h` checks
  the text before json-glib sees it.
