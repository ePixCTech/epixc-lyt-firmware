#!/usr/bin/env python3
# The image reports package.json's version. Run by ci/build.sh on the file it hands the box:
#   ci/checks/build-version.py <pixc.bin>
#
# Every light reports WLED's versionString as fw_version (provisioning, health, boot events), and
# the backend offers a release to each light whose fw_version is not that release's version.
# versionString is the version in WLED's build description (wled_metadata_t, magic "WSTS",
# wled00/wled_metadata.cpp). pio-scripts/set_metadata.py sets it from package.json, the version the
# box names the release after (the tag lyt-v<version>). If that script stops reaching
# wled_metadata.cpp, the file uses upstream's placeholder, "17.0.0-devV5" ("remove once that
# set_metadata.py is fixed for V5"), with a warning only. The image builds, every light on it
# reports a version that no release has, and the backend offers the same release again and again.
#
# Read from the image's bytes, as the light's own OTA check finds it (findWledMetadata): the magic,
# then a description whose hash is the DJB2 hash of its release name.
import json, struct, sys

MAGIC = struct.pack("<I", 0x57535453)
# magic, desc_version, wled_version[48], release_name[48], hash, safe_update_version[3]; packed
LAYOUT = struct.Struct("<II48s48sI3s")


def fail(msg):
    print("error: " + msg)
    sys.exit(1)


def djb2(s):
    h = 5381
    for c in s:
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def text(field):
    return field.split(b"\0", 1)[0].decode("ascii", "replace")


image = sys.argv[1] if len(sys.argv) > 1 else fail("usage: ci/checks/build-version.py <image>")
data = open(image, "rb").read()
want = json.load(open("package.json"))["version"]

found = []
at = data.find(MAGIC)
while at >= 0:
    if at + LAYOUT.size <= len(data):
        _, _, version, name, h, _ = LAYOUT.unpack_from(data, at)
        release = name.split(b"\0", 1)[0]
        if release and djb2(release) == h:
            found.append((text(version), text(name)))
    at = data.find(MAGIC, at + 1)

if len(found) != 1:
    fail(f"expected one WLED build description in the image, found {len(found)}")
version, release = found[0]
if version != want:
    fail(f"the image reports version '{version}', but package.json says '{want}': "
         "pio-scripts/set_metadata.py did not set WLED_VERSION for wled_metadata.cpp")
print(f"the image reports {version} ({release}), as package.json says")
