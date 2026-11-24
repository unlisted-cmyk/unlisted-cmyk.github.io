#!/bin/bash
# build.sh - builds a minimal Log OS image for the Raspberry Pi 4 / 400 / CM4 (64-bit) with Buildroot.
#   ./build.sh /path/to/buildroot [/path/to/logl-linux-arm64]
# Result: <buildroot>/output/images/sdcard.img  (write it with Raspberry Pi Imager "Use custom", or dd).
# STATUS: NOT TESTED. Neither the Buildroot build nor the boot of the result has been run by the author. Read the README before trusting it.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
BR="${1:?usage: ./build.sh /path/to/buildroot [logl-binary]}"
[ -f "$BR/Makefile" ] && [ -d "$BR/configs" ] || { echo "ERROR: $BR is not a Buildroot checkout"; exit 1; }
bash "$HERE/fetch-logl.sh" ${2:+"$2"}
make -C "$BR" raspberrypi4_64_defconfig
# keep the board's own post-build scripts and add ours
CUR="$(grep '^BR2_ROOTFS_POST_BUILD_SCRIPT=' "$BR/.config" | sed 's/^[^=]*=//; s/^"//; s/"$//' || true)"
sed -i '/^BR2_ROOTFS_POST_BUILD_SCRIPT=/d' "$BR/.config"
echo "BR2_ROOTFS_POST_BUILD_SCRIPT=\"$CUR $HERE/post-build.sh\"" >> "$BR/.config"
sed "s#@HERE@#$HERE#g" "$HERE/logl.fragment" >> "$BR/.config"
make -C "$BR" olddefconfig
make -C "$BR"
echo "Done: $BR/output/images/sdcard.img"
