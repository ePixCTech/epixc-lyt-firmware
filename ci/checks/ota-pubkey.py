#!/usr/bin/env python3
# An OTA public key is compiled in. Run by ci/build.sh after `pio run -e <env>`:
#   ci/checks/ota-pubkey.py <env>
#
# The device refuses to commit an OTA image unless its signature verifies against the key in
# usermods/pixc_connect_blink/pixc_ota_pubkey.h, so an image without one can never be updated.
# This checks the image carries the header's production key, an ECDSA P-256 public key, and not
# the empty dev define or the CI-only throwaway key.
#
# Which key it is, is the box's to check: it compares the image's key with its own OTA key when it
# signs (epixc-backend deploy/native/pipeline/signing.py), and signs nothing when they differ.
# The workflow this came from (.github/workflows/epixc-firmware.yaml, removed 2026-10-26) compared it
# with a GitHub secret instead; that half went with it.
import hashlib, os, re, subprocess, sys, tempfile

ENV = sys.argv[1] if len(sys.argv) > 1 else "PixC_V1"
header = "usermods/pixc_connect_blink/pixc_ota_pubkey.h"
app_bin = os.path.join(".pio", "build", ENV, "firmware.bin")


def fail(msg):
    print("error: " + msg)
    sys.exit(1)


src = open(header).read()
if re.search(r'^#define PIXC_OTA_PUBKEY_PEM ""', src, re.M):
    fail("the signing key is not in the header; the build would produce an image that refuses every OTA")

# The production define is the one at the start of a line. The CI-only key is indented
# inside its #ifdef, and the example at the end is a comment, so neither matches.
m = re.search(r'^#define PIXC_OTA_PUBKEY_PEM \\\n((?:[ \t]*"[^"\n]*"[ \t]*\\\n)*[ \t]*"[^"\n]*")', src, re.M)
if not m:
    fail("%s has neither the empty-key define nor a key this step can read; it reads by exact shape and "
         "must not guess" % header)
pem = "".join(re.findall(r'"([^"\n]*)"', m.group(1))).replace("\\n", "\n")

with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "ota_pub.pem")
    open(path, "w").write(pem)
    # Prove it is the curve both halves agree on. A wrong curve parses fine on the device and then
    # fails to verify anything the box signs.
    info = subprocess.run(["openssl", "pkey", "-pubin", "-in", path, "-noout", "-text"],
                          capture_output=True, text=True)
    if info.returncode != 0 or "NIST CURVE: P-256" not in info.stdout:
        fail("the key in %s is not an ECDSA P-256 public key. The device verifies with mbedTLS and the box "
             "signs SHA256withECDSA over prime256v1; nothing else interoperates." % header)
    der = subprocess.run(["openssl", "pkey", "-pubin", "-in", path, "-outform", "DER"],
                         check=True, capture_output=True).stdout

if not os.path.isfile(app_bin):
    fail(f"{app_bin} is missing: the build did not produce what this gate reads")
# The PEM is one string literal, so its bytes sit whole in the image. Absent means the build
# compiled another key in (the CI-only one) or none.
if pem.encode() not in open(app_bin, "rb").read():
    fail("%s does not carry the key in %s: this env compiled another key in (EPIXC_CI_TEST_ONLY_OTA_KEY) "
         "or none, and every light would refuse every update the box signs" % (app_bin, header))

print("OTA public key in this image, DER SHA-256: " + hashlib.sha256(der).hexdigest())
print("It must be the box's (Deploy, Machines, Firmware signing key); the box checks that when it signs.")
