#!/bin/sh
# Loads the Journal's AppArmor profile: the second network lock, next to
# the seccomp jail the program enters itself. Run once after installing
# (and again after reinstalling to another prefix):
#
#   sudo tools/install-apparmor.sh [PREFIX]      (default /usr/local)
#
# It copies the installed profile to /etc/apparmor.d/journal-diary and
# loads it in enforce mode. Remove with:
#   sudo apparmor_parser -R /etc/apparmor.d/journal-diary && sudo rm /etc/apparmor.d/journal-diary
set -eu
prefix="${1:-/usr/local}"
src="$prefix/share/journal/apparmor/journal-diary.apparmor"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$src" ] || { echo "not found: $src (run 'sudo meson install -C build' first)"; exit 1; }
command -v apparmor_parser >/dev/null || { echo "apparmor_parser is missing (apt install apparmor)"; exit 1; }
install -m 0644 "$src" /etc/apparmor.d/journal-diary
apparmor_parser -r -W /etc/apparmor.d/journal-diary
echo "loaded: journal-diary ($(grep -o 'profile journal-diary [^ ]*' /etc/apparmor.d/journal-diary | cut -d' ' -f3))"
echo "check it with: tools/check-apparmor.sh"
