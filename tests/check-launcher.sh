#!/bin/sh
# Runs the start screen (`journal`) with stand-in programs in place of
# the Journal and Notes, and checks the hand-over rules of programs.h:
#   journal → notes → quit         (switch, then close)
#   notes → journal → quit         (switch the other way, via --open)
#   journal → Home                 (start screen stays until logout)
#   journal crashes                (start screen with an error)
#   logout while a program runs    (it gets SIGTERM, then all quit)
# The stand-ins log what they were started as and whether the launcher
# marked them (JOURNAL_LAUNCHER=1); the launcher must pass nothing else.
set -u
launcher="$1"
dir="${TMPDIR:-/tmp}/launcher-check"
rm -rf "$dir"; mkdir -p "$dir/bin"
log="$dir/log"
fail=0

stand_in () { # name, what it does
  cat >"$dir/bin/$1" <<SCRIPT
#!/bin/sh
echo "$1 launcher=\${JOURNAL_LAUNCHER:-no} dir=\${JOURNAL_PROGRAM_DIR:-unset}" >>"$log"
$2
SCRIPT
  chmod +x "$dir/bin/$1"
}

# run CASE ARGS...: starts the launcher; if STOP is set, sends SIGTERM
# after that many seconds. Prints the launcher's exit status.
run () {
  : >"$log"
  JOURNAL_PROGRAM_DIR="$dir/bin" "$launcher" "$@" >"$dir/out" 2>&1 &
  pid=$!
  if [ -n "${STOP:-}" ]; then sleep "$STOP"; kill -TERM "$pid"; fi
  i=0
  while kill -0 "$pid" 2>/dev/null && [ $i -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
  if kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid"; echo "hung"; return; fi
  wait "$pid"
  echo "$?"
}

expect () { # case, expected status, got status, expected log
  got_log=$(tr '\n' '|' <"$log")
  if [ "$3" != "$2" ] || [ "$got_log" != "$4" ]; then
    echo "Case \"$1\": Expected status $2 log [$4]  Got status $3 log [$got_log]"
    sed 's/^/    /' "$dir/out"
    fail=1
  fi
}

M="launcher=1 dir=unset"

stand_in journal-diary 'exit 11'
stand_in journal-notes 'exit 0'
expect "journal → notes → quit" 0 "$(run --open=journal)" "journal-diary $M|journal-notes $M|"

stand_in journal-notes 'exit 12'
stand_in journal-diary 'exit 0'
expect "notes → journal → quit" 0 "$(run --open=notes)" "journal-notes $M|journal-diary $M|"

stand_in journal-diary 'exit 10'
expect "Home shows the start screen" 0 "$(STOP=2 run --open=journal)" "journal-diary $M|"

stand_in journal-diary 'kill -SEGV $$'
expect "crash returns to the start screen" 0 "$(STOP=2 run --open=journal)" "journal-diary $M|"
grep -q "closed unexpectedly" "$dir/out" && { echo "error text went to the terminal, not the screen"; fail=1; }

stand_in journal-diary "trap 'echo got-TERM >>\"$log\"; exit 0' TERM; while :; do sleep 0.1; done"
expect "logout while the journal runs" 0 "$(STOP=1.5 run --open=journal)" "journal-diary $M|got-TERM|"

expect "bad --open" 2 "$(run --open=elsewhere)" ""

rm -rf "$dir"
[ $fail -eq 0 ] && echo "launcher: 6/6 hand-over cases passed"
exit $fail
