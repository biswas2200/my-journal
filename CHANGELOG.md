# Changes

## Unreleased

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

### Fixed

- The sidebar now highlights what you clicked (a style rule hid it).

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
