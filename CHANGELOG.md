# Changes

## 0.1.5 (1 November 2026)

Back up Notes to your own Google Drive.

### Notes: back up to your own Google Drive

- **Sign in with Google** (☁ Drive in the header): Google's page opens
  in your browser; Notes asks only for the files it makes itself
  (`drive.file`) and keeps the sign-in in your login keyring.
- **Back up this topic** / **Back up everything**, and automatically
  when Notes closes: My Drive / Notes / <subject> / <topic>, one Google
  Doc per topic with its headings, lists and links, updated in place
  (Google keeps the earlier versions).
- Each topic shows "Backed up 3 Nov 14:05" or "Changed since the last
  backup" (amber); topics waiting for a backup are marked in the list.
- A backup counts only once the Doc reads back with the same text;
  anyone besides you being able to open a Doc is pointed out.
- A backup cut short (Notes closing after its minute) keeps every topic
  it finished, and nothing is ever made twice: before making a folder
  or Doc, Notes looks for one it made earlier.
- Every line of a note stays its own line in the Doc, and text such as
  `<tags>` is kept as written (Markdown readers take it for web code).
  Checked live against Google Docs before this release.
- The sign-in page opens in your default browser through the desktop's
  portal, so the browser never runs inside Notes' file lock. Notes
  waits 5 minutes and the ☁ Drive menu says so; clicking again starts
  over; with no browser to open, it says so at once.
- The Drive work runs in a locked-down helper (`journal-drive`) that
  sees only a one-hour token. One-time setup: see README, "Google Drive
  backup".

### Fixed

- **Export .md**: a plain or arrow line right after a bullet was read as
  part of the bullet by Markdown readers, "Time studied" joined the
  subject line, and `<tags>` in a note vanished. Every line now stays
  its own line and such text is escaped.
- Closing Notes while an **Enhance** was still out skipped the day's
  backup at close; it is now made as the window closes.

### Tests

- A fake Google (Drive and its sign-in service) for the helper, the
  client and the window; a sign-in that is stopped, replaced or cut off
  at any moment, and random junk sent to its return address; a fuzz
  target for everything the backup reads from Google and from its
  helper (`fuzz/fuzz_drive.c`).

## 0.1.4 (1 November 2026)

Notes stays quick with years of notes.

### Faster with many notes

- Measured with 10 000 topics and 100 000 sessions. The sidebar now
  shows one subject's topics at a time (▸ / ▾), and the autosave no
  longer rebuilds the list. Opening a topic went from 454 ms to 5 ms,
  and typing no longer freezes for about 280 ms at each autosave.

### Tests

- A load test (`meson test --suite load`) with years of notes, timed
  against fixed limits; CI runs it too.

## 0.1.3 (1 November 2026)

Crash, memory and sanity tests for Notes, and a backup fix they found.

### Fixed

- A backup cut off by a crash no longer leaves SQLite's side file
  behind, where the next backup could have picked it up.

### Tests

- Notes crash test (SIGKILL while saving and backing up), a memory
  no-growth test for the Notes window, and `tools/sanity.sh`: this tree
  and the last release side by side, including old notes in the new
  build and back. CI runs it on every push.

## 0.1.2 (1 November 2026)

A fix for the Notes sidebar.

### Fixed

- The sidebar now highlights what you clicked (a style rule hid it).

## 0.1.1 (1 November 2026)

Notes keeps your text through any failure.

### Notes: your text survives anything

- A save that fails (a full disk, a broken file) is never silent: the
  note goes at once to `~/Documents/Notes/Unsaved/`, the status line
  turns red and says where, and Notes keeps trying. If even that copy
  cannot be written, Notes will not close until the text is saved.
- Daily backups of `notes.db` (the last 14 days) in
  `~/.local/share/journal-notes/backups/`, made when Notes opens and
  when it closes, each checked before it replaces anything.
- A damaged notes file is never opened or backed up over a good copy;
  Notes names the newest backup to restore.

## 0.1.0 (30 October 2026)

The first release: a private journal and a separate Notes app for
study, on Linux (GTK 4, C11). Install from source (see README, "Build
and install"); `journal --version` shows the version.

### Start screen

- `journal` opens a start screen with two cards, Journal and Notes.
  They are two separate programs, and the start screen reads neither
  one's data. `journal --open=journal` or `--open=notes` skips it.

### Journal

- One page per day. It opens on today, and `Ctrl+K` jumps to any day
  ("4 Oct", "yesterday", "2026-10-04"). Past days open read-only, with
  an Edit button.
- Each entry is stamped with the time of its first keystroke;
  `Ctrl+Enter` starts a new one.
- Activity: a month calendar shaded by minutes written, totals, and the
  last 7 days. Minutes count only while you type.
- Encrypted on disk. Optional lock: a 12+ character passphrase, or a
  6-digit PIN with a master passphrase, plus a recovery key shown once.
  Five wrong PINs block the PIN.
- Locks when you leave it, close the app, or the laptop sleeps.
- No network at all: a seccomp filter inside the program, and an
  AppArmor profile from outside (installed with
  `sudo tools/install-apparmor.sh`).

### Notes

- Subjects, then topics, then time-stamped study sessions. Notes save
  as you type.
- Formats: heading, subheading, bullet, sub-bullet and arrow, from the
  toolbar, the keyboard or typed markers. A Markdown mode (`Ctrl+M`)
  shows a live preview.
- Sources box for the links you study from (optional).
- Export a whole topic as Markdown (`Ctrl+E`) to
  `~/Documents/Notes/<subject>/<topic>.md`.
- Study stopwatch and timer (`Ctrl+T`). Time is logged per topic,
  shown as "Today · in all", and included in the export. Time asleep
  is not counted.
- Notes cannot open the Journal's files (Landlock).

### AI enhanced copy (optional)

- **✦ Enhance** (`Ctrl+Shift+E`) writes a separate, read-only copy of a
  session with Gemini 3.8 Flash on Vertex AI, using your own Google
  sign-in. Your note is never changed.
- It works from your sources, with each added fact cited `[n]`, or
  without sources from well-established facts only. Anything doubtful
  is marked "(check this)". It ends with what your note does not cover
  yet. Plain words, no corporate jargon.
- Shown formatted, with diagrams drawn only after a safety check.
  **★ Topic reference** gathers every copy of a topic.
- Asks before the first use. A locked-down helper does the network
  work: it can't read your files and can't reach this computer or your
  home network. Your sign-in's refresh token never leaves Notes.

### Quality

- Unit, UI and integration tests (41 suites), AddressSanitizer, UBSan,
  Valgrind, the GCC analyzer, and 10 fuzz targets. Every check passes
  on GitHub and in SonarQube Cloud. See [REPORT.md](REPORT.md).

### Known limits

- Linux only, built from source; no packages yet.
- PDF sources can't be read yet (HTML and plain text only).
- No backup feature: back up `~/.local/share/journal/journal.db`
  yourself (it is encrypted).
