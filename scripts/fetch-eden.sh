#!/usr/bin/env bash
# Fetches Eden at the commit this port is based on (the one ProsperoEden pins) into deps/eden and
# applies this port's patches on top, one commit each.
#
#   bash scripts/fetch-eden.sh            # patches/eden (the v0.1.0 state on main)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMIT=5f142c7926d0c7fcbbd0ce30794d72f638a43b2a
SHA256=35ec0bb96b197181e66c0aeb8f44f0ff05334be832c631c50a4f1e271277285f   # ProsperoEden's UPSTREAM.json
URL="https://github.com/eden-emulator/mirror/archive/$COMMIT.tar.gz"
DEST="$ROOT/deps/eden"

[ -e "$DEST" ] && { echo "$DEST exists; remove it first"; exit 1; }
mkdir -p "$ROOT/deps"
TAR="$ROOT/deps/eden-${COMMIT:0:8}.tar.gz"
[ -f "$TAR" ] || curl -L --fail -o "$TAR" "$URL"
echo "$SHA256  $TAR" | sha256sum -c - || echo "warning: checksum differs from ProsperoEden's (GitHub archives are not always byte-stable); continuing"
mkdir -p "$DEST"
tar -xzf "$TAR" -C "$DEST" --strip-components=1
cd "$DEST"
git init -q
git config core.autocrlf false
git add -A
git -c user.name=base -c user.email=base@localhost commit -q -m "Eden ${COMMIT:0:12} (pristine, ProsperoEden pin)"
git am -q --keep-cr "$ROOT"/patches/eden/*.patch
git log --oneline -n 3   # (not "| head": under pipefail head closing the pipe early is exit 141)
echo "Eden ready in $DEST"
