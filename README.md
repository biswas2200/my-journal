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
4. **PIN lock.** Optional 6-digit PIN. It locks when the app closes and
   when the laptop sleeps or the lid closes. After 5 wrong PINs it waits
   30 seconds, and a restart does not reset that wait.
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
| `Ctrl+L` | Lock now (opens Lock & security if no PIN is set) |
| `Esc` | Close the dropdown or a PIN dialog |
| `Ctrl+Q` | Quit (saves, then wipes the key and text from memory) |

On the lock screen, type the six digits; it unlocks on the sixth.
`Backspace` removes a digit and `Esc` clears them.

## Security model (Option B: encrypt at rest)

- A random 32-byte **data key** encrypts each entry body with libsodium
  `secretbox` (XSalsa20-Poly1305). Dates, stamp times and minutes are not
  secret and stay readable, so the Activity screen never decrypts text.
- With a PIN set, the data key is never stored in the clear. It is stored
  twice, wrapped with keys derived by **Argon2id** (8 passes, 64 MiB,
  random salt): once from the PIN, once from the **recovery key**. A
  wrong PIN fails authentication, so the wrapped key also serves as the
  salted Argon2id PIN check (no separate `crypto_pwhash_str` hash is
  needed, and the PIN itself is never stored).
- The **recovery key** (`XXXX-XXXX-XXXX-XXXX-XXXX-XXXX`, 120 random bits)
  is shown once when the PIN is set, or when you ask for a new one. It
  unlocks in place of the PIN. If you lose both, the entries cannot be
  read by anyone, including you.
- Changing or turning off the PIN, or making a new recovery key, asks for
  the current PIN first.
- Without a PIN the data key is stored next to the data. That only keeps
  text out of casual view in tools like `strings`; it is not protection.
- **Locking** (`Ctrl+L`, sleep or lid close, quit) saves, destroys every
  screen and text buffer, and zeroes the key (`sodium_free`). The window
  then shows only the lock screen. Before sleep, a logind "delay"
  inhibitor makes the system wait until the lock is done.
- **Limits:** a 6-digit PIN has a million values. Argon2id makes each
  guess cost about 0.2 s and 64 MiB, which stops casual access and file
  browsing, not a determined attacker with the file and time.

## Footprint (measured 6 Oct 2026, Ubuntu, GTK 4.22, x86-64)

| What | Value |
| --- | --- |
| Binary (`build/journal`, release) | 146 KiB, 118 KiB stripped |
| Idle memory, today screen | RSS 52 MiB, **PSS 16 MiB, private 10.4 MiB** |
| Startup CPU (to first frame) | ~0.27 s |
| Idle CPU (cursor done blinking) | ~1 ms per second |
| Memory growth, 25 tours of every screen + lock/unlock | ~17 KiB, then flat |
| PIN check | ~0.17 s, 64 MiB held only during the check |

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
recovery key, word counting, and the journal store (PIN, lockout
persisted across restarts). The manual checklist is in
[TESTING.md](TESTING.md).

## Layout

```
src/core/   plain C, no UI, fully unit tested
  jdate       local days, parsing, formatting
  active_time seconds actually spent writing
  shade       calendar levels, minute formatting
  lockout     5 tries then 30 s
  db          SQLite (only SQL here)
  vault       keys, Argon2id wrapping, entry encryption, recovery key
  journal     the rules: locked/unlocked, PIN, lockout, encrypted entries
  text        word count
src/ui/     GTK4
  window      screens, ticker, shortcuts, locking
  lock_view   01 lock screen
  day_view    02 today / past days
  day_popover 03 day dropdown
  activity_view, bar_chart   04 activity and time
  security_view, pin_dialog  05 lock and security
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
