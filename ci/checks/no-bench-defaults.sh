#!/bin/bash
# The release image carries no bench defaults. Run by ci/build.sh:  ci/checks/no-bench-defaults.sh <env>
#
# Release-shaped images must carry none of the bench conveniences (audit B2, pairing v2):
# WLED_DEBUG (it printed MQTT payloads to the UART), the dev hotspot password, the bench API
# address, and plaintext MQTT. Checked in the binary, because each is one build flag deep.
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds), where it
# ran on the release-shaped target only, as here. Changed only where it read GitHub's variables:
# the env is the first argument, and the strings are read before they are matched (below). The
# verifyRollbackLater half of that step is ci/checks/rollback-later.sh.
set -euo pipefail
. "$(dirname "$0")/nm.sh"
bad=0
# Read first, then matched: `strings | grep -q` under pipefail fails when grep stops at the first
# match and strings is cut off, which read as "not found" (in the workflow too).
STRINGS=$(strings "$BIN")
for s in "MQTT msg:" "epixcbench" "192.168.0.12"; do
  if grep -qF "$s" <<<"$STRINGS"; then echo "error: release image contains '$s'"; bad=1; fi
done
[ "$bad" -eq 0 ] && echo "release image carries no bench defaults"
exit $bad
