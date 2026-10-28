#!/bin/sh
# Starts the real Notes program, alone and through the real start screen,
# and checks it stays up and closes cleanly when asked (logout, SIGTERM).
#   $1 journal-notes   $2 journal (the launcher)
set -u
notes="$1"; launcher="$2"
fail=0

JOURNAL_LAUNCHER=1 "$notes" >/dev/null 2>&1 &
pid=$!
sleep 1.5
kill -0 "$pid" 2>/dev/null || { echo "Notes did not stay open"; fail=1; }
kill -TERM "$pid"; wait "$pid"; status=$?
[ $status -eq 0 ] || { echo "Notes exited with $status after SIGTERM"; fail=1; }

# Through the start screen: Notes runs as its child; logout closes both.
JOURNAL_PROGRAM_DIR="$(dirname "$notes")" "$launcher" --open=notes >/dev/null 2>&1 &
pid=$!
sleep 2
child=$(pgrep -P "$pid" -x journal-notes)
[ -n "$child" ] || { echo "the start screen did not start Notes"; fail=1; }
kill -TERM "$pid"; wait "$pid"; status=$?
[ $status -eq 0 ] || { echo "start screen exited with $status after SIGTERM"; fail=1; }
# The launcher waits for its child, so that exact process is gone too.
[ -n "$child" ] && kill -0 "$child" 2>/dev/null && { echo "Notes ($child) was left running"; fail=1; }

[ $fail -eq 0 ] && echo "notes: opens, runs under the start screen, closes cleanly"
exit $fail
