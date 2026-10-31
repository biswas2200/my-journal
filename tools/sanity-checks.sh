#!/bin/sh
# The checks of tools/sanity.sh, run on the private headless display
# (tests/run-headless.sh):
#
#   sanity-checks.sh NEW_PREFIX OLD_PREFIX OLD_NAME WORK
#
# Every program runs on a throwaway profile under WORK (its own HOME and
# XDG folders), so nothing of yours is read or written. Prints one line
# per check, PASS or FAIL, and exits 1 if any failed.
set -u
new=$1 old=$2 old_name=$3 work=$4
here=$(cd "$(dirname "$0")" && pwd)
failed=0

notes_log=""

result () { # result NAME OK [DETAIL]
  if [ "$2" = 0 ]; then
    echo "PASS  $1${3:+  ($3)}"
  else
    echo "FAIL  $1${3:+  ($3)}"
    failed=1
    # What Notes said, when it was a Notes check.
    case "$1" in *otes*) [ -s "$notes_log" ] && sed 's/^/        /' "$notes_log" | tail -5 ;; esac
  fi
}

# Runs a program on profile $1 (a folder); the rest is the command.
in_profile () {
  p=$1; shift
  mkdir -p "$p/data" "$p/config" "$p/cache" "$p/state"
  HOME="$p" XDG_DATA_HOME="$p/data" XDG_CONFIG_HOME="$p/config" XDG_CACHE_HOME="$p/cache" \
    XDG_STATE_HOME="$p/state" "$@"
}

now_ms () { date +%s%3N; }

# Starts Notes on profile $1, waits until it has its notes open (the file
# exists and it is still running), lets it settle, then asks it to quit.
# Sets: started (0/1), open_ms, exit_status.
run_notes () {
  p=$1 prefix=$2
  mkdir -p "$p"
  db="$p/data/journal-notes/notes.db"
  t0=$(now_ms)
  mkdir -p "$p/data" "$p/config" "$p/cache" "$p/state"
  # Not through in_profile: a backgrounded shell function is a subshell,
  # and $! would be that subshell, not Notes.
  env HOME="$p" XDG_DATA_HOME="$p/data" XDG_CONFIG_HOME="$p/config" XDG_CACHE_HOME="$p/cache" \
    XDG_STATE_HOME="$p/state" "$prefix/libexec/journal-notes" >"$p/notes.log" 2>&1 &
  pid=$!
  started=0
  for _ in $(seq 1 100); do
    if [ -s "$db" ] && kill -0 "$pid" 2>/dev/null; then started=1; break; fi
    sleep 0.05
  done
  open_ms=$(( $(now_ms) - t0 ))
  notes_log="$p/notes.log"
  sleep 1.5 # it shows its window and makes its backup
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid"
    wait "$pid"; exit_status=$?
  else
    started=0; wait "$pid"; exit_status=$?
  fi
}

# Every row of every table, sorted, as text: equal dumps, equal notes.
dump () {
  python3 -I - "$1" <<'EOF'
import sqlite3, sys
db = sqlite3.connect("file:" + sys.argv[1] + "?mode=ro", uri=True)
for (t,) in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"):
    for row in sorted(db.execute("SELECT * FROM " + t).fetchall(), key=repr):
        print(t, repr(row))
EOF
}

sound () {
  python3 -I -c 'import sqlite3,sys; print(sqlite3.connect(sys.argv[1]).execute("PRAGMA integrity_check").fetchone()[0])' "$1"
}

# Notes in every shape: names in several scripts, all line kinds, sources,
# an enhanced copy, study time, a setting.
seed () {
  python3 -I - "$1" <<'EOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
subjects = ["DSA", "Operating Systems", "गणित", "Café ☕"]
for i, s in enumerate(subjects, 1):
    db.execute("INSERT INTO subjects (name, key, created) VALUES (?, ?, ?)", (s, s.casefold(), 1793500000 + i))
    for j in range(1, 6):
        db.execute("INSERT INTO topics (subject, name, key, created) VALUES (?, ?, ?, ?)",
                   (i, f"Topic {j} of {s}", f"topic {j} of {s}".casefold(), 1793500000 + j))
        t = db.execute("SELECT last_insert_rowid()").fetchone()[0]
        for k in range(3):
            body = f"# Idea {k}\n## Detail\n- point\n  - sub point\n→ so this\n\\# not a heading\nplain é ✓"
            db.execute("INSERT INTO sessions (topic, started, ended, body, sources) VALUES (?, ?, ?, ?, ?)",
                       (t, 1793500000 + k * 3600, 1793500000 + k * 3600 + 1800, body, "https://example.org/x"))
        db.execute("INSERT INTO study_log (topic, at, seconds) VALUES (?, ?, ?)", (t, 1793500100, 1500))
db.execute("INSERT INTO enhanced (session, markdown, model, created) VALUES (1, '## Idea\n- more [1]', 'test', 1793509999)")
db.execute("INSERT INTO settings (key, value) VALUES ('ai_consent', 'yes')")
db.commit()
EOF
}

for build in new old; do
  prefix=$new; label="this tree"
  [ "$build" = old ] && { prefix=$old; label=$old_name; }
  want=$(meson introspect "$work/$build-build" --projectinfo | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["version"])')
  got=$("$prefix/bin/journal" --version 2>/dev/null)
  if [ "$build" = old ] && [ -z "$got" ]; then
    echo "SKIP  $label: journal --version (older than the option)"
  else
    [ "$got" = "journal $want" ]; result "$label: journal --version says $want" $? "$got"
  fi

  # The Journal: hardened, then a clean quit.
  p="$work/profile-journal-$build"
  mkdir -p "$p"
  hardened=$(in_profile "$p" env TMPDIR="$p" sh "$here/../tests/check-hardened.sh" "$prefix/libexec/journal-diary" 2>&1)
  result "$label: Journal starts hardened and closes cleanly" $? "$(echo "$hardened" | tail -1)"

  # Notes on an empty profile.
  p="$work/profile-empty-$build"
  run_notes "$p" "$prefix"
  ok=$(( started == 1 && exit_status == 0 ? 0 : 1 ))
  result "$label: Notes opens on an empty profile and closes cleanly" $ok "open in ${open_ms} ms, exit $exit_status"
done

# Between versions: notes made by the old build.
p="$work/profile-upgrade"
run_notes "$p" "$old" # the old build makes the file
db="$p/data/journal-notes/notes.db"
seed "$db"
dump "$db" >"$work/before.txt"
rows=$(wc -l <"$work/before.txt")

run_notes "$p" "$new"
dump "$db" >"$work/after-new.txt"
ok=$(( started == 1 && exit_status == 0 ? 0 : 1 ))
cmp -s "$work/before.txt" "$work/after-new.txt" || ok=1
result "notes from $old_name open in this tree with all $rows rows intact" $ok "$(sound "$db")"
backup=$(ls "$p/data/journal-notes/backups/" 2>/dev/null | grep -c '^notes-....-..-..\.db$')
[ "$backup" -ge 1 ]; result "this tree made a backup of them" $? "$backup backup(s)"
for b in "$p"/data/journal-notes/backups/notes-*.db; do
  [ -f "$b" ] || continue
  dump "$b" >"$work/backup.txt"
  cmp -s "$work/before.txt" "$work/backup.txt"; result "the backup holds every row" $? "$(sound "$b")"
done

run_notes "$p" "$old"
dump "$db" >"$work/after-old.txt"
ok=$(( started == 1 && exit_status == 0 ? 0 : 1 ))
cmp -s "$work/before.txt" "$work/after-old.txt" || ok=1
result "and open in $old_name again, intact (going back loses nothing)" $ok "$(sound "$db")"

[ $failed -eq 0 ] && echo "sanity: all checks passed" || echo "sanity: some checks FAILED"
exit $failed
