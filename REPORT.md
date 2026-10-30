# Journal and Notes: test and safety report

30 October 2026, commit SHA_HERE. What protects your writing, how it was
tested, what testing found, and what only you can check.

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
  keyring, for Notes and anything it starts.
- **The AI helper** (`journal-ai`) does all network work, one run per
  Enhance:
  - It locks itself first: it can't read or write any of your files,
    can reach only ports 443 and 80, and refuses to run without the lock.
  - It gets the note, its links and a one-hour token. Your Google
    refresh token stays in Notes.
  - Links and redirects into this laptop or your home network are refused.
- What the AI writes is only shown, never run. Its diagrams pass an
  allow-list cleaner first, and only http(s) links open.

**What no app can stop:** a program already running as you (malware, a
stolen login) can wait for you to unlock. Full-disk encryption and a
strong login password cover that.

## How it was tested

| Check | Result |
| --- | --- |
| Unit, UI and integration tests (LeetCode style: tables of cases plus random "hidden" cases) | 40 suites, all pass |
| AddressSanitizer + UBSan (core, sandbox, UI) | clean |
| Valgrind (core, fuzz replays) | 0 errors, 0 leaks |
| GCC static analyzer | 0 findings |
| Fuzzing, 10 targets (5 Journal, 5 Notes), libFuzzer + ASan + UBSan | 8 bugs found, all fixed; every input kept and replayed on each test run |
| SonarQube Cloud quality gate | SONAR_HERE |
| GitHub CI (build, tests, sanitizers, 10 fuzz jobs) | CI_HERE |
| Crash safety: writer killed at random 50 times per run | every saved entry survives |
| Network jail and AppArmor, probed from outside | everything refused |

## What fuzzing found (step 6)

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

Each one now has its input in `fuzz/corpus/` and a named unit-test
case. The test fails on the old code and passes on the new one.
Earlier (Journal): a damaged file could hand the calendar a missing day.

## What only you can check

These need your laptop, your accounts or sudo. The steps are in
[TESTING.md](TESTING.md), "Manual checks":

- **Install** (not done yet; the laptop still runs the 11 Oct build):
  `sudo meson install -C build --no-rebuild && sudo tools/install-apparmor.sh && tools/check-apparmor.sh`.
  Until then, the AppArmor layer isn't active.
- Lid close while writing (#1), and the PIN on a new keyring (#11).
- A real enhanced copy with and without sources (#13).
- The study timer's desktop notification, and that time asleep isn't
  counted (#15).
