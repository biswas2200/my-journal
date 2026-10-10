#!/bin/sh
# Builds with coverage, runs every test (UI tests off-screen) and writes
# gcov reports to build-cov/gcov-reports for SonarQube. Prints a summary.
set -eu
cd "$(dirname "$0")/.."
[ -d build-cov ] || meson setup build-cov -Dbuildtype=debug -Db_coverage=true
find build-cov -name '*.gcda' -delete
meson compile -C build-cov
meson test -C build-cov --print-errorlogs
rm -rf build-cov/gcov-reports
mkdir -p build-cov/gcov-reports
for gcda in $(find build-cov -name '*.gcda' | grep -E 'journal-(core|ui)\.a\.p|journal\.p'); do
  (cd build-cov/gcov-reports && gcov -p -o "../../$(dirname "$gcda")" "../../$gcda" >/dev/null 2>&1)
done
# Keep only reports for our own sources.
find build-cov/gcov-reports -name '*.gcov' ! -name '*src#*' -delete
for gcda in $(find build-cov -name '*.gcda' | grep -E 'journal-(core|ui)\.a\.p|journal\.p'); do
  gcov -n -o "$(dirname "$gcda")" "$gcda" 2>/dev/null
done | awk '/^File/ {f=$2} /^Lines executed/ {split($2,a,":"); sub("%","",a[2]);
  if (f ~ /src\/.*\.c.$/) { pct[f]=a[2]; n[f]=$4 } }
  END { for (f in n) { c+=pct[f]*n[f]/100; t+=n[f] }
        printf "Line coverage: %.1f%% of %d lines in src/\n", c*100/t, t }'
