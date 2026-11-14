#!/usr/bin/env python3
# The image names its ELF. Run by ci/build.sh after `pio run -e <env>`:  ci/checks/elf-sha.py <env>
#
# A light's crash report (the boot event's "panic", review 2026-11-14) names the crashed image by
# its ELF SHA-256: the dump has the first digits, and the image's descriptor (esp_app_desc_t,
# app_elf_sha256) the whole hash, which the light compares to find the image that crashed after an
# OTA revert. Decoding the crash's addresses needs that ELF. esptool writes the hash into the image
# only when told to (--elf-sha256-offset 0xb0, which Arduino's platformio-build.py adds); without
# it the field is 32 zero bytes, every crash reports "000000000", and no image can be matched. A
# platform or framework change could drop the flag with no warning, so this fails instead.
#
# The descriptor is the first thing in the image's first segment: a 24-byte image header, an 8-byte
# segment header, then esp_app_desc_t, whose magic word is 0xABCD5432 and whose app_elf_sha256 is
# at offset 0x90 (ESP-IDF esp_app_desc.h). So the hash is at 0xb0 in firmware.bin.
import hashlib, os, struct, sys

ENV = sys.argv[1] if len(sys.argv) > 1 else "PixC_V1"
build_dir = os.path.join(".pio", "build", ENV)
app_bin = os.path.join(build_dir, "firmware.bin")
app_elf = os.path.join(build_dir, "firmware.elf")

DESC_AT = 24 + 8
DESC_MAGIC = 0xABCD5432
ELF_SHA_AT = DESC_AT + 0x90


def fail(msg):
    print(f"error: {msg}")
    sys.exit(1)


for path in (app_bin, app_elf):
    if not os.path.isfile(path):
        fail(f"{path} is missing: the build did not produce what this gate reads")

image = open(app_bin, "rb").read()
if len(image) < ELF_SHA_AT + 32 or image[0] != 0xE9:
    fail(f"{app_bin} is not an ESP32 app image")
(magic,) = struct.unpack_from("<I", image, DESC_AT)
if magic != DESC_MAGIC:
    fail(f"{app_bin} has no app descriptor where ESP-IDF puts it (magic 0x{magic:08x})")

named = image[ELF_SHA_AT:ELF_SHA_AT + 32]
want = hashlib.sha256(open(app_elf, "rb").read()).digest()
if named == bytes(32):
    fail("the image's descriptor has no ELF SHA-256: esptool was not given --elf-sha256-offset 0xb0, "
         "so a crash report cannot name the image that crashed")
if named != want:
    fail(f"the image names ELF {named.hex()[:16]}, but firmware.elf is {want.hex()[:16]}: "
         "the ELF changed after the image was made")
print(f"the image names its ELF: {want.hex()[:16]} (the crash report's elf)")
