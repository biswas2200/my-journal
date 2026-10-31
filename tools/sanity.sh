#!/bin/sh
# Sanity check before a release (and after any big change): builds and
# installs this working tree and an earlier release side by side, each
# into its own throwaway folder (no sudo, nothing touches the installed
# app or your notes), then runs the real programs on throwaway profiles
# on a private headless display:
#
#   tools/sanity.sh            this tree against the newest v* tag
#   tools/sanity.sh v0.1.0     this tree against v0.1.0
#
# What it checks, for both builds: `journal --version`; the Journal starts
# hardened (memory protected, network jail) and closes cleanly; Notes
# starts on an empty profile and closes cleanly, and how long it takes to
# open. Between them: notes written by the old build open in the new one
# with every row intact (and get a backup), then open in the old one
# again (going back a version loses nothing). The checks themselves are
# in tools/sanity-checks.sh.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
old_ref=${1:-$(git describe --tags --abbrev=0 --match 'v*')}
work=$(mktemp -d "${TMPDIR:-/tmp}/journal-sanity.XXXXXX")
cleanup () {
  git -C "$root" worktree remove --force "$work/old-src" 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

# Builds a source tree into $work/<name>-build and installs it under
# $work/<name> (its own prefix, so its programs find each other there).
build () {
  name=$1 src=$2
  echo "== building $name"
  meson setup "$work/$name-build" "$src" --prefix="$work/$name" --buildtype=release >"$work/$name-setup.log" 2>&1 ||
    { tail -20 "$work/$name-setup.log"; exit 1; }
  meson compile -C "$work/$name-build" >"$work/$name-build.log" 2>&1 || { tail -20 "$work/$name-build.log"; exit 1; }
  meson install -C "$work/$name-build" --no-rebuild >"$work/$name-install.log" 2>&1 ||
    { tail -20 "$work/$name-install.log"; exit 1; }
}

build new "$root"
git worktree add --detach "$work/old-src" "$old_ref" >/dev/null 2>&1
build old "$work/old-src"

echo "== running both ($old_ref and this tree) on throwaway profiles"
"$root/tests/run-headless.sh" "$root/tools/sanity-checks.sh" "$work/new" "$work/old" "$old_ref" "$work"
