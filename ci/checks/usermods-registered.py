#!/usr/bin/env python3
# The usermods are registered. Run by ci/build.sh after `pio run -e <env>`:
#   ci/checks/usermods-registered.py <env>
#
# WLED finds its usermods in a "dynamic array" (wled00/dynarray.h): each REGISTER_USERMOD puts one
# pointer in a .dynarray.usermods.<key> section, and pio-scripts/dynarray.py adds a KEEP for those
# sections to the linker script. If that script does not find the linker script, nothing keeps the
# pointers: --gc-sections drops them, and the image links and boots with no usermod. For ePixC that
# is pixc_connect_blink: no provisioning, no MQTT, and an OTA image that is never confirmed. Platform
# espressif32 8.0.0 did exactly that (fw-idf5 stage 2), and the build only printed
# "0 usermod object entries found".
#
# Read from the map file this build just produced: the entries between the array's begin and end
# markers, by the object file each came from.
import os, re, sys

ENV = sys.argv[1] if len(sys.argv) > 1 else "PixC_V1"
# PixC_V1's custom_usermods (platformio_override.ini), by the folder of their object files.
EXPECTED = ("pixc_connect_blink", "audioreactive")
mapfile = "build_output/map/%s.map" % ENV


def fail(msg):
    print("error: " + msg)
    sys.exit(1)


if not os.path.isfile(mapfile):
    fail(f"{mapfile} is missing, so it cannot tell which usermods were linked")

lines = open(mapfile, errors="ignore").read().splitlines()
try:
    start = lines.index("Linker script and memory map")
except ValueError:
    fail(f"{mapfile} has no memory map")

# " .dynarray.usermods.00001" and, on the same line or the next, "0x<address> 0x<size> <object>".
name_rx = re.compile(r"^ \.dynarray\.usermods\.(\d{5})(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*))?$")
place_rx = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
entries = []
for i in range(start, len(lines)):
    m = name_rx.match(lines[i])
    if not m:
        continue
    key, addr, size, obj = m.groups()
    if addr is None and i + 1 < len(lines):
        p = place_rx.match(lines[i + 1])
        if p:
            addr, size, obj = p.groups()
    if addr is not None:
        entries.append((key, int(addr, 16), int(size, 16), obj))

begin = [a for k, a, s, o in entries if k == "00000"]
end = [a for k, a, s, o in entries if k == "99999"]
if len(begin) != 1 or len(end) != 1:
    fail(f"expected one begin and one end marker of the usermod array, found {len(begin)} and {len(end)}")
members = [o for k, a, s, o in entries if k not in ("00000", "99999") and s > 0 and begin[0] <= a < end[0]]
if (end[0] - begin[0]) // 4 != len(members):
    fail(f"the usermod array spans {end[0] - begin[0]} bytes, but {len(members)} entries were found in it")

missing = [u for u in EXPECTED if not any(f"/{u}/" in o for o in members)]
if missing:
    fail(f"no entry of {missing} in the usermod array: the image would run without "
         f"{' and '.join(missing)} (pio-scripts/dynarray.py found no linker script to patch?)")
print(f"{len(members)} usermods registered: " + ", ".join(os.path.basename(o) for o in members))
