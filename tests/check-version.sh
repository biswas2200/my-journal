#!/bin/sh
# `journal --version` prints the release and exits, without a display
# and without starting anything.
#   $1 journal (the launcher)   $2 the version meson.build gives
set -u
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$1" --version 2>&1)
status=$?
if [ "$status" -ne 0 ] || [ "$out" != "journal $2" ]; then
  echo "Expected \"journal $2\" and exit 0  Got \"$out\" and exit $status"
  exit 1
fi
