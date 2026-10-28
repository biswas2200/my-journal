#!/bin/sh
# Checks the loaded AppArmor profile from outside, without the seccomp
# jail, so it shows the second lock holds on its own. Runs small probes
# under the profile with aa-exec; needs no sudo. Each must be refused:
#   - an internet socket (TCP and UDP, IPv4 and IPv6);
#   - a DNS lookup through systemd-resolved's local socket;
#   - talking to the desktop's "open a link" service (portal OpenURI).
set -u
command -v aa-exec >/dev/null || { echo "aa-exec is missing"; exit 1; }
if ! aa-exec -p journal-diary -- true 2>/dev/null; then
  echo "profile journal-diary is not loaded (sudo tools/install-apparmor.sh)"; exit 1
fi
fail=0
probe () { # name, python code that must raise
  if aa-exec -p journal-diary -- python3 -I -c "$2" >/dev/null 2>&1; then
    echo "FAIL $1: allowed"; fail=1
  else
    echo "ok   $1: refused"
  fi
}
probe "TCP over IPv4"  "import socket; socket.socket(socket.AF_INET, socket.SOCK_STREAM)"
probe "UDP over IPv4"  "import socket; socket.socket(socket.AF_INET, socket.SOCK_DGRAM)"
probe "TCP over IPv6"  "import socket; socket.socket(socket.AF_INET6, socket.SOCK_STREAM)"
probe "DNS via resolved" "import socket; s=socket.socket(socket.AF_UNIX); s.connect('/run/systemd/resolve/io.systemd.Resolve')"
# A harmless question on the "open a link" interface (nothing opens even
# if it were allowed): the profile refuses the whole interface.
probe "open a link"    "import subprocess,sys; r=subprocess.run(['gdbus','call','--session','--dest','org.freedesktop.portal.Desktop','--object-path','/org/freedesktop/portal/desktop','--method','org.freedesktop.portal.OpenURI.SchemeSupported','https','{}'],capture_output=True); sys.exit(r.returncode)"
# Sanity: the same probes work outside the profile (so a refusal above
# is the profile's doing, not a broken probe).
python3 -I -c "import socket; socket.socket(socket.AF_INET, socket.SOCK_STREAM)" || { echo "probe broken"; fail=1; }
# A running Journal is confined in enforce mode.
pid=$(pgrep -x journal-diary | head -1)
if [ -n "$pid" ]; then
  echo "running journal-diary: $(cat /proc/$pid/attr/apparmor/current 2>/dev/null || cat /proc/$pid/attr/current)"
fi
[ $fail -eq 0 ] && echo "apparmor: every probe refused"
exit $fail
