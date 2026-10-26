#!/bin/bash
# verifyRollbackLater is overridden. Run by ci/build.sh:  ci/checks/rollback-later.sh <env>
#
# An OTA image is confirmed only once it has reached the broker (D35). The weak default of
# verifyRollbackLater confirms it at boot, before anything has proved the image can be updated
# again. Checked in the linked binary: exactly one strong definition.
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds), where it
# was the second half of "Check the release image carries no bench defaults". Changed only where
# it read GitHub's variables: the env is the first argument.
set -euo pipefail
. "$(dirname "$0")/nm.sh"
if [[ $("$NM" "$ELF" | grep -c ' verifyRollbackLater$' || true) -ne 1 ]]; then
  echo "error: verifyRollbackLater is not overridden: OTA images would be confirmed before the broker (D35)"
  exit 1
fi
echo "verifyRollbackLater is overridden"
