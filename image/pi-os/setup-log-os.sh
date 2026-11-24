#!/bin/bash
# setup-log-os.sh - turns a Raspberry Pi OS Lite (64-bit, no desktop) into a "Log computer":
# it boots straight into the Log terminal and your autorun program starts by itself.
#
#   sudo ./setup-log-os.sh                 downloads the latest logl-linux-arm64 from the GitHub release and checks its SHA-256
#   sudo ./setup-log-os.sh /path/to/logl   uses a binary you already have (no download)
#
# Run it from the image/pi-os folder of the repository. It is safe to run twice.
# Test switches (for developers):  DRY=1 only prints the system commands;  DESTDIR=/tmp/x installs the files under /tmp/x.
# STATUS: the file handling and the checksum check are tested; the systemd / raspi-config / boot steps are NOT TESTED ON A REAL RASPBERRY PI.
set -eu

DRY="${DRY:-0}"; DESTDIR="${DESTDIR:-}"
REPO="${LOGL_REPO:-unlisted-cmyk/unlisted-cmyk.github.io}"
BASE="${LOGL_BASE_URL:-https://github.com/$REPO/releases/latest/download}"
ASSET="${LOGL_ASSET:-logl-linux-arm64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SESSION_SRC="$HERE/../common/logl-session"
UNIT_SRC="$HERE/logl-console.service"

run() { if [ "$DRY" = 1 ]; then echo "+ $*"; else "$@"; fi; }   # commands that change the SYSTEM (systemctl, raspi-config): only printed in a dry run
say() { echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

if [ "$DRY" = 1 ] && [ -z "$DESTDIR" ]; then
  DESTDIR="$(mktemp -d)"; say "dry run: files go under $DESTDIR only; system commands are just printed"
fi
if [ "$DRY" != 1 ] && [ -z "$DESTDIR" ]; then
  [ "$(id -u)" = 0 ] || die "run it with sudo"
  [ "$(uname -m)" = aarch64 ] || [ "${LOGL_ALLOW_ANY_ARCH:-0}" = 1 ] || die "this needs the 64-bit Raspberry Pi OS (aarch64); this system is $(uname -m). (Pi 3, 4, 5 and Zero 2 W can run it; Pi Zero, Pi 1 and Pi 2 cannot.)"
fi
[ -f "$SESSION_SRC" ] && [ -f "$UNIT_SRC" ] || die "run this script from the image/pi-os folder of the repository (logl-session / logl-console.service not found)"

# ---- 1. the logl executable (checked against the SHA-256 published with the release)
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
if [ -n "${1:-}" ]; then
  [ -f "$1" ] || die "$1 does not exist"
  cp "$1" "$TMP/logl"; say "using $1"
else
  say "downloading $ASSET from $BASE"
  curl -fsSL -o "$TMP/$ASSET" "$BASE/$ASSET" || die "could not download $ASSET (is there a release with that file?)"
  curl -fsSL -o "$TMP/SHA256SUMS.txt" "$BASE/SHA256SUMS.txt" || die "could not download SHA256SUMS.txt"
  WANT="$(grep -E "[[:space:]]\*?$ASSET\$" "$TMP/SHA256SUMS.txt" | awk '{print $1}' | head -n1)"
  [ -n "$WANT" ] || die "$ASSET is not listed in SHA256SUMS.txt"
  GOT="$(sha256sum "$TMP/$ASSET" | awk '{print $1}')"
  [ "$WANT" = "$GOT" ] || die "SHA-256 DOES NOT MATCH (expected $WANT, got $GOT): the download is corrupt or was tampered with. Nothing was installed."
  mv "$TMP/$ASSET" "$TMP/logl"; say "SHA-256 verified: $GOT"
fi
chmod 755 "$TMP/logl"
"$TMP/logl" --version >/dev/null 2>&1 || [ "$DRY" = 1 ] || [ -n "$DESTDIR" ] || die "the downloaded logl does not run on this system"

install -d "$DESTDIR/usr/local/bin" "$DESTDIR/etc/systemd/system" "$DESTDIR/home/log"
install -m 755 "$TMP/logl" "$DESTDIR/usr/local/bin/logl"
install -m 755 "$SESSION_SRC" "$DESTDIR/usr/local/bin/logl-session"
install -m 644 "$UNIT_SRC" "$DESTDIR/etc/systemd/system/logl-console.service"
if [ ! -f "$DESTDIR/etc/logl.conf" ]; then
  cat > "$DESTDIR/etc/logl.conf" <<'CONF'
# Settings of the Log computer (read by logl-session at every start).
# LOGL_FLAGS gives every program full access to files, pins and network. This computer is meant to be yours alone.
# For a safer setup remove --allow-all and add only what you need, for example:  LOGL_FLAGS="--hw --allow-pins --no-window"
#LOGL_FLAGS="--hw --allow-all --no-window"
#LOGL_HOME=/home/log
#LOGL_AUTORUN_DIR=/boot/firmware
CONF
fi
say "installed logl, logl-session, the service and /etc/logl.conf"

# ---- 2. the folder where autorun.log* is looked for: /boot/firmware on Bookworm and newer, /boot before
if [ -d "$DESTDIR/boot/firmware" ]; then BOOT="$DESTDIR/boot/firmware"; else BOOT="$DESTDIR/boot"; fi
if [ -d "$BOOT" ]; then
  # quiet boot: no kernel messages on the Log terminal, no splash, no blinking cursor (added only once)
  if [ -f "$BOOT/cmdline.txt" ] && ! grep -q "logo.nologo" "$BOOT/cmdline.txt"; then
    sed -i '1 s/$/ quiet loglevel=3 logo.nologo vt.global_cursor_default=0/' "$BOOT/cmdline.txt"
  fi
  if [ -f "$BOOT/config.txt" ] && ! grep -q "^disable_splash=1" "$BOOT/config.txt"; then
    echo "disable_splash=1" >> "$BOOT/config.txt"
  fi
  if [ -f "$BOOT/autorun.log+" ] || ls "$BOOT"/autorun.log* >/dev/null 2>&1; then say "an autorun.log* already exists in $BOOT"; else say "to start a program at power-on, put a file named autorun.log+ in $BOOT"; fi
fi

# ---- 3. hardware buses for I2C, SPI and the serial port (no login on the serial port)
if command -v raspi-config >/dev/null 2>&1 || [ "$DRY" = 1 ]; then
  run raspi-config nonint do_i2c 0
  run raspi-config nonint do_spi 0
  run raspi-config nonint do_serial_cons 1
  run raspi-config nonint do_serial_hw 0
else
  say "raspi-config not found: enable I2C, SPI and the serial port by hand if you need them (dtparam=i2c_arm=on, dtparam=spi=on, enable_uart=1 in config.txt)"
fi

# ---- 4. Log instead of the login prompt on the first console
run systemctl daemon-reload
run systemctl disable getty@tty1.service
run systemctl enable logl-console.service
say "done. Reboot (sudo reboot) and the Raspberry Pi starts in the Log terminal."
say "To get a shell back once: create an empty file named logl-shell in $BOOT (or use SSH, which this script did not touch)."
