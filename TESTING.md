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
| PIN lockout: 5 tries then 30 s, ignores tries while waiting, survives restart | `core/lockout`, `/journal/lockout-persists` |
| Storage: entries ordered by stamp, minutes add up, file 0600 in a 0700 folder | `core/db` |
| Encryption: bodies unreadable in the file, tamper detected, wrong key fails | `core/vault`, `/journal/no-pin` |
| PIN set / change / turn off, recovery key unlocks, new key replaces old | `core/journal` |
| Locked journal reads and writes nothing | `/journal/enable-pin` |
| Opens on today with the cursor ready; `Ctrl+Enter` stamps a new block; autosave after ~1 s | `/ui/flow` |
| Day dropdown opens, frees its content on close, gives the cursor back | `/ui/flow` |
| Past days are read-only | `/ui/flow` |
| Locking clears every text view and the key; unlock brings the text back | `/ui/flow` |
| Loaded text is not undoable; long text is laid out at full height | `/ui/flow` |
| Quit while writing saves; window and key are destroyed | `/ui/save-on-quit-and-sleep` |
| Sleep while writing saves before locking | `/ui/save-on-quit-and-sleep` |
| Very long entry (1 MiB) saves fast; typing stays responsive | `/ui/long-entry`, `/journal/large-entry` |
| No memory growth across screens and lock cycles | `/ui/no-growth` |
| Passphrase: rules (12+ chars, NFC), unlock, wrong one refused, switch to/from PIN | `core/vault`, `/journal/passphrase*`, `/ui/passphrase` |
| Every lock change needs the current secret; wrong ones count; a lock mid-change applies nothing | `/journal/lock-jobs`, `/journal/change-disable`, `/ui/passphrase` |
| Old weaker keys re-saved at the current Argon2id cost on unlock | `/journal/upgrade-cost` |
| Recovery key cannot be copied or selected | `/ui/flow` |
| Writing area marked private for input methods | `/ui/flow` |
| Real binary: non-dumpable, core dumps off, clean exit on SIGTERM | `hardened` |
| Crash during saving (SIGKILL at random moments): no lost acknowledged save, file intact | `crash` (`/crash/entries-survive-kill`) |
| Crash during a PIN change: old or new PIN still opens it, recovery key still works | `crash` (`/crash/lock-change-survives-kill`) |
| Lock screen keyboard: PIN on 6th digit, Backspace/Esc, recovery key, 5 wrong PINs then wait | `/ui/pin-lock-screen` |
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

*After each of the first four: "Wrong PIN. N tries left…". After the
fifth: "Too many wrong tries. Try again in 30 s.", counting down; digits
do nothing while it counts. At 0: "You can try again now.", and the
right PIN unlocks.*

### 4. Restart during lockout
1. Trigger the lockout (check 3), wait ~10 s, then quit with `Ctrl+Q`.
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
2. Lock, choose "Forgot PIN? Use recovery key", and type the key in
   lower case without dashes.

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
