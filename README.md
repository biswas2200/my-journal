# Journal

A private daily journal for one person, running as a small native app on
Linux (GTK4, C11). Write the loop down once, so it stops running in your
head. Everything stays in one encrypted file on this laptop: no network,
no accounts, no telemetry, no sync.

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
4. **Lock.** Optional 6-digit PIN or, stronger, a passphrase (12 or more
   characters). It locks when the app closes and when the laptop sleeps
   or the lid closes. After 5 wrong tries it waits 30 seconds, and a
   restart does not reset that wait.
5. **Dark, monochrome theme** that uses only system fonts.

## Build and install

```bash
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-4-dev libsqlite3-dev libsodium-dev
meson setup build
meson compile -C build
meson test -C build            # unit + UI tests
sudo meson install -C build    # binary, .desktop file, icon
```

Run it from the app grid ("Journal") or with `journal`. To uninstall:
`sudo ninja -C build uninstall`.

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

## Security model (Option B: encrypt at rest)

- A random 32-byte **data key** encrypts each entry body with libsodium
  `secretbox` (XSalsa20-Poly1305). Dates, stamp times and minutes are not
  secret and stay readable, so the Activity screen never decrypts text.
- With a lock set, the data key is never stored in the clear. It is
  stored twice, wrapped with keys derived by **Argon2id** (10 passes,
  256 MiB, random salt): once from the PIN or passphrase, once from the
  **recovery key**. A wrong secret fails authentication, so the wrapped
  key also serves as the salted Argon2id check. The PIN or passphrase
  itself is never stored, not even hashed. Keys saved with an older,
  weaker setting are re-saved with the current one the next time they
  unlock.
- **PIN or passphrase.** A 6-digit PIN has a million values; with each
  guess costing ~0.9 s and 256 MiB, someone with a copy of the file could
  still try them all in about a day on a 12-core laptop. A passphrase of
  12+ characters (a few random words) has far too many possibilities for
  that. Passphrases are Unicode-normalized, so the same words typed with
  a different keyboard layout or input method still match.
- The **recovery key** (`XXXX-XXXX-XXXX-XXXX-XXXX-XXXX`, 120 random bits)
  is shown once when the lock is set, or when you ask for a new one. It
  cannot be copied or selected, so it never reaches the clipboard: write
  it down. It unlocks in place of the PIN or passphrase. If you lose both,
  the entries cannot be read by anyone, including you.
- Changing or turning off the lock, or making a new recovery key, asks
  for the current PIN or passphrase first, and wrong ones count toward
  the 5-try lockout. The slow work runs on a background thread on copies;
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
- **No network.** The only outside contact is the local system bus, to
  hear "about to sleep" from logind. No sockets, no telemetry, no logging
  of entry text.
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

| Check | Result (10 Oct 2026) | How |
| --- | --- | --- |
| Unit, UI and integration tests | 12 suites, all pass | `meson test -C build` |
| Crash safety: writer killed with SIGKILL at random moments, 50 times per run | every acknowledged save survives and decrypts; file passes `PRAGMA integrity_check`; a PIN change cut off half-way always leaves the old or new PIN working | `tests/test_crash.c` |
| Static analysis (GCC `-fanalyzer`) | 0 findings in app and tests | `tools/analyze.sh` |
| AddressSanitizer + UndefinedBehaviorSanitizer | clean | see Tests |
| Valgrind memcheck | 0 errors, 0 leaks | see Tests |
| Line coverage | 89.4% overall, 94.8% in `src/core` | `tools/coverage.sh` |
| Compiler warnings | `-Wall -Wextra -Werror`, none | every build |

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
`compile_commands.json` and the gcov reports. With a SonarQube server
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

Note that Community Build has no C/C++ analyzer, so it checks the
stylesheet, XML, desktop file and scripts, plus a secrets scan of every
file, but not the C code. The C code is covered by the compiler
(`-Werror`), GCC's analyzer, the sanitizers, Valgrind and the tests
above. For Sonar rules on the C code too, use a SonarQube edition with
C/C++ support, or SonarQube for IDE in VS Code with
`build/compile_commands.json`.

## Layout

```
src/core/   plain C, no UI, fully unit tested
  jdate       local days, parsing, formatting
  active_time seconds actually spent writing
  shade       calendar levels, minute formatting
  lockout     5 tries then 30 s
  db          SQLite (only SQL here)
  vault       keys, Argon2id wrapping, entry encryption, recovery key
  journal     the rules: locked/unlocked, PIN/passphrase, lock jobs, lockout
  harden      non-dumpable process, no core dumps
  text        word count
src/ui/     GTK4
  window      screens, ticker, shortcuts, locking
  lock_view   01 lock screen
  day_view    02 today / past days
  day_popover 03 day dropdown
  activity_view, bar_chart   04 activity and time
  security_view, secret_dialog  05 lock and security
  sleep_watch logind PrepareForSleep
data/       stylesheet, icons, .desktop file
tests/      unit tests and the UI test
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
  after opening and corrects it.
