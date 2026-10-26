#!/bin/bash
# The box's pipeline runs this before it builds a tag (epixc-backend deploy/native/pipeline): every
# check of the old workflow that needs no image. A failure stops the build. Needs a C++17 compiler,
# Python 3 and openssl; no PlatformIO. ci/build.sh runs the checks that read the image.
set -euo pipefail
cd "$(dirname "$0")/.."
step() { printf '\n==> %s\n' "$1"; }

step "host tests (test/pixc)";                     bash test/pixc/run.sh
step "claims still match the code they point at";  python3 tools/claims_evidence.py
step "the retired product name";                   python3 scripts/brand-check.py
step "the pinned CA roots";                        python3 ci/checks/ca-roots.py

# The console's own rule for a release's version (FirmwareController, SEGMENT). The version is
# WLED's versionString, the one every light reports; the tag is lyt-v<version> and the box checks that.
step "a version the console accepts"
version=$(python3 -c 'import json; print(json.load(open("package.json"))["version"])')
[[ "$version" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] || {
  echo "error: package.json version '$version' is not one the console accepts"; exit 1; }
echo "package.json says $version: the tag for it is lyt-v$version"
