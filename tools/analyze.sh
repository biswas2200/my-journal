#!/bin/sh
# GCC's path-sensitive static analyzer (-fanalyzer) over app and tests:
# leaks, use-after-free, double free, NULL dereference, fd leaks.
# Exits non-zero if it finds anything.
set -eu
cd "$(dirname "$0")/.."
[ -d build-analyzer ] || meson setup build-analyzer -Dbuildtype=debug -Dwerror=false -Dc_args=-fanalyzer
ninja -C build-analyzer -t clean >/dev/null
out=$(ninja -C build-analyzer 2>&1)
n=$(printf '%s\n' "$out" | grep -c '\[-Wanalyzer' || true)
printf '%s\n' "$out" | grep -A20 '\[-Wanalyzer' || true
echo "Static analyzer findings: $n"
[ "$n" -eq 0 ]
