#!/bin/sh
# Runs one libFuzzer target (see fuzz/README.md):
#
#   tools/fuzz.sh TARGET [SECONDS]
#
# Needs a clang build in build-fuzz:
#   CC=clang meson setup build-fuzz -Dfuzz=true -Db_sanitize=address,undefined \
#     -Db_lundef=false -Dbuildtype=debugoptimized -Dwerror=false
#
# Starts from the seeds in fuzz/corpus/TARGET plus what earlier runs
# found (fuzz-corpus/TARGET, kept). A crash is saved as
# fuzz-crashes/TARGET-crash-<hash>; copy it into fuzz/corpus/TARGET once
# fixed, so every test run replays it.
set -eu
cd "$(dirname "$0")/.."
target=$1
seconds=${2:-60}
exe=build-fuzz/fuzz/fuzz_$target
if [ ! -x "$exe" ]; then
  echo "No $exe: build with -Dfuzz=true first (see fuzz/README.md)." >&2
  exit 2
fi
max_len=4096
[ "$target" = journal_file ] && max_len=65536
mkdir -p "fuzz-corpus/$target" fuzz-crashes
# journal_file writes a file per input: keep it in memory when possible.
if [ -z "${TMPDIR:-}" ] && [ -w /dev/shm ]; then
  export TMPDIR=/dev/shm
fi
"$exe" "fuzz-corpus/$target" "fuzz/corpus/$target" \
  -max_total_time="$seconds" -max_len="$max_len" -timeout=10 -rss_limit_mb=2048 \
  -artifact_prefix="fuzz-crashes/$target-" -print_final_stats=1
