#!/usr/bin/env python3
# Bootloader rollback is compiled in. Run by ci/build.sh after `pio run -e <env>`:
#   ci/checks/bootloader-rollback.py <env>
#
# Bootloader rollback is the other half of a safe OTA: without it, an image that flashes
# successfully and then cannot reach the broker is a brick until someone drives to it.
#
# It is not a source-level decision. It is CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in the
# sdkconfig of whichever prebuilt Arduino framework the build linked against, and this fork
# has TWO candidates installed at different times: the Tasmota fork of arduino-esp32 has it
# OFF, upstream espressif32 has it ON. `esp_ota_mark_app_valid_cancel_rollback()` links and
# returns ESP_OK either way, so the difference is invisible in the code and invisible at
# build time, and only shows as `rollback_armed: false` on a device's boot event, which is
# to say, in the field.
#
# The framework is read out of the map file this build just produced, so this cannot drift
# from what was actually linked.
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds). Changed only
# where it read GitHub's variables: the env is the first argument, and PlatformIO's home is
# PLATFORMIO_CORE_DIR when set.
import glob, os, re, sys

mapfile = "build_output/map/%s.map" % (sys.argv[1] if len(sys.argv) > 1 else "PixC_V1")
if not os.path.isfile(mapfile):
    print(f"error: {mapfile} is missing, so it cannot tell which framework was linked")
    sys.exit(1)

pkgs = set(re.findall(r"packages/(framework-arduinoespressif32[^/\s]*)", open(mapfile, errors="ignore").read()))
if len(pkgs) != 1:
    print(f"error: expected exactly one Arduino framework package in the link, found {sorted(pkgs)}")
    sys.exit(1)
pkg = pkgs.pop()

hits = glob.glob(os.path.join(os.environ.get("PLATFORMIO_CORE_DIR") or os.path.expanduser("~/.platformio"), f"packages/{pkg}/tools/**/esp32s3/sdkconfig"), recursive=True)
if len(hits) != 1:
    print(f"error: could not locate a single esp32s3 sdkconfig in {pkg}, found {hits}")
    sys.exit(1)

cfg = open(hits[0]).read()
if "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" not in cfg:
    print(f"error: {pkg} was built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE off. "
          "A failed OTA would leave the device on the broken image with no automatic "
          "revert, and nothing in the firmware can compensate: the bootloader is prebuilt.")
    sys.exit(1)
print(f"{pkg}: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y")
