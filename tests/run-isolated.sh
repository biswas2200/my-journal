#!/bin/sh
# Runs a test with TMPDIR pointing at a fresh private folder that is
# deleted afterwards, even if the test crashes. Test journals and keys
# never outlive the run.
#
# JR_TEST_WRAPPER wraps only the test binary (not this script), e.g.
#   JR_TEST_WRAPPER='valgrind --leak-check=full --error-exitcode=1' meson test ...
set -u
base="${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}"
tmp=$(mktemp -d "$base/jr-test.XXXXXX") || exit 99
trap 'rm -rf "${tmp:?}"' EXIT INT TERM
set -f # the wrapper is split into words but never globbed
TMPDIR="$tmp" ${JR_TEST_WRAPPER:-} "$@"
