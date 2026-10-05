#!/bin/sh
# Runs a GTK test without showing anything on the desktop.
#
# 1. Preferred: a private headless GNOME Shell (a real Wayland compositor,
#    so focus, popovers and memory behave as on the desktop), with its own
#    D-Bus session bus and its own config/data/cache dirs.
# 2. Fallback: GTK's Broadway backend (functional checks only).
# 3. Neither available: exit 77, which meson reports as "skipped".
#
# Test files go to a private TMPDIR that is deleted afterwards, even if
# the test crashes. JR_UI_VISIBLE=1 runs on the real display instead.
set -u
here=$(cd "$(dirname "$0")" && pwd)

if [ "${JR_UI_VISIBLE:-0}" = 1 ]; then
  exec "$@"
fi

# Inner step, already inside the private bus: start the shell, run the test.
if [ "${JR_IN_HEADLESS_SHELL:-0}" = 1 ]; then
  # The test's strict settings (fatal warnings, sanitizers) are for the
  # test, not for the compositor.
  env -u G_DEBUG -u MALLOC_PERTURB_ -u ASAN_OPTIONS -u UBSAN_OPTIONS -u LD_PRELOAD \
    gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x800 \
    --wayland-display jr-test >"$XDG_RUNTIME_DIR/shell.log" 2>&1 &
  shell=$!
  i=0
  while [ ! -e "$XDG_RUNTIME_DIR/jr-test" ] && [ $i -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
  if [ ! -e "$XDG_RUNTIME_DIR/jr-test" ]; then
    kill "$shell" 2>/dev/null
    echo "headless gnome-shell did not start:"; tail -5 "$XDG_RUNTIME_DIR/shell.log"
    exit 99
  fi
  mkdir -p "$XDG_RUNTIME_DIR/tmp"
  TMPDIR="$XDG_RUNTIME_DIR/tmp" WAYLAND_DISPLAY=jr-test GDK_BACKEND=wayland GTK_A11Y=none "$@"
  status=$?
  kill "$shell" 2>/dev/null
  wait "$shell" 2>/dev/null
  exit $status
fi

if command -v gnome-shell >/dev/null 2>&1 && command -v dbus-run-session >/dev/null 2>&1; then
  # Wayland socket paths are limited to 108 bytes, so keep this short.
  base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  rt=$(mktemp -d "$base/jr-ui.XXXXXX") || exit 99
  trap 'rm -rf "${rt:?}"' EXIT INT TERM
  mkdir -p "$rt/config" "$rt/data" "$rt/cache" "$rt/state"
  env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$rt" XDG_CONFIG_HOME="$rt/config" XDG_DATA_HOME="$rt/data" \
    XDG_CACHE_HOME="$rt/cache" XDG_STATE_HOME="$rt/state" JR_IN_HEADLESS_SHELL=1 \
    dbus-run-session --config-file="$here/headless-bus.conf" -- "$0" "$@"
  exit $?
fi

if command -v gtk4-broadwayd >/dev/null 2>&1; then
  dir="${XDG_RUNTIME_DIR:-/tmp}"
  n=40
  while [ -e "$dir/broadway$((n + 1)).socket" ]; do n=$((n + 1)); done
  tmp=$(mktemp -d "$dir/jr-ui.XXXXXX") || exit 99
  gtk4-broadwayd ":$n" >/dev/null 2>&1 &
  daemon=$!
  # broadwayd leaves its socket behind when killed, so remove it too.
  trap 'kill "$daemon" 2>/dev/null; wait "$daemon" 2>/dev/null;
        rm -f "$dir/broadway$((n + 1)).socket"; rm -rf "${tmp:?}"' EXIT INT TERM
  i=0
  while [ ! -e "$dir/broadway$((n + 1)).socket" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
  env -u WAYLAND_DISPLAY -u DISPLAY TMPDIR="$tmp" GDK_BACKEND=broadway \
    BROADWAY_DISPLAY=":$n" GSK_RENDERER=broadway "$@"
  exit $?
fi

echo "No headless display available (gnome-shell or gtk4-broadwayd): skipping UI test"
exit 77
