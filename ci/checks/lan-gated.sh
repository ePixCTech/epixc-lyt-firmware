#!/bin/bash
# The release image actually gates the LAN API. Run by ci/build.sh:  ci/checks/lan-gated.sh <env>
#
# The LAN gate is a BUILD FLAG, which is the whole reason this check exists. `PIXC_LAN_AUTH`
# can be dropped from platformio_override.ini in one keystroke, the firmware will compile
# cleanly, every test will pass, and the shipped device will answer any stranger on the
# customer's Wi-Fi again. Nothing in the source would look wrong, because the source would be
# correct - it is the flag that would be missing.
#
# D295 established the flag reaches the compiler by planting an undeclared symbol inside the
# guarded block. That proved it once, by hand, on one machine. This proves it on every build.
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds). Changed only
# where it read GitHub's variables: the env is the first argument.
set -euo pipefail
. "$(dirname "$0")/nm.sh"
GATE=$("$NM" "$ELF" | grep -c 'pixcAuthorise' || true)
ALEXA=$("$NM" "$ELF" | grep -ci 'espalexa' || true)

# The gate. `pixcAuthorise` (pairing v2, wled00/pixc_lan.cpp) has external linkage precisely so this is checkable:
# were it static it could be inlined away and its absence would prove nothing.
if [[ "$GATE" -eq 0 ]]; then
  echo "error: this image links no pixcAuthorise symbol, so the LAN JSON API,"
  echo "error: the legacy /win query API and every GET /json are OPEN to anybody on"
  echo "error: the network. -D PIXC_LAN_AUTH is missing from platformio_override.ini."
  echo "error: See Debt Register D295."
  exit 1
fi
echo "the LAN API is gated (pixcAuthorise is linked)"

# Alexa, the one command path that could not be gated because the protocol carries no
# credential, so it was removed, and staying removed is the only guarantee available.
# Checked in the SAME step as the gate because they are one decision: the LAN surface is
# closed, or it is not, and a Hue bridge on port 80 reopens it without touching any of
# the code the gate protects.
if [[ "$ALEXA" -ne 0 ]]; then
  echo "error: this image links Espalexa ($ALEXA symbols). It answers SSDP on UDP 1900"
  echo "error: and serves an unauthenticated Hue API on port 80: it has nowhere to"
  echo "error: carry the device PIN, so it cannot be gated, only removed. D296."
  exit 1
fi
echo "no Espalexa in the image"
