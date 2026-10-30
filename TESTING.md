# Testing

Every V1 feature has an automated test, a manual check, or both. Run the
automated ones with `meson test -C build` (see README for sanitizers and
Valgrind). The manual checks need a real laptop, a real lid and a real
clock, so they are done by hand before each release.

Use a scratch file for manual checks so your real journal is untouched:

```bash
JOURNAL_DB=/tmp/journal-check.db ./build/journal
```

## Automated coverage

| Feature or rule | Test |
| --- | --- |
| Day boundary: 00:30 belongs to the new day; local timezone, summer time | `core/jdate` (`/jdate/day-from-time`, `/jdate/hidden/day-from-time`) |
| Jump to a date: `4 Oct`, `2026-10-04`, 29 Feb, no future days, long input | `core/jdate` (`/jdate/jump-to-date`, `/jdate/hidden/jump-to-date`, `/jdate/huge-input`) |
| Calendar arithmetic, weekdays and date text, 0001-01-01 to 9999-12-31 | `core/jdate` (tables, plus `/jdate/hidden/every-day`) |
| Active time: counts only with a keystroke in the last 60 s, flushes every 15 s, splits at midnight, stops on lock | `core/active_time` |
| Calendar shading: none, 1-10, 11-20, 21+ minutes | `core/shade` |
| Wrong tries: 5 wrong PINs block the PIN (no timer); 5 wrong passphrases or keys wait 30 s; both survive a restart | `core/lockout`, `/journal/scenarios` |
| Storage: entries ordered by stamp, minutes add up, file 0600 in a 0700 folder (an existing file others could read is made private), a newer version's file refused untouched, callbacks may query again | `core/db` |
| Encryption: bodies unreadable in the file, tamper detected, wrong key fails | `core/vault`, `/journal/entries` |
| PIN needs a master passphrase; set / change / turn off; recovery key unlocks, new key replaces old | `/journal/scenarios` |
| Blocked PIN: master passphrase or recovery key opens it, a new different PIN is a must, no entries until then | `/journal/scenarios`, `/ui/pin-lock-screen` |
| PIN bound to the login keyring: a copy of the file does not open with it; keyring missing or unavailable handled | `core/vault` (`/vault/device-bound`), `/journal/scenarios` |
| A PIN from before master passphrases asks for one and moves onto the keyring | `/journal/scenarios`, `/ui/master-upgrade` |
| No sequence of actions locks the owner out | `/journal/hidden/never-locked-out` |
| Locked journal reads and writes nothing | `/journal/scenarios` |
| Opens on today with the cursor ready; `Ctrl+Enter` stamps a new block; autosave after ~1 s | `/ui/flow` |
| Typing past the bottom keeps the cursor in view with a short glide; moving it up scrolls up (small, default and maximized windows) | `/ui/follow-cursor` |
| Day dropdown opens, frees its content on close, gives the cursor back | `/ui/flow` |
| Past days are read-only | `/ui/flow` |
| Locking clears every text view and the key; unlock brings the text back | `/ui/flow` |
| Loaded text is not undoable; long text is laid out at full height | `/ui/flow` |
| Quit while writing saves; window and key are destroyed | `/ui/save-on-quit-and-sleep` |
| Sleep while writing saves before locking | `/ui/save-on-quit-and-sleep` |
| Very long entry (1 MiB) saves fast; typing stays responsive | `/ui/long-entry`, `/journal/huge-entry` |
| No memory growth across screens and lock cycles | `/ui/no-growth` |
| Passphrase: rules (12+ chars, NFC), unlock, wrong one refused, switch to/from PIN | `core/vault`, `/journal/scenarios`, `/ui/passphrase` |
| Every lock change needs the current secret; wrong ones count; a lock mid-change applies nothing | `/journal/scenarios`, `/journal/three-steps`, `/ui/passphrase` |
| Old weaker keys re-saved at the current Argon2id cost on unlock (PIN, master, passphrase) | `/journal/stronger-cost` |
| Recovery key cannot be copied or selected | `/ui/flow` |
| Writing area marked private for input methods | `/ui/flow` |
| Real binary: non-dumpable, core dumps off, network jail on (seccomp mode 2, no_new_privs), clean exit on SIGTERM | `hardened` |
| Network jail: only exact `AF_UNIX` sockets; every other family, garbage and 64-bit domains, `socketpair`, `io_uring`, x32 refused with `EACCES` and counted; a real TCP listener receives nothing; threads started earlier and child programs are covered | `sandbox/netjail` |
| The whole Journal UI runs inside the jail and never even tries the network (0 blocked calls over every UI test) | `ui` (main) |
| Hand-over: exit codes 0/10/11/12 mean quit/Home/Notes/Journal; any other code or signal returns to the start screen with an error; SIGTERM/SIGINT quit | `core/programs` (table, plus every code and signal) |
| Start screen: Journal → Notes → quit, Notes → Journal, Home, a crash, logout while a program runs (it gets SIGTERM first), bad `--open` | `launcher` (stand-in programs) |
| `journal --version` prints "journal 0.1.0" (the version in `meson.build`) and exits, with no display | `version` |
| Notes lines: six kinds stored as a Markdown subset; Markdown typed by hand (`*`, `+`, tabs, `->`) reads back; text that looks like a format is escaped; every line round-trips (hidden, 20 000 random lines); 100 KB line in linear time; typing shortcuts; Tab/Shift+Tab | `core/noteline` |
| Notes store: subjects and topics unique ignoring case in any script, trimmed, never empty; sessions newest first; end time follows edits only while studying (30 min); empty sessions dropped; deletes cascade; ids never reused; restart keeps everything; file 0600 in 0700, newer version refused; hidden model of 1 500 random operations; 1 MiB note, 10 000 sessions; study time per topic (adds up, since a time, none or over a day refused, missing topic refused, goes with its topic and subject, survives a restart, an older file without the study table opens and gains it) | `core/notesdb` |
| Export: session times (midnight, under a minute, clock back), file names that cannot leave the folder or hide (hidden, 5 000 random names), the whole topic as Markdown with headings moved down, "Time studied" under the subject (none, no line) | `core/noteexport` |
| Study clock, with no real clock: stopwatch counts up, pause holds, resume adds, reset, start twice is one run, the clock seen going back counts nothing; timer counts down (rounded up, 0:00 only at the end), finishes once and stops at its length, 1 s to 10 h; study seconds handed over whole and never twice, a timer's at most its length, reset forgets the rest; "1:02:03" and "3 h 10 min"; hidden 3 000 random scripts against a model that adds up the running stretches | `core/studyclock` |
| The clock row with a stand-in clock: time shown, a minute or more logged while running, pause and reset log the rest, a mode change stops and logs first, the timer ends once with its whole length logged and starts again from the top, minutes cannot change under a running timer | `ui/notes-ui` (`/notes-ui/clock-bar`) |
| Study time in the window: no clock without a topic; time goes to the topic it ran on, also when another topic is opened with the clock running; "Today … · … in all"; "Time's up" in the status line; closing Notes keeps what ran; with Notes' own styles the page fits beside the sidebar in an 800-px window, in both modes | `ui/notes-ui` (`/notes-ui/study-clock`) |
| Notes file lock: the journal and keyring are on no list and unreachable (open, list, write, `..`, symlink, hard link, rename, `/proc/self/root`, child programs); its own data, settings, cache and exports work; the system is read-only; the journal is untouched afterwards | `sandbox/notesjail` |
| Notes editor, typed key by key: every format and shortcut, lists, indent, Backspace, Enter before a heading, undo, the cursor never inside a bullet or marker, toolbar on a selection, Markdown mode both ways; Markdown mode's live preview shows the note formatted (no markup left), follows typing, and is hidden outside Markdown mode | `ui/notes-ui` (`/notes-ui/markdown-preview`) |
| Notes window: a session starts on the first keystroke and autosaves; switching saves at once; looking creates nothing; empty sessions vanish; export writes `<subject>/<topic>.md`; Home saves and closes; all inside the file lock | `ui/notes-ui` (`/notes-ui/window-flow`) |
| Page to text: tags, scripts, styles, comments, entities (bad and surrogate ones kept), blocks and lists, `<pre>`, broken or hostile HTML, invalid UTF-8; hidden random tag soup; 5 MB page in linear time | `core/textify` |
| Links and addresses: only http/https on 80/443, no credentials; every private, loopback, link-local, carrier-NAT, multicast, reserved and documentation range refused, IPv4 and IPv6 and IPv4-inside-IPv6; hidden 20 000 private addresses | `core/netguard` |
| AI exchange: Google sign-in file (only authorized_user; broken, partial, other kinds), token answers (revoked, server trouble), the request (the person's rules: sources optional, only what is sure without them, nothing invented, no corporate jargon; fenced note and sources that hostile text cannot close; a request with no sources, or none readable, says so and holds no source block; style samples, thinking kept low), Gemini answers (thoughts left out, cut off, blocked, quota, permission), Notes <-> helper messages, the stored copy; no secret in any error | `core/aiproto` |
| Fetching from a local server: pages, plain text, redirects, 404, PDF, script-only pages, a 5 MB page capped; the host check and the per-connection check each stop a request before it reaches the server | `core/fetch` |
| The helper's lock: only the allowed port connects, any other is refused, nothing listens; the journal, the notes, settings and every write refused; certificates and network settings readable | `sandbox/notesjail` (`/notesjail/helper-lock`) |
| The real helper against a fake Gemini: token and project as headers, the note, rules and sources in the request, Markdown back; a port not allowed is unreachable; a broken request or Gemini error gives one sentence and never the token | `sandbox/ai-helper` |
| Notes' AI client with fake Google services: one token, reused; the refresh token and client secret reach only the token endpoint; revoked, missing sign-in, no project, missing helper each explained | `sandbox/ai-client` |
| Enhance in the window: nothing to send without a note; nothing sent until you agree; the note, links and a style sample go; writing goes on meanwhile; the copy is saved and shown beside the unchanged note, comes back with its session, and an error keeps the old copy; a note with no links is enhanced too, and nothing claims pages were read | `ui/notes-ui` (`/notes-ui/enhance`) |
| Reading the copy's Markdown: headings, bullets and depth, numbered, arrows, code fences (a fence inside a list, unclosed, an info line that is not a language), rules; bold, italic, code, links (only web links), `[n]` citations (but `pre[0]`, `a[1]` are code, not citations), `$math$` as plain symbols, prices left alone; hidden random text keeps every letter | `core/markdown` |
| Diagram safety: scripts, handlers, links, images (remote and local files), `foreignObject`, `use`, style sheets and attributes, remote `url()`, comments, processing instructions removed; DOCTYPE, unknown entities, huge sizes, deep nesting, too many shapes, oversized input refused; cleaning twice changes nothing; hidden random fragments never yield markup that can act | `core/svgsafe` |
| The formatted copy: every kind of line shown with its look, links open, a wide diagram fits the pane, an unsafe one shows a note instead | `ui/notes-ui` (`/notes-ui/md-view`) |
| Topic reference: every enhanced copy of a topic oldest first, headings moved down, written-by lines dropped; the sidebar row appears right after an answer and exports; a long error never widens the window | `core/noteexport`, `core/notesdb`, `ui/notes-ui` (`/notes-ui/enhance`) |
| Notes opens, runs as the start screen's child, and closes cleanly on logout (the launcher waits for it) | `notes` |
| Home and Notes on every screen (lock screen too) only when the launcher started the Journal; leaving saves, locks and closes; the next visit starts locked | `/ui/leave` |
| Reopened entries are exactly as tall as their text, also after the window changes width; "New stamped entry" sits right under them | `/ui/saved-entries-fit` |
| Crash during saving (SIGKILL at random moments): no lost acknowledged save, file intact | `crash` (`/crash/entries-survive-kill`) |
| Crash during a PIN change: old or new PIN still opens it, master passphrase and recovery key still work | `crash` (`/crash/lock-change-survives-kill`) |
| Lock screen keyboard: PIN on 6th digit, Backspace/Esc, master passphrase, recovery key, 5 wrong PINs block it, closing the new-PIN dialog locks again | `/ui/pin-lock-screen` |
| Day dropdown: typed dates, refusing bad/future dates, month pages | `/ui/day-dropdown` |

### LeetCode-style tests

The core logic is tested the way LeetCode judges a solution
([`tests/cases.h`](tests/cases.h)):

- **Tables of cases.** Each test is a table of named cases (input and
  expected output) with the input's constraints written above it: the
  basic case, then the edge cases (empty, `NULL`, first and last day there
  is, leap days, year ends, one past each limit, maximum length). Every
  case runs, and each failure prints like a LeetCode verdict:

  ```
  jump-to-date: Case "29 Feb, early the next year": Input "29 Feb" (today 2029-01-15)  Expected 2028-02-29  Got invalid
  jump-to-date: 50/51 cases passed
  ```

- **Hidden tests.** Thousands of generated inputs checked against a slow
  reference that is obviously right (for dates: walking the calendar one
  day at a time, and the C library's own calendar). They use GLib's test
  random numbers, so a failure prints a seed and `--seed` replays it.

- **Huge inputs.** Inputs far beyond any real use (100,000 characters)
  must be refused or handled, never cut short and never crash.

Run one test binary verbosely to see every table's score:

```bash
build/tests/test_jdate --verbose
```

| Module | Tables | Hidden cases |
| --- | --- | --- |
| `jdate` | 10 tables, 245 cases | every day 1900-2200 (773,177 checks), 40,000 random pairs, 24,000 moments in 6 time zones, 35,190 typed dates |
| `text` | 2 tables, 29 cases (Unicode spaces, emoji, broken bytes, 12 MB under 1 s) | 20,000 random texts, 20,000 random byte strings |
| `active_time` | 29 scripted cases written like "design a class" problems (keystrokes, ticks, sleep, the clock moving back, midnight in UTC, India and a summer-time change, locking) + 2 callback cases (none, saving from inside a save) | 400 random scripts in 4 time zones, checked after every event against a reference that counts a second if any keystroke is in the minute before it: session, every day's total, at most 14 s unsaved, every save 1-15 s. Huge input: 3 days of non-stop writing (259,200 ticks) split exactly by day |
| `db` | 47 operation scripts written like "design a class" problems (settings, entries, zero bytes, empty and NULL bodies, time, days, months, transactions, restarts) + 11 ways to open a file (new, private, shared, empty, not a database, newer version, folder in the way, read-only...) + 4 callbacks that query again | 230 random scripts (200 in memory, 30 on a file with restarts) checked after every step against plain lists: every day's entries in order, every day's time, every setting, a random range of days and months. Huge: a 10 MB entry, a 1 MB setting, ten years of entries (22 MB: a month's calendar in under 50 ms, SQLite under 2 MB of memory while reading), 100,000 entries on one day |
| `shade` | 3 tables, 47 cases (every step boundary, half a minute, a whole day, the largest and smallest numbers there are, short text buffers) | 200,000 minute counts against the spec's steps, 300,000 second counts of every size against rounding by remainder, 200,000 minute texts read back to the number |
| `lockout` | 3 tables, 68 cases (30 s wait, PIN block, restarts, saved form incl. overflow, equality whatever the padding bytes hold) | 5,000 random event scripts, each checked after every event, straight and with a restart after every event (199,816 checks) |
| `vault` | 8 tables, 157 cases (PIN, passphrase and recovery-key rules; every kind of damage to a saved key; device-bound keys incl. an edited version tag; sealing and tampering; 10 MB entry) | 300 random keys and secrets with damage, 2,000 recovery keys typed messily, 2,000 random texts sealed and bit-flipped |
| `journal` | 30 scenarios, 248 checked steps, written like LeetCode's "design a class" problems (operations in, outputs expected) | 150 random action sequences with right and wrong secrets, restarts and keyring trouble: entries open exactly when allowed, a copy of the file never opens with the PIN, and the owner always gets back in |

### Fuzzing

Ten fuzz targets ([`fuzz/`](fuzz/README.md)) take input that does not
come from this program. Journal: typed dates, any text, the saved
wrong-try state, typed secrets and saved keys, and whole (possibly
damaged) journal files. Notes: fetched web pages, Gemini's answers and
the sign-in files, the messages between Notes and its AI helper, the
AI's Markdown and SVG diagrams, and stored note lines. On GitHub each runs for two minutes per push under
libFuzzer with AddressSanitizer and UBSan, keeping what it learns
between runs. Every normal test run replays the seeds and every input
that ever found a bug (`meson test -C build --suite fuzz`).

## Manual checks

Tick each one and note the date and build. Expected results are in
*italics*.

### 1. Lid close while writing
1. Set a PIN (Lock button > switch).
2. On today's page, type a sentence and close the lid **within a second**.
3. Open the lid and log in to the desktop.

*The journal shows only the lock screen, with "Locked automatically ·
laptop lid closed or went to sleep at HH:MM". After the PIN, the sentence
is there in full.*

Also try with "The laptop lid closes or it goes to sleep" unchecked:
*the journal stays unlocked after resume.*

### 2. Quit while writing
1. Type a sentence and press `Ctrl+Q` immediately.
2. Start the app again (and enter the PIN if set).

*The sentence is there.* Repeat with the window close button and with
`pkill -TERM -x journal` (logout): *same result, and the process exits
with status 0.*

### 3. Wrong PIN five times
1. Lock (`Ctrl+L`), then type five wrong PINs.

*After each of the first four: "Wrong PIN. N tries left before the PIN is
blocked." After the fifth the screen asks for the master passphrase
("the PIN is blocked"); digits do nothing and there is no "Use PIN
instead".*
2. Quit and start the app again: *still blocked, no timer.*
3. Enter the master passphrase. *A "Choose a new PIN" dialog opens over
   Lock & security; no day page is shown. Typing the old PIN twice says
   it is the PIN being replaced.*
4. Close the dialog. *The journal locks again.* Repeat 3 with a new PIN:
   *today's page opens, and the new PIN unlocks next time.*

### 4. Restart during a wait
1. With a passphrase lock (or on the master passphrase screen), type five
   wrong ones, wait ~10 s, then quit with `Ctrl+Q`.
2. Start the app again at once.

*The countdown continues from about 20 s; it does not reset to a fresh
five tries.* The recovery key is also refused until the wait ends.

### 5. Entry written across midnight
1. Before 23:59, open today and start typing; keep typing past 00:00.
2. Press `Ctrl+Enter` after midnight and type a little more.

*The first block keeps its evening stamp and stays on the old day. The
new block is stamped after 00:00 and appears on the new day's page.
Minutes before midnight count to the old day and minutes after it to the
new day (check in Activity). About a minute after you stop typing, the
page switches to the new date by itself.*

### 6. Very long entry
1. Paste about 1 MB of text with normal paragraphs into one block.
2. Keep typing at the end, then press `Ctrl+L` and unlock.

*Typing stays smooth, "Saved HH:MM" appears about a second after you
stop, and after unlocking the whole text is back at full height.*
Known limit: a single paragraph of hundreds of kilobytes with no line
breaks makes each keystroke slow (GTK re-wraps the whole paragraph).

### 7. Recovery key
1. Set a PIN or passphrase and write down the recovery key shown.
2. Lock, choose "Use recovery key", and type the key in lower case
   without dashes.

*It unlocks and opens Lock & security so a new PIN or passphrase can be
set. A wrong key counts toward the 5-try lockout. The key on screen
cannot be selected or copied.*

### 8. Passphrase
1. Lock & security > **Use a passphrase instead**, enter the current PIN,
   then a passphrase of 12+ characters twice. The dialog says "Working…"
   for about a second while the window stays responsive.
2. Lock (`Ctrl+L`), type the passphrase, press Enter.

*The lock screen asks for a passphrase, wrong ones count toward the
lockout, the right one unlocks.*

### 9. File stays private
```bash
ls -l ~/.local/share/journal/journal.db
sqlite3 ~/.local/share/journal/journal.db 'select hex(substr(body,1,16)) from entries limit 3'
```
*Mode `-rw-------`. Bodies are random-looking bytes, never your words.*

### 10. Upgrading a PIN from an older version
1. With a PIN set by the earlier version, install this one and unlock
   with the PIN.

*A "Set a master passphrase" dialog asks for the PIN once more and a
master passphrase twice; closing it locks the journal. Afterwards
Passwords and Keys (Seahorse) > Login shows a "Journal PIN key" item,
and Lock & security shows "Change master passphrase".*

### 11. The PIN needs this laptop's keyring
1. Copy `~/.local/share/journal/journal.db` to another user account (or
   another machine) and open it there with `JOURNAL_DB=...`.

*The PIN cannot be checked ("the login keyring ... no longer has its
key"); the master passphrase opens it, and a new PIN is then set up for
that keyring.*

### 12. The AppArmor lock holds on its own

After `sudo meson install -C build` and `sudo tools/install-apparmor.sh`,
run `tools/check-apparmor.sh`. Expect every probe "refused" (TCP, UDP,
IPv6, DNS through systemd-resolved, the "open a link" portal) and, with
the Journal open, `running journal-diary: journal-diary (enforce)`.

### 13. A real enhanced copy

Signed in with `gcloud auth application-default login`, open a topic,
write a few lines, put a page you studied (e.g. a Wikipedia article) in
Sources, press **✦ Enhance** and agree. Within a minute the copy appears
on the right: your points in your order, added facts cited `[1]`, a
"Gaps" list, and your note on the left exactly as you wrote it.

Then a second topic with Sources left empty: the copy has no `[n]`
marks and no "Sources:" list, adds only standard facts, and has no
corporate words (leverage, utilize, synergy, robust...).

### 14. Markdown preview

In a note, press `Ctrl+M`: the Markdown is on top, the same note
formatted below. Type `## Idea` and `- **key** point`: within a moment
the preview shows a heading and a bullet with "key" in bold, and no `#`
or `*` signs. `Ctrl+M` again: the preview goes away.

### 15. Study stopwatch and timer

Open a topic. Press `Ctrl+T`: the stopwatch counts up. After a minute,
"Today 1 min" appears next to the session's time. Open another topic
with it running, then come back: each topic shows only its own time.
Pause, choose **Timer**, set 1 minute and **Start**: it counts down, and
at 0:00 the bell rings, a desktop notification says "Time's up", the
status line says so, and the button reads **Start again**. Suspend the
laptop for a few minutes with the stopwatch running: on waking it has
not counted the sleep. **Export .md** shows "Time studied".
