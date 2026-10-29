# Journal

A private daily journal for one person, running as a small native app on
Linux (GTK4, C11). Write the loop down once, so it stops running in your
head. Everything stays in one encrypted file on this laptop: no network,
no accounts, no telemetry, no sync.

The start screen also opens **Notes**, for study notes: a separate
program that never sees the journal, with an optional AI "enhanced copy"
of each note (see [Notes](#notes) and [AI enhanced copy](#ai-enhanced-copy)).

## What it does (V1)

1. **Daily journal with a day dropdown.** Opens on today with the cursor
   ready. `Ctrl+K` opens a dropdown with this week, earlier months, and a
   "Jump to a date" field (`4 Oct`, `Oct 4`, `2026-10-04`, `yesterday`).
   Past days open read-only, with an **Edit** button.
2. **Automatic time stamps.** Each entry is a block stamped from the
   system clock on its first keystroke. `Ctrl+Enter` starts a new block.
   An entry written at 00:30 belongs to the new day.
3. **Activity and time.** A month calendar shaded by minutes written,
   days written, total time, average per day, entries, and the last 7
   days as a bar chart. Minutes count only while typing and pause after
   60 seconds idle. An empty day is just empty: no streaks, no "missed".
4. **Lock.** Optional: a passphrase (12 or more characters), or a 6-digit
   PIN together with a master passphrase. It locks when the app closes
   and when the laptop sleeps or the lid closes. After 5 wrong PINs the
   PIN is blocked: only the master passphrase (or the recovery key) opens
   the journal, and then a new PIN, different from the old one, must be
   chosen. After 5 wrong passphrases it waits 30 seconds. A restart
   resets neither.
5. **Dark, monochrome theme** that uses only system fonts.

## Build and install

```bash
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-4-dev libsqlite3-dev libsodium-dev libsecret-1-dev libseccomp-dev
meson setup build
meson compile -C build
meson test -C build            # unit + UI tests
sudo meson install -C build    # programs, .desktop files, icon, AppArmor profile
sudo tools/install-apparmor.sh # load the second network lock (once)
tools/check-apparmor.sh        # optional: probe that it refuses the network
```

Run it from the app grid ("Journal") or with `journal`; the start screen
offers the Journal and Notes. `journal --open=journal` (or `notes`) skips
it, as do the two actions in the app menu. To uninstall:
`sudo ninja -C build uninstall`, then remove the AppArmor profile as
`tools/install-apparmor.sh` explains.

The journal lives at `$XDG_DATA_HOME/journal/journal.db`
(normally `~/.local/share/journal/journal.db`), created with mode 0600 in
a 0700 folder. Set `JOURNAL_DB=/some/other.db` to try the app on a
scratch file without touching the real one.

## Keyboard

| Keys | Action |
| --- | --- |
| `Ctrl+K` | Open the day dropdown |
| `Ctrl+Enter` | New stamped block (always on today's page) |
| `Ctrl+L` | Lock now (opens Lock & security if no lock is set) |
| `Esc` | Close the dropdown or a PIN/passphrase dialog |
| `Ctrl+Q` | Quit (saves, then wipes the key and text from memory) |

On the lock screen, type the six digits; it unlocks on the sixth.
`Backspace` removes a digit and `Esc` clears them. With a passphrase,
type it and press Enter.

## Notes

Subjects (DSA) hold topics (Prefix sum), which hold study sessions: one
note each, timed from its first keystroke to the last edit while you
study (an edit more than 30 minutes later, like fixing a typo next week,
keeps the time you studied). Opening a topic creates nothing until you
type; a session left empty is removed. Notes save about a second after
you stop typing, and when you switch, export or leave.

Five formats and plain text, with no syntax on screen:

| Format | Toolbar | Keys | Or type at the start of a line |
| --- | --- | --- | --- |
| Heading | H | `Ctrl+1` | `# ` |
| Subheading | h | `Ctrl+2` | `## ` |
| Bullet ● | ● | `Ctrl+.` | `- ` or `* ` |
| Sub-bullet ◦ | ◦ | `Tab` on a bullet | `  - ` |
| Arrow → | → | `Ctrl+-` | `> ` or `-> ` |
| Plain text | ¶ | `Ctrl+0` | |

Enter continues a list and ends it on an empty item; Tab and Shift+Tab
indent and outdent; Backspace right after a bullet makes the line text
again; Enter before a heading opens a line above it. **Markdown**
(`Ctrl+M`) shows the note as Markdown to type by hand; both views store
the same lines, a small Markdown subset (`# `, `## `, `- `, `  - `, `→ `,
text; text that would read as a format is escaped with `\`).

The **Sources** box holds the links you study from, one per line.
**Export .md** (`Ctrl+E`) writes the whole topic, every session with its
date, time and sources, to `~/Documents/Notes/<subject>/<topic>.md`.
`Ctrl+N` starts another session on the same topic, `Ctrl+S` saves now.

Notes live in `~/.local/share/journal-notes/notes.db` (0600 in a 0700
folder). They are study material, so the file is not encrypted.

## AI enhanced copy

**✦ Enhance** (`Ctrl+Shift+E`) writes an enhanced copy of the open
session with Gemini 3.8 Flash on Vertex AI, using your Google sign-in
(`gcloud auth application-default login`; the project is the sign-in's
quota project, or `$GOOGLE_CLOUD_PROJECT`). The copy appears read-only
next to your note and is kept with its session; **your note is never
changed**. It keeps your headings, order and points, fills in only from
the pages in Sources (each added fact cited as `[n]`, gaps marked "(not
in the sources)"), writes in your plain style (a few of your other notes
go along as samples; filler words, emojis and hype are ruled out), may
add one diagram, and ends with **Gaps**: what the sources cover that
your note does not yet.

The copy is shown formatted: headings, bullets, code, links (they open
in your browser), `[n]` citations, and math as plain symbols (`1 ≤ k ≤ N`,
never raw LaTeX). A diagram is drawn only after it passes a safety check
that keeps shapes and text and drops scripts, links, images, style sheets,
outside references and oversized drawings; one that fails is not shown.
It shrinks to fit the pane. **★ Topic reference** in the sidebar puts every
enhanced copy of the topic together, oldest first, and exports as
`<topic> (reference).md`.

The first time, Notes asks before anything is sent and says exactly
what goes where. How it stays contained:

- **The journal is never involved**: it has no network (two locks, see
  below) and Notes cannot read its files.
- **Notes keeps your sign-in.** It trades it for a one-hour access token
  at Google; the refresh token and client secret never leave Notes, and
  the token lives only in memory.
- **A separate helper does the network work** (`libexec/journal-ai`),
  one run per Enhance, started with an empty environment. Before any
  network use it locks itself (Landlock): no file of yours can be read or
  written (only system code, website certificates and network settings),
  TCP may reach only ports 443 and 80, nothing may listen. It refuses to
  run without the lock, and it is not dumpable.
- **Sources are fetched carefully**: http and https only, normal ports,
  no `user:password@`; a link or a redirect that leads into this computer
  or its network (localhost, home addresses, link-local cloud metadata,
  IPv4 inside IPv6...) is refused before a byte is sent; at most 8 links,
  3 MB and 20 s each; HTML and plain text only (PDF not yet).
- **Pages cannot steer it**: their text is fenced as reference material,
  the AI is told never to follow instructions in it, and whatever comes
  back is only shown as text, never run.

## Security model (Option B: encrypt at rest)

- A random 32-byte **data key** encrypts each entry body with libsodium
  `secretbox` (XSalsa20-Poly1305). Dates, stamp times and minutes are not
  secret and stay readable, so the Activity screen never decrypts text.
- With a lock set, the data key is never stored in the clear. It is
  stored wrapped with keys derived by **Argon2id** (10 passes, 256 MiB,
  random salt): from the PIN or passphrase, from the master passphrase
  (PIN locks), and from the **recovery key**. A wrong secret fails authentication, so the wrapped
  key also serves as the salted Argon2id check. The PIN or passphrase
  itself is never stored, not even hashed. Keys saved with an older,
  weaker setting are re-saved with the current one the next time they
  unlock.
- **PIN and master passphrase.** A 6-digit PIN has only a million values,
  so on its own it could be guessed from a copy of the file. Two things
  close that:
  - The PIN's key also needs a 32-byte **device secret** kept in your
    login keyring (GNOME Keyring, via libsecret; it shows in Passwords
    and Keys as "Journal PIN key"). The key-encryption key is BLAKE2b
    keyed with that secret over Argon2id(PIN), so a copy of the file
    alone gives nothing to test PIN guesses against. The keyring is
    locked by your login password, so that password now guards the PIN
    path; anyone using your logged-in session can read the keyring, as
    they could anything else in it.
  - **5 wrong PINs block the PIN.** No timer brings it back. Only the
    master passphrase or the recovery key opens the journal then, and a
    new PIN must be chosen first (not the blocked one). Until then no
    entry opens. Wrong PINs typed while changing the lock count too, and
    the 5th locks the journal at once.
  A PIN set by an earlier version is asked for a master passphrase on
  the next unlock; that also moves the PIN onto the keyring.
- **Passphrase.** 12+ characters (a few random words) have far too many
  possibilities to guess even from a copy of the file. Passphrases are
  Unicode-normalized, so the same words typed with a different keyboard
  layout or input method still match.
- The **recovery key** (`XXXX-XXXX-XXXX-XXXX-XXXX-XXXX`, 120 random bits)
  is shown once when the lock is set, or when you ask for a new one. It
  cannot be copied or selected, so it never reaches the clipboard: write
  it down. It unlocks in place of any other secret. If you lose all of
  them, the entries cannot be read by anyone, including you.
- Changing or turning off the lock, or making a new recovery key, asks
  for the current PIN, passphrase or master passphrase first, and wrong
  ones count (5 wrong PINs block the PIN). The slow work runs on a background thread on copies;
  if the journal locks meanwhile (lid closed), nothing is applied.
- Without a lock the data key is stored next to the data. That only keeps
  text out of casual view in tools like `strings`; it is not protection.
- **Locking** (`Ctrl+L`, sleep or lid close, quit) saves, destroys every
  screen and text buffer, and zeroes the key (`sodium_free`). The window
  then shows only the lock screen. Before sleep, a logind "delay"
  inhibitor makes the system wait until the lock is done.
- **Process hardening.** At startup the app makes itself non-dumpable and
  disables core dumps: a crash never writes entry text or keys to disk,
  and other programs running as you cannot attach a debugger or read its
  memory (`/proc/<pid>/mem`). The writing area tells input methods it is
  private, so they do not learn or remember your words.
- **Two programs, kept apart.** `journal` is only the start screen: it
  links none of the Journal's code, never opens its file, and learns
  nothing from a program but its exit code. The Journal
  (`libexec/journal-diary`) and Notes (`libexec/journal-notes`) are
  separate processes, one at a time. Leaving the Journal (Home, or
  switching to Notes) saves, wipes and ends its process, so every visit
  starts at the lock screen.
- **No network, two locks.** The Journal needs none: its only outside
  contact is local, the display, the session bus and logind ("about to
  sleep"). It is made unable to reach the network at all:
  1. *seccomp jail* (`src/core/netjail.c`), entered first thing in
     `main()`, before any thread: only local `AF_UNIX` sockets can be
     created. Internet, IPv6, raw, packet, netlink and every other
     socket, and `io_uring` (which could open sockets without
     `socket()`), fail with `EACCES` before anything is sent: no request
     leaves, so no answer can come back. Every attempt is counted and
     logged. It covers threads that already exist and every program the
     Journal starts, cannot be undone, and if the kernel refuses it the
     Journal does not start.
  2. *AppArmor profile* (`data/apparmor/journal-diary.in`), enforced by
     the kernel from outside the program: the same socket families are
     denied (also sockets handed over by another process), and so are the
     local doors that would go online on its behalf: systemd-resolved's
     DNS socket, container engines, and D-Bus calls that open links,
     start services or talk to the resolver or network manager.
- **Notes cannot reach the Journal's files.** Notes will be the only part
  with network and AI, so before anything else it locks itself with
  Landlock (`src/notes/core/notesjail.c`), a kernel allow-list a program
  puts on itself: system folders and fonts to read; its own data,
  settings, caches, the runtime folder and `~/Documents/Notes` to write
  (never `/tmp` or another folder others can write to). `~/.local/share/journal` and the login keyring are on no list, so
  opening, listing, linking or renaming them fails with `EACCES`, also
  through symlinks or `/proc/self/root`, and for every program Notes
  starts. Without that lock Notes opens no notes.
- **What the app cannot protect against:** someone who already runs
  programs as you (malware, a stolen login) can wait for you to unlock,
  or swap the launcher for a fake one. An unencrypted disk or swap file
  can hold copies of text that was in memory. Full-disk encryption and a
  strong login password cover what an app cannot.

## Footprint (measured 6 Oct 2026, Ubuntu, GTK 4.22, x86-64)

| What | Value |
| --- | --- |
| Binary (`build/journal`, release) | 146 KiB, 118 KiB stripped |
| Idle memory, today screen | RSS 52 MiB, **PSS 16 MiB, private 10.4 MiB** |
| Startup CPU (to first frame) | ~0.27 s |
| Idle CPU (cursor done blinking) | ~1 ms per second |
| Memory growth, 25 tours of every screen + lock/unlock | ~17 KiB, then flat |
| PIN/passphrase check | ~0.9 s on a worker thread, 256 MiB held only during the check |

Most of the RSS is GTK and its libraries, shared with other GTK apps on
the desktop. PSS (proportional share) and private memory are the fair
numbers. The app sets `GSK_RENDERER=cairo` (software rendering), because
GTK's GPU renderers cost far more memory for a text app: idle RSS was
240 MiB with `ngl` and 160 MiB with `vulkan`. Set `GSK_RENDERER` yourself
to override.

**Proposed budget (to agree):** binary under 300 KiB, idle PSS under
25 MiB, private memory under 16 MiB, no growth across screen switches
(enforced by the `/ui/no-growth` test, which allows 128 KiB of cache
noise).

## How memory is kept low

- Only the visible screen exists. Switching screens destroys the old one,
  and with it its widgets and text buffers.
- The day dropdown builds its content when it opens and frees it when it
  closes.
- SQLite statements are prepared once and reused; rows go to callbacks
  straight from SQLite's buffers (no lists); page cache is 256 KiB.
- Decrypted text exists only for the duration of a callback, and every
  plaintext copy the app makes is zeroed before it is freed.
- One 1-second timer while unlocked; none while locked.

## Tests

```bash
meson test -C build                       # all suites
meson test -C build --suite core          # logic, storage, crypto
meson test -C build --suite ui            # drives the real window, off-screen

# AddressSanitizer + UndefinedBehaviorSanitizer
meson setup build-asan -Db_sanitize=address,undefined -Dbuildtype=debug
meson test -C build-asan --suite core                      # with leak detection
ASAN_OPTIONS=detect_leaks=0 meson test -C build-asan --suite ui

# Valgrind (applied to the test binaries, not the runner script)
JR_TEST_WRAPPER='valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=1' \
  meson test -C build --suite core
```

Every test writes its journals and keys into a private temporary folder
that is deleted when the test ends, even if it crashes
(`tests/run-isolated.sh`). UI tests never open windows on your desktop. `tests/run-headless.sh`
starts a private headless GNOME Shell (a real Wayland compositor) on its
own D-Bus bus with its own config dirs, runs the test there, and shuts it
down. Without gnome-shell it falls back to GTK's Broadway backend, and
with neither it skips. `JR_UI_VISIBLE=1` runs on the real display.

The UI suite runs with leak detection off under ASan because GTK,
fontconfig and Pango keep process-wide caches that LeakSanitizer reports
at exit. Leaks in this app's UI code are caught by `/ui/no-growth`
instead, which repeats every screen and a lock cycle and checks that the
heap stops growing. These tests found and fixed real bugs: the dropdown
was kept alive by its focused entry (a leak), a text block's focus
handler could run after the block was freed when a screen closed while
typing (use-after-free), and loaded entries could stay one line tall. Set `JR_SHOTS_DIR=/some/dir` to
have `/ui/flow` save a PNG of each screen.

Unit tests cover day boundaries and date parsing, the active-time
counter (idle pause, 15 s flush, midnight split), calendar shading, the
PIN lockout timer, SQLite storage, encryption and key wrapping, the
recovery key, word counting, process hardening, and the journal store
(PIN, passphrase, lock jobs, cost upgrade, lockout
persisted across restarts). The manual checklist is in
[TESTING.md](TESTING.md).

## Quality checks

| Check | Result (28 Oct 2026) | How |
| --- | --- | --- |
| Unit, UI and integration tests | 32 suites, all pass | `meson test -C build` |
| Network jail: every non-local socket family, random and 64-bit domains, a real TCP listener that must receive nothing, io_uring, x32, threads, child programs | all refused | `tests/test_netjail.c` |
| AppArmor profile, probed from outside without the jail | TCP, UDP, IPv6, DNS via resolved and the link portal refused | `tools/check-apparmor.sh` |
| Crash safety: writer killed with SIGKILL at random moments, 50 times per run | every acknowledged save survives and decrypts; file passes `PRAGMA integrity_check`; a PIN change cut off half-way always leaves the old or new PIN working | `tests/test_crash.c` |
| Static analysis (GCC `-fanalyzer`) | 0 findings in app and tests | `tools/analyze.sh` |
| AddressSanitizer + UndefinedBehaviorSanitizer | clean | see Tests |
| Valgrind memcheck | 0 errors, 0 leaks | see Tests |
| Line coverage | 89.2% overall (Journal, Notes and the start screen) | `tools/coverage.sh` |
| Compiler warnings | `-Wall -Wextra -Werror`, none | every build |
| Fuzzing (libFuzzer + ASan + UBSan on GitHub, 5 targets) | 1 bug found and fixed (a damaged file could hand the calendar a missing day) | [`fuzz/README.md`](fuzz/README.md) |

What a crash can and cannot lose: entries are saved about a second
after you stop typing, and on blur, lock and quit. SQLite runs with
`synchronous=FULL` and every change is a transaction, so a crash, kill
or power cut loses at most the last second of typing (and up to 15 s of
the minutes counter), never anything already saved, and never leaves a
half-written file. Lock changes are all-or-nothing. There is no backup
feature in V1: the file is the only copy, so back up
`~/.local/share/journal/journal.db` (it is encrypted, so a copy is as
safe as the original).

### SonarQube

`sonar-project.properties` points SonarQube at the C sources, meson's
`compile_commands.json` and the line coverage that `tools/coverage.sh`
writes to `build-cov/coverage.xml`. SonarQube Cloud scans every push
from GitHub Actions, C code included. With a local SonarQube server
running:

```bash
export SONAR_HOST_URL=http://localhost:9000
export SONAR_TOKEN=...            # an analysis token from your server
tools/coverage.sh && tools/sonar-scan.sh
```

Result on SonarQube Community Build 26.8 (11 Oct 2026): quality gate
passed, 0 bugs, 0 vulnerabilities, 0 security hotspots, 0 code smells,
0% duplication, A ratings. Its first scan found 3 real issues in the
stylesheet (two low-contrast text colours, one duplicated selector),
now fixed. Two web-only CSS rules are switched off for `data/` because
GTK CSS uses widget names a browser does not know (see the comment in
`sonar-project.properties`).

Result on SonarQube Cloud (11 Oct 2026, C code included): quality gate
passed, 0 bugs, 0 vulnerabilities, 0 security hotspots, 0 code smells,
0% duplication, 89.3% line coverage. Its first scan found one real
issue: two lock states compared with `memcmp`, padding bytes included;
they are now compared field by field.

Note that Community Build has no C/C++ analyzer, so it checks the
stylesheet, XML, desktop file and scripts, plus a secrets scan of every
file, but not the C code. The C code is covered by the compiler
(`-Werror`), GCC's analyzer, the sanitizers, Valgrind and the tests
above. For Sonar rules on the C code too, use a SonarQube edition with
C/C++ support, or SonarQube for IDE in VS Code with
`build/compile_commands.json`.

## Layout

```
src/launcher/ `journal`: the start screen; runs one program at a time
src/diary/    `journal-diary`: the Journal's main(), in the network jail
src/notes/    `journal-notes`: Notes (study notes, AI enhanced copy)
  core/       plain C, no UI, nothing of the Journal: noteline (the six
              line kinds), notesdb (SQLite), notesjail (Landlock),
              noteexport (Markdown export, topic reference), markdown
              (reading the copy's Markdown), svgsafe (diagram safety)
  ai/         textify (page to text), netguard (which links and
              addresses), aiproto (sign-in, Gemini request and answer,
              Notes <-> helper messages), fetch (Sources), client (Notes'
              side), helper.c (`journal-ai`, the locked-down helper)
  ui/         editor (formats, Markdown mode), md_view (the formatted
              copy), notes_window
src/core/   plain C, no UI, fully unit tested
  jdate       local days, parsing, formatting
  active_time seconds actually spent writing
  shade       calendar levels, minute formatting
  lockout     5 tries then 30 s
  db          SQLite (only SQL here)
  vault       keys, Argon2id wrapping, entry encryption, recovery key
  journal     the rules: locked/unlocked, PIN/passphrase, lock jobs, lockout
  harden      non-dumpable process, no core dumps
  netjail     the Journal's no-network seccomp filter
  programs    launcher hand-over: exit codes and what follows them
  text        word count
src/ui/     GTK4
  window      screens, ticker, shortcuts, locking
  lock_view   01 lock screen
  day_view    02 today / past days
  day_popover 03 day dropdown
  activity_view, bar_chart   04 activity and time
  security_view, secret_dialog  05 lock and security
  sleep_watch logind PrepareForSleep
data/       stylesheet, icons, .desktop files, AppArmor profile
tests/      unit tests and the UI test
fuzz/       fuzz targets, their seeds, and every input that found a bug
```

## Decisions worth knowing

- **Header "Lock" button** opens Lock & security, as the wireframe flow
  says. Locking right away is `Ctrl+L`, or **Lock now** on that screen.
- **When a block is stamped:** a new block shows the live time, dimmed,
  until its first keystroke, which stamps it. A block whose text is
  deleted again is removed from the file.
- **Past days** keep their original stamps when edited. `Ctrl+Enter`
  on a past day jumps to today, because new stamps only belong on today.
- **Midnight:** blocks carry their own day, so nothing is misfiled. Once
  you pause for a minute after midnight, the page moves to the new day.
- **GtkSwitch** is replaced by a small drawn toggle, because GTK 4.22's
  switch logs a layout warning on every frame.
- **Loaded text height:** GTK 4.22's GtkTextView drops the resize it
  asks for when it wraps text during allocation, so the day view checks
  each block's height against its laid-out text for the first frames
  after opening and corrects it, both ways: too short (one line for a
  paragraph) and too tall (one letter per line, seen when the launcher
  opens the window with a focus token).

## License

MIT. See [LICENSE](LICENSE).
