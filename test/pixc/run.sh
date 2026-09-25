#!/usr/bin/env bash
# Host-side unit tests for the ePixC firmware's pure logic. Needs only a C++17 compiler.
# Usage: test/pixc/run.sh        (CXX=g++ test/pixc/run.sh to pick a compiler)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -O1 \
  -I"$root/usermods/pixc_connect_blink" -I"$root/wled00" \
  -o "$out/pixc_host_tests" "$here/test_pixc.cpp"
"$out/pixc_host_tests"
