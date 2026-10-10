#!/bin/sh
# Runs SonarQube's scanner (Docker image sonarsource/sonar-scanner-cli)
# on this project against a local SonarQube server. (SonarQube Cloud runs
# from GitHub Actions instead.) SONAR_PROJECT_KEY defaults to "Journal".
#
#   export SONAR_HOST_URL=http://localhost:9000   # your server
#   export SONAR_TOKEN=...                         # type it yourself
#   tools/coverage.sh && tools/sonar-scan.sh
#
# The project is mounted at the same path inside the container, so the
# absolute paths in compile_commands.json and the gcov reports still match.
set -eu
cd "$(dirname "$0")/.."
: "${SONAR_HOST_URL:?set SONAR_HOST_URL to your SonarQube server}"
: "${SONAR_TOKEN:?set SONAR_TOKEN (a SonarQube analysis token)}"
[ -f build/compile_commands.json ] || meson setup build
here=$(pwd)
mkdir -p "$HOME/.sonar"
# The token goes in as an environment variable, never on a command line.
exec docker run --rm --network host \
  -e SONAR_HOST_URL -e SONAR_TOKEN -e SONAR_USER_HOME=/sonar-home \
  -v "$HOME/.sonar:/sonar-home" \
  -v "$here:$here" -w "$here" \
  --user "$(id -u):$(id -g)" \
  sonarsource/sonar-scanner-cli:latest \
  -Dsonar.projectKey="${SONAR_PROJECT_KEY:-Journal}" -Dsonar.organization=
