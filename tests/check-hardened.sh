#!/bin/sh
# Starts the real app (on whatever display it is given, normally the
# private headless one) with a scratch journal and checks, from outside,
# that its memory is protected:
#  - not dumpable: /proc/<pid>/mem and environ belong to root, so other
#    programs of the same user cannot read its memory or attach a debugger;
#  - core dumps off: a crash cannot write entry text or keys to disk.
# Then asks it to quit (SIGTERM) and expects a clean exit.
set -u
app="$1"
JOURNAL_DB="${TMPDIR:-/tmp}/hardened-check.db" "$app" >/dev/null 2>&1 &
pid=$!
sleep 2
fail=0
if ! kill -0 "$pid" 2>/dev/null; then echo "app did not start"; exit 1; fi
owner=$(stat -c %U "/proc/$pid/mem")
[ "$owner" = root ] || { echo "process is dumpable (/proc/$pid/mem owned by $owner)"; fail=1; }
# A program running as the same user must not be able to read it.
if cat "/proc/$pid/environ" >/dev/null 2>&1; then
  echo "another process of the same user can read its /proc/$pid/environ"; fail=1
fi
core=$(awk '/Max core file size/ {print $5, $6}' "/proc/$pid/limits")
[ "$core" = "0 0" ] || { echo "core dumps allowed: $core"; fail=1; }
kill -TERM "$pid"
wait "$pid"
status=$?
[ $status -eq 0 ] || { echo "app exited with $status after SIGTERM"; fail=1; }
[ $fail -eq 0 ] && echo "hardened: not dumpable, no core dumps, clean exit"
exit $fail
