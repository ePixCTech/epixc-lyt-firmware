#!/bin/bash
# The image is an ESP32 app image. Run by ci/build.sh on the file it hands the box:
#   ci/checks/esp32-image.sh <pixc.bin>
#
# An ESP32 image starts with the magic byte 0xE9; anything else is not firmware at all.
# Taken from the release job of .github/workflows/epixc-firmware.yaml (removed 2026-10-26).
set -euo pipefail
f="${1:?usage: ci/checks/esp32-image.sh <image>}"
[ "$(head -c 1 "$f" | od -An -tx1 | tr -d ' \n')" = e9 ] || { echo "error: $(basename "$f") is not an ESP32 image"; exit 1; }
echo "$(basename "$f") is an ESP32 image"
