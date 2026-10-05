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
| Day boundary: 00:30 belongs to the new day; local timezone | `core/jdate` (`/jdate/midnight`, `/jdate/timezone`) |
| Jump to a date: `4 Oct`, `2026-10-04`, no future days | `core/jdate` (`/jdate/parse*`) |
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
1. Set a PIN and write down the recovery key shown.
2. Lock, choose "Forgot PIN? Use recovery key", and type the key in
   lower case without dashes.

*It unlocks and opens Lock & security so a new PIN can be set. A wrong
key counts toward the 5-try lockout.*

### 8. File stays private
```bash
ls -l ~/.local/share/journal/journal.db
sqlite3 ~/.local/share/journal/journal.db 'select hex(substr(body,1,16)) from entries limit 3'
```
*Mode `-rw-------`. Bodies are random-looking bytes, never your words.*
