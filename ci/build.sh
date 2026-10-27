#!/bin/bash
# The image the box signs and the console offers the lights (Deploy, Firmware), built after
# ci/test.sh passed, then held to every image gate of the old workflow (ci/checks/):
#   ci/build.sh <out>
#     <out>/pixc.bin                the app image; the box signs it (pixc.sig)
#     <out>/bootloader.bin, partitions.bin, boot_app0.bin, flash.json
#                                   what a first flash over USB needs, and how to write it
#                                   (ci/flash-files.py); written to a board as they are, not signed
# PlatformIO is requirements.txt's, in a venv under $HOME/.cache/epixc-pio, made once and made again
# when requirements.txt changes, with its own PlatformIO home beside it: a tool's Python modules
# (esptool's) are installed into the Python that installs the tool, so a venv made again must
# install the tools again too. PIO=<path to pio> uses another PlatformIO, and its home, instead.
set -euo pipefail
out="${1:?usage: ci/build.sh <out>}"
cd "$(dirname "$0")/.."
step() { printf '\n==> %s\n' "$1"; }
ENV_NAME=PixC_V1

if [ -z "${PIO:-}" ]; then
  cache="$HOME/.cache/epixc-pio"
  want=$(python3 -c 'import hashlib; print(hashlib.sha256(open("requirements.txt", "rb").read()).hexdigest())')
  if [ ! -x "$cache/venv/bin/pio" ] || [ "$(cat "$cache/requirements.sha256" 2>/dev/null || true)" != "$want" ]; then
    step "PlatformIO from requirements.txt, into $cache"
    rm -rf "$cache"
    mkdir -p "$cache"
    python3 -m venv "$cache/venv"
    "$cache/venv/bin/pip" install --quiet --disable-pip-version-check -r requirements.txt
    echo "$want" > "$cache/requirements.sha256"
  fi
  PIO="$cache/venv/bin/pio"
  export PLATFORMIO_CORE_DIR="$cache/core"
fi
step "$("$PIO" --version), node $(node --version)"

# The anti-rollback build number is the commit time in minutes (pio-scripts/pixc_build_id.py), so
# the same commit gets the same number on the box and on a laptop. The box builds `git archive`'s
# tree, which has no .git: there it is read from the files' time, which git archive sets to the
# commit time. Without a number the image says 0, and every light refuses it as a downgrade.
if [ -z "${EPIXC_BUILD_NUMBER:-}" ] && ! git rev-parse --git-dir >/dev/null 2>&1; then
  EPIXC_BUILD_NUMBER=$(python3 -c 'import os; print(int(os.stat("package.json").st_mtime) // 60)')
  export EPIXC_BUILD_NUMBER
fi
if [ -n "${EPIXC_BUILD_NUMBER:-}" ] && ! [ "${EPIXC_BUILD_NUMBER}" -gt 0 ] 2>/dev/null; then
  echo "error: the build number '${EPIXC_BUILD_NUMBER}' is not a positive number of minutes"; exit 1
fi

# build_output/ is where pio-scripts/output_bins.py drops the release copy, and it is
# gitignored, so a reused workspace could leave an older binary sitting there for the gate to
# measure instead. Start empty.
rm -rf build_output

# pio-scripts/build_ui.py runs `npm ci` and `npm run build` as a pre-action and aborts the build if
# node is missing, so the web UI toolchain is a hard dependency of `pio run`.
step "pio run -e $ENV_NAME";                       "$PIO" run -e "$ENV_NAME"
# The first-flash images by name: a plain `pio run` makes the bootloader and the partition table only when
# PlatformIO's cache already holds them, so a clean checkout (the box's) had neither.
step "bootloader.bin and partitions.bin";         "$PIO" run -e "$ENV_NAME" \
  -t ".pio/build/$ENV_NAME/bootloader.bin" -t ".pio/build/$ENV_NAME/partitions.bin"

step "the image fits the OTA slot";                python3 ci/checks/ota-slot.py "$ENV_NAME"
step "no plaintext HTTP client";                   bash ci/checks/no-plaintext-http.sh "$ENV_NAME"
step "the LAN API is gated";                       bash ci/checks/lan-gated.sh "$ENV_NAME"
step "no bench defaults";                          bash ci/checks/no-bench-defaults.sh "$ENV_NAME"
step "verifyRollbackLater is overridden";          bash ci/checks/rollback-later.sh "$ENV_NAME"
step "bootloader rollback is compiled in";         python3 ci/checks/bootloader-rollback.py "$ENV_NAME"
step "an OTA public key is compiled in";           python3 ci/checks/ota-pubkey.py "$ENV_NAME"

# The app image exactly as built, not a merged factory image: the box signs these bytes (pixc.sig).
cp ".pio/build/$ENV_NAME/firmware.bin" "$out/pixc.bin"
step "pixc.bin";                                   bash ci/checks/esp32-image.sh "$out/pixc.bin"

step "the first flash: bootloader, partition table, boot_app0, flash.json"
meta="$(mktemp -d "${TMPDIR:-/tmp}/epixc-pio-meta.XXXXXX")"
trap 'rm -rf "$meta"' EXIT
if "$PIO" project metadata -e "$ENV_NAME" --json-output --json-output-path "$meta/idedata.json" >"$meta/log" 2>&1 \
   && [ -s "$meta/idedata.json" ]; then
  python3 ci/flash-files.py "$ENV_NAME" "$meta/idedata.json" "$out"
else
  tail -5 "$meta/log"
  echo "PlatformIO gave no idedata: the offsets come from the partition table instead"
  python3 ci/flash-files.py "$ENV_NAME" - "$out"
fi
