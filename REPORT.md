# Journal and Notes: test and safety report

1 November 2026, release 0.1.6. What protects your writing, how it was
tested, what testing found since 0.1.0, and what only you can check.

## What protects what

**The Journal** is private, offline writing.

- Each entry is encrypted (libsodium). With a lock set, the key that
  opens them is itself locked by your PIN or passphrase (Argon2id), a
  master passphrase and a recovery key. A PIN also needs a secret in
  your login keyring, so a copied file alone is no use for guessing it.
  Five wrong PINs block the PIN.
- It has no network at all. A seccomp filter, set before anything else
  runs, lets it open only local sockets. An AppArmor profile, enforced
  by the kernel from outside, denies the same, plus the local services
  that could go online for it.
- Leaving it (Home, switching to Notes, lid close, quit) saves, wipes
  the key from memory and ends the program, so every visit starts at
  the lock screen.
- It never writes a core dump, and other programs can't read its memory.

**Notes** is for study notes: plain files (0600), network and AI allowed.

- Landlock keeps it from opening the Journal's folder or the login
  keyring's files, for Notes and anything it starts. So links and
  Google's sign-in page open through the desktop's portal: a browser
  started by Notes would be locked too.
- Every save is one transaction; a failed save leaves a rescue copy in
  `~/Documents/Notes/Unsaved/` and keeps retrying; 14 daily backups;
  a damaged file is never opened or backed up over a good one.
- **The AI helper** (`journal-ai`) does the network work of an Enhance:
  no files, ports 443 and 80 only, never this laptop or your home
  network; it sees a one-hour token, never your sign-in.
- **The Drive helper** (`journal-drive`) does the network work of a
  backup: no files, port 443 only, a one-hour token. Notes asks Google
  only for `drive.file` (the files it made itself, never the rest of
  your Drive); the sign-in is kept in your login keyring. A backup
  counts only once the Doc reads back with the same text, and a Doc
  anyone else can open is pointed out.
- What the AI writes is only shown, never run. Its diagrams pass an
  allow-list cleaner first, and only http(s) links open.
- **⚑ Report** only opens a GitHub form in your browser, with the
  version and system filled in; nothing is sent from the app.

**What no app can stop:** a program already running as you (malware, a
stolen login) can wait for you to unlock, or ask the keyring service
for Notes' Google sign-in. Full-disk encryption, a strong login password
and 2-Step Verification on your Google account cover that.

## How it was tested

| Check | Result |
| --- | --- |
| Unit, UI and integration tests (LeetCode style: tables of cases plus random "hidden" cases) | 51 suites, all pass |
| AddressSanitizer + UBSan | clean (core, sandbox); the UI suite's exit leaks are GTK's own (none from this code) |
| Valgrind (core tests and fuzz replays) | 0 errors, 0 leaks |
| GCC static analyzer (GCC 15 here, GCC 13 on GitHub) | 0 findings |
| Fuzzing, 11 targets, libFuzzer + ASan + UBSan | every input that ever found a bug kept and replayed on each run |
| Crash safety: writer killed with SIGKILL at random moments | Journal 50 times, Notes 25 times while saving and backing up: nothing reported saved is ever lost |
| Memory growth: the Journal's screens and lock cycles, 30 tours of everything in Notes | no growth |
| Load: 10 000 topics, 100 000 sessions, 1 MB notes | open a topic 5-20 ms, longest typing freeze under 15 ms |
| Sanity: each release installed beside the one before, old notes opened in the new and back | 0.1.1 to 0.1.6: every check passes |
| Live check with Google (1 Nov 2026, test notes) | sign-in through the portal and keyring, three Docs made, updated in place, private, nothing made twice, backup on close |
| SonarQube Cloud quality gate | passed: 0 open issues, A ratings |
| GitHub CI (build, tests, sanitizers, sanity, 11 fuzz jobs) | all 14 jobs pass |

## What testing found since 0.1.0

| Found by | What | Could it hurt you? | Fixed in |
| --- | --- | --- | --- |
| The new highlight test | A style rule hid the sidebar's highlight | No; you could not see what was selected | 0.1.2 |
| The crash test | A backup cut off by a crash left SQLite's side file, which the next backup could pick up | A stale backup copy | 0.1.3 |
| The load test | With years of notes the sidebar took 454 ms to rebuild, and typing froze 280 ms at each autosave | Typing stutter | 0.1.4 |
| A review of the Drive backup | A backup cut short at closing lost track of the Docs it had made (the next one made them again), and its helper kept running | Duplicate Docs | before 0.1.5 |
| A review of the sign-in | The browser for Google's page was started inside Notes' lock; a silent connection froze Notes 5 s; a second sign-in left the first running (a false error 5 minutes later); closing mid-sign-in used freed memory; no browser meant a 5-minute wait | A locked-up browser, a freeze, a possible crash at closing | before 0.1.5 |
| The same review | An Enhance (or a sign-in) still out kept the window alive past the end, so the closing backup could be skipped | One day's closing backup missing | 0.1.5 |
| The live check with Google | A line right after a bullet was joined onto it in the Doc (and in Export .md); `<tags>` in a note was dropped; Google's escaped Markdown failed the read-back | Lines merged, a word lost in the copy (your notes were never touched) | 0.1.5 |
| The Report menu | All three programs asked GTK for its light style (3 is LIGHT, 2 is DARK) | Light menus and dialogs, nearly invisible window buttons | 0.1.6 |
| GitHub's older compiler | Two analyzer complaints about correct code | No | 0.1.6 |

Each has a test that fails on the old code and passes on the new one;
Google's real answers from the live check are kept in `tests/data`.

### Earlier: what fuzzing found (0.1.0)

New fuzz targets cover everything in Notes that reads outside input:

- web pages;
- Gemini's answers and the sign-in files;
- the messages between Notes and its helper;
- the AI's Markdown and diagrams;
- stored note lines.

Each target also checks what the code promises: valid text, safe
diagrams, messages that read back unchanged, and so on. These were
found and fixed:

| Where | What | Could it hurt you? |
| --- | --- | --- |
| Web page to text | Read one byte past the end of every downloaded page | In the helper only; a crash at worst |
| Diagram cleaner | Only the first `url(...)` in a value was checked, so a second one could point at another site | Little: librsvg, which draws it in Notes, doesn't fetch outside references; but the cleaner's own rule was broken |
| Diagram cleaner | A second `<svg>` after the first escaped the size limit; a repeated attribute was written twice | A huge drawing could slow the window; the diagram failed to show |
| Markdown reader | A line ending in a broken character was read past its end | A crash showing a damaged copy |
| Messages | json-glib aborts the whole program on some broken JSON (`null{...}`) | A bad answer could close Notes |
| Markdown reader | A long line of unclosed marks took seconds to read | Notes froze for a few seconds |
| Messages, on the older json-glib that Ubuntu 24.04 ships | Leaked memory on odd JSON, passed broken text on, read a huge token lifetime as -1 | Small leaks; garbled text; a clock sum that could overflow |
| Sign-in file | The "not supported" error repeated the file's `type`, which could hold a secret | Only with a strange file; but no error may show a secret |
| Markdown reader | Math made only of braces (`$}}$`) came out as an empty piece | No; it just broke a rule |

Each one now has its input in `fuzz/corpus/` and a named unit-test
case. The test fails on the old code and passes on the new one.
Earlier (Journal): a damaged file could hand the calendar a missing day.

After that, a random test on GitHub found that a link with no text,
`[](https://...)`, vanished along with its address. It now shows the
address. Locally, the random tests then ran with 500 seeds for
Markdown and 100 for each other Notes test, with no failure.

The older-json-glib row and the sign-in row only showed up on GitHub,
which runs an older Ubuntu than this laptop. They were reproduced and checked in an Ubuntu
24.04 container. On the way, CI's older GCC analyzer and Sonar also
flagged three small code issues, all fixed. None of them could hurt
you.

## What only you can check

These need your laptop, your accounts or sudo. The steps are in
[TESTING.md](TESTING.md), "Manual checks":

- **Install** (not done yet; the laptop still runs the 11 Oct build):
  `sudo meson install -C build --no-rebuild && sudo tools/install-apparmor.sh && tools/check-apparmor.sh`.
  Until then, the AppArmor layer isn't active and Notes has no Drive
  backup or Report button.
- **Google**: the app is in "Testing", so Google ends the sign-in after
  7 days (Notes then asks you to sign in again). Publishing it needs a
  homepage and a privacy policy page.
- A Doc's earlier versions (File, Version history) after a backup
  (#16, step 4), and the report forms (#17).
- Lid close while writing (#1), the PIN on a new keyring (#11), a real
  enhanced copy (#13), the study timer's notification and sleep (#15).
- When you are done with them, delete the "Live check (delete me)"
  folder in My Drive / Notes.
