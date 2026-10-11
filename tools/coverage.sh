#!/bin/sh
# Builds with coverage, runs every test (UI tests off-screen) and writes
# build-cov/coverage.xml (SonarQube's generic coverage format, paths from
# the project root) from gcov's reports. Prints a summary.
set -eu
cd "$(dirname "$0")/.."
[ -d build-cov ] || meson setup build-cov -Dbuildtype=debug -Db_coverage=true
find build-cov -name '*.gcda' -delete
meson compile -C build-cov
meson test -C build-cov --print-errorlogs
rm -rf build-cov/gcov-reports
mkdir -p build-cov/gcov-reports
# gcov runs from build-cov, where the compiler ran, so "../src/..." in the
# coverage data leads to the sources (anywhere else the reports come out
# empty).
for gcda in $(find build-cov -name '*.gcda' | grep -E 'journal-(core|ui)\.a\.p|journal\.p'); do
  rel=${gcda#build-cov/}
  (cd build-cov && gcov -p -o "$(dirname "$rel")" "$rel" >/dev/null 2>&1)
done
mv build-cov/*.gcov build-cov/gcov-reports/
# Keep only reports for our own sources.
find build-cov/gcov-reports -name '*.gcov' ! -name '*src#*' -delete
# gcov names each source relative to the build folder ("../src/core/x.c");
# Sonar needs "src/core/x.c". Count "-" = not code, "#####" = never run.
awk -F: '
  BEGIN { print "<coverage version=\"1\">" }
  FNR == 1 && NR > 1 { print "  </file>" }
  $3 == "Source" { p = $4; sub(/^(\.\.\/)+/, "", p); printf "  <file path=\"%s\">\n", p; next }
  { c = $1; gsub(/ /, "", c) }
  $2 + 0 > 0 && c != "-" {
    printf "    <lineToCover lineNumber=\"%d\" covered=\"%s\"/>\n", $2, (c ~ /^[0-9]/ ? "true" : "false")
  }
  END { if (NR > 0) print "  </file>"; print "</coverage>" }
' $(find build-cov/gcov-reports -name '*.gcov' | sort) > build-cov/coverage.xml
for gcda in $(find build-cov -name '*.gcda' | grep -E 'journal-(core|ui)\.a\.p|journal\.p'); do
  gcov -n -o "$(dirname "$gcda")" "$gcda" 2>/dev/null
done | awk '/^File/ {f=$2} /^Lines executed/ {split($2,a,":"); sub("%","",a[2]);
  if (f ~ /src\/.*\.c.$/) { pct[f]=a[2]; n[f]=$4 } }
  END { for (f in n) { c+=pct[f]*n[f]/100; t+=n[f] }
        printf "Line coverage: %.1f%% of %d lines in src/\n", c*100/t, t }'
