#!/usr/bin/env python3
# The image fits the OTA slot. Run by ci/build.sh after `pio run -e <env>`:  ci/checks/ota-slot.py <env>
#
# A partition table is written by the flasher and CANNOT be changed over OTA, so a unit's
# layout is fixed for its life. If a build ever produces an image larger than the OTA slot,
# the fleet cannot be updated at all, and PlatformIO's own size check would only warn.
# Fail instead.
#
# The slot size is read from the partition binary this build just produced, not hardcoded, so
# the gate cannot drift from tools/WLED_ESP32_8MB_epixc.csv, and the CSV is checked against
# that binary too, in case board_build.partitions ever stops pointing at it and the board's
# own default_8MB.csv (larger app slots) silently takes over.
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds). Changed only
# where it read GitHub's variables: the env is the first argument.
import glob, hashlib, os, struct, sys

ENV = sys.argv[1] if len(sys.argv) > 1 else "PixC_V1"
CSV = "tools/WLED_ESP32_8MB_epixc.csv"
build_dir = os.path.join(".pio", "build", ENV)
app_bin = os.path.join(build_dir, "firmware.bin")
part_bin = os.path.join(build_dir, "partitions.bin")

def fail(msg):
    print(f"error: {msg}")
    sys.exit(1)

for path in (app_bin, part_bin):
    if not os.path.isfile(path):
        fail(f"{path} is missing: the build did not produce what this gate measures")

# Parse the flashed partition table. Entries are 32 bytes, magic 0xAA 0x50.
built = {}
blob = open(part_bin, "rb").read()
for i in range(0, len(blob), 32):
    entry = blob[i:i + 32]
    if entry[:2] != b"\xaa\x50":
        break
    ptype, subtype = entry[2], entry[3]
    _offset, size = struct.unpack("<II", entry[4:12])
    name = entry[12:28].rstrip(b"\x00").decode()
    if ptype == 0x00 and subtype in (0x10, 0x11):   # app / ota_0, ota_1
        built[name] = size
if not built:
    fail("the built partition table declares no OTA app slots")

# The same numbers as read from the CSV in the repo, so the two cannot disagree unnoticed.
declared = {}
for line in open(CSV):
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    cols = [c.strip() for c in line.split(",")]
    if len(cols) >= 5 and cols[1] == "app":
        declared[cols[0]] = int(cols[4], 0)
if declared != built:
    fail(f"{CSV} declares {declared} but the build flashed {built}: "
         "board_build.partitions is not pointing at the ePixC table")

slot = min(built.values())
size = os.path.getsize(app_bin)

# The uploaded artifact is what a device is actually asked to swallow. Prove it is the
# image just measured, not a leftover from another env or an earlier version. PixC_V1_dev
# now carries its own WLED_RELEASE_NAME (..._UNSIGNED_DEV) so the two no longer collide
# on one filename, which makes a stray bench binary here visible rather than confusable.
release = sorted(glob.glob("build_output/release/*.bin"))
if len(release) != 1:
    fail(f"expected exactly one release binary, found {release}")
sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()
if sha(release[0]) != sha(app_bin):
    fail(f"{release[0]} is not the image this build produced")

print("partitions: " + ", ".join(f"{n} {s} bytes" for n, s in sorted(built.items())))
print(f"artifact:   {release[0]}")
print(f"image {size} bytes of {slot} ({size * 100 // slot}% of the slot)")
if size > slot:
    fail("Image exceeds the OTA slot: devices in the field could not take this update.")
