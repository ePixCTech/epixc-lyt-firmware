#!/usr/bin/env python3
# Bootloader rollback is compiled in. Run by ci/build.sh after `pio run -e <env>`:
#   ci/checks/bootloader-rollback.py <env>
#
# Bootloader rollback is the other half of a safe OTA: without it, an image that flashes
# successfully and then cannot reach the broker is a brick until someone drives to it.
#
# It is not a source-level decision. It is CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in the
# sdkconfig of whichever prebuilt Arduino framework the build linked against, and this fork
# has had TWO candidates installed at different times: the Tasmota fork of arduino-esp32 has it
# OFF, upstream espressif32 has it ON. `esp_ota_mark_app_valid_cancel_rollback()` links and
# returns ESP_OK either way, so the difference is invisible in the code and invisible at
# build time, and only shows as `rollback_armed: false` on a device's boot event, which is
# to say, in the field.
#
# The framework is read out of the map file this build just produced, so this cannot drift
# from what was actually linked. Since fw-idf5 stage 2 (platform espressif32 8.0.0, Arduino 3.3.12)
# the prebuilt ESP-IDF is its own package, framework-arduinoespressif32-esp32s3-libs, with the
# sdkconfig and versions.txt at its top. The same sdkconfig also has to keep:
#   - CONFIG_APP_ROLLBACK_ENABLE: the app side of the same rollback (esp_ota_mark_app_*);
#   - CONFIG_MBEDTLS_CERTIFICATE_BUNDLE: esp-tls calls `crt_bundle_attach`, the hook that sets the
#     pinned roots (pixcTlsAttach, D411), only when it is on;
#   - CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT off: on, esp-tls frees the CA chain after the first
#     handshake, which is the shared pinned roots, and every later TLS connection fails.
# And versions.txt must name ESP-IDF v5.5.5, the version the bench tests (docs/pixc-idf5-bench.md).
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds). Changed
# where it read GitHub's variables (the env is the first argument, and PlatformIO's home is
# PLATFORMIO_CORE_DIR when set) and for the IDF 5 package layout.
import os, re, sys

IDF = "v5.5.5"

mapfile = "build_output/map/%s.map" % (sys.argv[1] if len(sys.argv) > 1 else "PixC_V1")
if not os.path.isfile(mapfile):
    print(f"error: {mapfile} is missing, so it cannot tell which framework was linked")
    sys.exit(1)


def fail(msg):
    print("error: " + msg)
    sys.exit(1)


pkgs = set(re.findall(r"packages/(framework-arduinoespressif32[^/\s]*)", open(mapfile, errors="ignore").read()))
libs = sorted(p for p in pkgs if re.fullmatch(r"framework-arduinoespressif32-esp32s3-libs(@\S+)?", p))
if len(libs) != 1:
    fail(f"expected exactly one ESP32-S3 libs package (framework-arduinoespressif32-esp32s3-libs) in the "
         f"link, found {sorted(pkgs)}")
pkg = libs[0]
home = os.path.join(os.environ.get("PLATFORMIO_CORE_DIR") or os.path.expanduser("~/.platformio"), "packages", pkg)
for name in ("sdkconfig", "versions.txt"):
    if not os.path.isfile(os.path.join(home, name)):
        fail(f"{pkg} has no {name} at its top: this is not the Arduino 3.x libs package layout")

cfg = open(os.path.join(home, "sdkconfig")).read().splitlines()
if "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" not in cfg:
    fail(f"{pkg} was built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE off. "
         "A failed OTA would leave the device on the broken image with no automatic "
         "revert, and nothing in the firmware can compensate: the bootloader is prebuilt.")
if "CONFIG_APP_ROLLBACK_ENABLE=y" not in cfg:
    fail(f"{pkg} was built with CONFIG_APP_ROLLBACK_ENABLE off: the app cannot hold a new image "
         "unconfirmed until it reaches the broker (D35)")
if "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y" not in cfg:
    fail(f"{pkg} was built with CONFIG_MBEDTLS_CERTIFICATE_BUNDLE off: esp-tls would not call "
         "pixcTlsAttach, and no pinned root would be set for MQTT")
if "CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y" in cfg:
    fail(f"{pkg} was built with CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT on: esp-tls would free the "
         "shared pinned roots after the first handshake")

versions = open(os.path.join(home, "versions.txt")).read()
m = re.search(r"^esp-idf:\s*(\S+)", versions, re.M)
if not m or m.group(1) != IDF:
    fail(f"{pkg} is built on ESP-IDF {m.group(1) if m else '(not named)'}, not {IDF}: the platform pin "
         "moved, so the bench results do not apply to this image")

print(f"{pkg}: ESP-IDF {IDF}, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, CONFIG_APP_ROLLBACK_ENABLE=y, "
      "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT off")
