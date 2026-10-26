#!/usr/bin/env python3
# What a first flash over USB needs, beside the app (pixc.bin), from the build ci/build.sh just made:
#   ci/flash-files.py <env> <idedata.json or -> <out>
#
# Copies bootloader.bin, partitions.bin and boot_app0.bin into <out> and writes <out>/flash.json, which
# the factory flashing tool reads: the chip, the flash mode, frequency and size, and each file with its
# offset. The box signs pixc.bin only; these three are written to a board as they are.
#
# Read from the build, not assumed:
#   - the files and their offsets from PlatformIO's idedata (`pio project metadata`), and the app's
#     offset checked against the ota_0 row of tools/WLED_ESP32_8MB_epixc.csv. Without idedata ("-"):
#     the bootloader at 0x0 (ESP32-S3), the partition table at 0x8000 (ESP-IDF's default), boot_app0
#     at the CSV's otadata offset, the app at its ota_0 offset;
#   - the chip, flash mode, frequency and size from the header of the bootloader.bin and firmware.bin
#     this build wrote (esptool's elf2image puts them there), and the two must agree. For a board set
#     to qio this reads dio: the image header says how the ROM loads the bootloader, which then
#     switches the flash to QIO itself.
import glob, hashlib, json, os, shutil, sys

ENV, IDEDATA, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
CSV = "tools/WLED_ESP32_8MB_epixc.csv"
build_dir = os.path.join(".pio", "build", ENV)
WANT = ("bootloader.bin", "partitions.bin", "boot_app0.bin")

CHIPS = {0x0000: "esp32", 0x0002: "esp32s2", 0x0005: "esp32c3", 0x0009: "esp32s3", 0x000C: "esp32c2",
         0x000D: "esp32c6", 0x0010: "esp32h2", 0x0012: "esp32p4"}
MODES = {0: "qio", 1: "qout", 2: "dio", 3: "dout"}
SIZES = {0: "1MB", 1: "2MB", 2: "4MB", 3: "8MB", 4: "16MB", 5: "32MB", 6: "64MB", 7: "128MB"}
FREQS = {0x0: "40m", 0x1: "26m", 0x2: "20m", 0xF: "80m"}


def fail(msg):
    print("error: " + msg)
    sys.exit(1)


def header(path):
    """Chip and flash settings from an ESP32 image header: magic 0xE9, segments, mode, size and
    frequency; the chip id at byte 12 of the extended header."""
    head = open(path, "rb").read(24)
    if len(head) < 24 or head[0] != 0xE9:
        fail(f"{path} is not an ESP32 image")
    chip = CHIPS.get(head[12] | head[13] << 8)
    mode, size, freq = MODES.get(head[2]), SIZES.get(head[3] >> 4), FREQS.get(head[3] & 0xF)
    if None in (chip, mode, size, freq):
        fail(f"{path} has a header this does not know: bytes {head[:4].hex()}, chip {head[12:14].hex()}")
    return {"chip": chip, "flash_mode": mode, "flash_size": size, "flash_freq": freq}


def csv_offsets():
    rows = {}
    for line in open(CSV):
        line = line.split("#", 1)[0].strip()
        cols = [c.strip() for c in line.split(",")] if line else []
        if len(cols) >= 4 and cols[3]:
            rows[cols[2]] = int(cols[3], 0)
    if "ota_0" not in rows or "ota" not in rows:
        fail(f"{CSV} has no ota_0 or otadata row")
    return rows


rows = csv_offsets()
images = {}
app_offset = None
if IDEDATA != "-":
    data = json.load(open(IDEDATA))
    data = data.get(ENV, data)
    extra = data.get("extra") or {}
    for img in extra.get("flash_images") or []:
        images[os.path.basename(img["path"])] = (int(str(img["offset"]), 0), img["path"])
    if extra.get("application_offset"):
        app_offset = int(str(extra["application_offset"]), 0)
    if set(images) != set(WANT) or app_offset is None:
        fail(f"PlatformIO's idedata lists {sorted(images)} at application offset {app_offset}; "
             f"a first flash needs {list(WANT)} and the app's offset")
else:
    core = os.environ.get("PLATFORMIO_CORE_DIR") or os.path.expanduser("~/.platformio")
    boot_app0 = glob.glob(os.path.join(core, "packages", "framework-arduinoespressif32*", "tools", "partitions",
                                       "boot_app0.bin"))
    if len(boot_app0) != 1:
        fail(f"expected one boot_app0.bin in the Arduino framework, found {boot_app0}")
    images = {"bootloader.bin": (0x0, os.path.join(build_dir, "bootloader.bin")),
              "partitions.bin": (0x8000, os.path.join(build_dir, "partitions.bin")),
              "boot_app0.bin": (rows["ota"], boot_app0[0])}
    app_offset = rows["ota_0"]

if app_offset != rows["ota_0"]:
    fail(f"the build puts the app at {app_offset:#x}, but {CSV} has ota_0 at {rows['ota_0']:#x}")
if images["boot_app0.bin"][0] != rows["ota"]:
    fail(f"the build puts boot_app0 at {images['boot_app0.bin'][0]:#x}, but {CSV} has otadata at {rows['ota']:#x}")

settings = header(os.path.join(build_dir, "bootloader.bin"))
app = header(os.path.join(build_dir, "firmware.bin"))
if app != settings:
    fail(f"bootloader.bin says {settings} but firmware.bin says {app}: the build is not one flash setup")

files = []
for name in WANT:
    offset, path = images[name]
    if not os.path.isfile(path):
        fail(f"{path} is missing: the build did not produce what a first flash needs")
    shutil.copyfile(path, os.path.join(OUT, name))
    files.append((offset, name))
files.append((app_offset, "pixc.bin"))

doc = dict(settings)
doc["files"] = []
for offset, name in sorted(files):
    blob = open(os.path.join(OUT, name), "rb").read()
    # "name", as the box's signing.py reads it and the signed flash-manifest.json carries it.
    doc["files"].append({"offset": f"{offset:#x}", "name": name, "size": len(blob),
                         "sha256": hashlib.sha256(blob).hexdigest()})
with open(os.path.join(OUT, "flash.json"), "w") as f:
    json.dump(doc, f, indent=2)
    f.write("\n")
print(f"{doc['chip']}, {doc['flash_mode']} {doc['flash_freq']} {doc['flash_size']}: " +
      ", ".join(f"{x['name']} at {x['offset']}" for x in doc["files"]))
