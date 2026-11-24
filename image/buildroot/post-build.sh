#!/bin/sh
# Buildroot post-build script of the Log OS image. $1 = target rootfs. Runs after the overlay is applied.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="$1"
install -m 755 "$HERE/../common/logl-session" "$TARGET/usr/bin/logl-session"
mkdir -p "$TARGET/home/log"
# hardware buses: appended to the firmware config.txt that genimage puts on the boot partition (same mechanism Buildroot's own Raspberry Pi scripts use)
CFG="${BINARIES_DIR:-}/rpi-firmware/config.txt"
if [ -f "$CFG" ]; then
  for l in "dtparam=i2c_arm=on" "dtparam=spi=on" "enable_uart=1" "disable_splash=1"; do grep -q "^$l\$" "$CFG" || echo "$l" >> "$CFG"; done
fi
