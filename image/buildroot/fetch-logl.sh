#!/bin/bash
# fetch-logl.sh - downloads logl-linux-arm64 from the GitHub release into overlay/usr/bin/logl and verifies its SHA-256.
#   ./fetch-logl.sh                 latest release of the repository
#   ./fetch-logl.sh /path/to/logl   use a binary you already have
# Settings: LOGL_REPO (owner/name), LOGL_BASE_URL (full folder URL; tests use file://), LOGL_ASSET
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${LOGL_REPO:-unlisted-cmyk/unlisted-cmyk.github.io}"
BASE="${LOGL_BASE_URL:-https://github.com/$REPO/releases/latest/download}"
ASSET="${LOGL_ASSET:-logl-linux-arm64}"
OUT="$HERE/overlay/usr/bin/logl"
die() { echo "ERROR: $*" >&2; exit 1; }
mkdir -p "$HERE/overlay/usr/bin"
if [ -n "${1:-}" ]; then
  [ -f "$1" ] || die "$1 does not exist"
  install -m 755 "$1" "$OUT"; echo "using $1"; exit 0
fi
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
curl -fsSL -o "$T/$ASSET" "$BASE/$ASSET" || die "could not download $ASSET"
curl -fsSL -o "$T/SHA256SUMS.txt" "$BASE/SHA256SUMS.txt" || die "could not download SHA256SUMS.txt"
WANT="$(grep -E "[[:space:]]\*?$ASSET\$" "$T/SHA256SUMS.txt" | awk '{print $1}' | head -n1)"
[ -n "$WANT" ] || die "$ASSET is not listed in SHA256SUMS.txt"
GOT="$(sha256sum "$T/$ASSET" | awk '{print $1}')"
[ "$WANT" = "$GOT" ] || die "SHA-256 DOES NOT MATCH (expected $WANT, got $GOT). Nothing was written."
install -m 755 "$T/$ASSET" "$OUT"; echo "logl verified ($GOT) and placed in $OUT"
