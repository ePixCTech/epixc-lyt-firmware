#!/bin/bash
# The release image has no plaintext HTTP client. Run by ci/build.sh:  ci/checks/no-plaintext-http.sh <env>
#
# Ticket 33. The device provisions itself by asking an API which MQTT broker to trust, and
# that is the one bootstrap step with nothing protecting it: whoever answers first names
# the broker and collects the unit's credential.
#
# The transport used to be chosen at RUNTIME from `_apiPort == 443`, and `_apiPort` comes
# out of `POST /json/cfg`, which is gated on a settings PIN that shipped unset. So a
# guarantee of "no plaintext fallback" was revocable by anyone on the customer's Wi-Fi.
#
# This asserts the fix in the LINKED BINARY, not in the source. The previous version of this
# guarantee was a comment reasoning that no release build could reach the plaintext path;
# a symbol count of zero is a different kind of statement from an argument.
#
# `nm` on the elf rather than a grep of the map file: `build_output/map/` is written by a
# post-build script that does not always run, and a check whose input can be absent is a
# check that can pass by accident.
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds). Changed only
# where it read GitHub's variables: the env is the first argument.
set -euo pipefail
. "$(dirname "$0")/nm.sh"
COUNT=$("$NM" "$ELF" | grep -ci 'HTTPClient' || true)

# Both directions. The bench env compiles the plaintext path in deliberately, so there
# it must be PRESENT: without that half, a rename of the symbol would read as "no
# plaintext client" forever and the zero would stop meaning anything.
if [[ "$ENV_NAME" == *_dev ]]; then
  if [[ "$COUNT" -eq 0 ]]; then
    echo "error: the bench image links no HTTPClient symbols: either the plaintext"
    echo "error: path is gone from the dev env too, or this check matches nothing now"
    exit 1
  fi
  echo "bench image links the plaintext client ($COUNT symbols), as intended"
else
  if [[ "$COUNT" -ne 0 ]]; then
    echo "error: the release image links a plaintext HTTP client ($COUNT symbols)."
    echo "error: Provisioning is HTTPS only (see ticket 33)."
    echo "error: EPIXC_ALLOW_PLAINTEXT_PROVISION belongs to PixC_V1_dev alone."
    exit 1
  fi
  echo "release image links no plaintext HTTP client"
fi
