"""Prints every failed test in meson's JSON logs as a GitHub annotation
(annotations are readable without signing in, unlike job logs), with the
last lines of its output, so script tests without "not ok" lines show too.

Reads build*/meson-logs/testlog.json under the project root and nothing
else; it takes no arguments.

  python3 -I tools/report-failures.py
"""
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
shown = 0
for path in sorted(root.glob("build*/meson-logs/testlog.json")):
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        continue
    for line in lines:
        try:
            t = json.loads(line)
        except ValueError:
            continue
        if t.get("result") in ("OK", "SKIP", "EXPECTEDFAIL"):
            continue
        out = (t.get("stdout") or "") + (t.get("stderr") or "")
        tail = [l for l in out.splitlines() if l.strip()][-6:]
        msg = f"{t.get('name')} ({t.get('result')}, exit {t.get('returncode')}): " + " | ".join(tail)
        print("::error::" + msg.replace("%", "%25").replace("\r", "").replace("\n", " ")[:900])
        shown += 1
if shown == 0:
    print("no failed tests in the JSON logs")
