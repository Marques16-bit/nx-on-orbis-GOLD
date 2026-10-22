#!/usr/bin/env bash
# Prints the CHANGELOG.md section of one version: the text of its GitHub release notes.
#   bash ci/release-notes.sh 0.3.0      (a leading "v" is accepted)
set -euo pipefail
v="${1:?usage: release-notes.sh VERSION}"; v="${v#v}"
notes="$(awk -v v="$v" '
    /^## \[/ { if (on) exit; if (index($0, "[" v "]") > 0) { on = 1; next } }
    on { print }
' "$(dirname "$0")/../CHANGELOG.md")"
if [ -z "${notes//[[:space:]]/}" ]; then
    echo "CHANGELOG.md has no section for version $v" >&2
    exit 1
fi
printf '%s\n' "$notes"
