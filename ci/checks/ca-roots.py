#!/usr/bin/env python3
# The pinned CA roots. Run by ci/test.sh (it needs no build):  python3 ci/checks/ca-roots.py
#
# The CA roots in usermods/pixc_connect_blink/pixc_roots.h are the entire trust store of
# this firmware. Provisioning (usermods/pixc_connect_blink/pixc_https.cpp) and MQTT over TLS
# (wled00/pixc_mqtt_client.cpp) both hand `cert_pem` that one bundle and trust nothing else.
#
# A pinned root that lapses is not a build failure, it is a fleet that stops provisioning and
# stops reaching the broker. And the repair is an OTA whose *command* arrives over the very
# MQTT/TLS link these roots anchor, so the fix has to ship BEFORE the root dies; afterwards
# there is no channel left to ship it on.
#
# Which is why checking expiry alone is not enough. A root valid to 2035 passes an expiry
# check every day for nine years and then fails on one: the same silence, one step removed.
# The gate is on REMAINING validity:
#
#     under 1095 days (36 months)  ->  warn loudly
#     under  730 days (24 months)  ->  fail the build
#
# 24 months is not a round number, it is the fleet-reach number. An ePixC controller is
# mains-powered decor, not an always-on appliance: it takes an update only while it happens
# to be plugged in, on Wi-Fi, and connected to the broker. Reaching effectively all of the
# fleet means spanning a full seasonal cycle, and a unit that is only used in one season a
# year needs a SECOND cycle to have a realistic second chance if it missed the first. Two
# cycles is 24 months. Below that, "push an OTA" stops being a remedy and turns into a
# support-and-RMA exercise: a business event, and a build should not be the thing that
# discovers it. The 36-month warning exists so the replacement root goes into a planned
# release instead of an emergency one; it gives a full year of noise before the hard stop.
#
# It also asserts pinning DEPTH, which matters more than the dates do. These must be
# self-signed roots. Pinning an intermediate (Let's Encrypt R10/R11, Google WE1) is the same
# code with a fuse of a year or two instead of a decade, and pinning a leaf breaks at every
# 90-day renewal, each of them repairable only over the connection the pin just broke.
#
# Taken from .github/workflows/epixc-firmware.yaml (removed 2026-10-26: the box builds), unchanged
# but for GitHub's annotation prefixes.
import datetime, os, re, subprocess, sys, tempfile

HEADER = "usermods/pixc_connect_blink/pixc_roots.h"

# Remaining-validity thresholds, in days. See the comment above for why these numbers.
FAIL_DAYS = 730    # 24 months
WARN_DAYS = 1095   # 36 months
MIN_ROOTS = 2

failed = []

def fail(msg):
    print("error: " + msg)
    failed.append(msg)

def openssl(args, ok_codes=(0,)):
    p = subprocess.run(["openssl"] + args, capture_output=True, text=True)
    if p.returncode not in ok_codes:
        return None, (p.stderr.strip() or p.stdout.strip())
    return p.stdout, None

def field(text, key):
    for line in text.splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    return ""

def parse_date(s):
    return datetime.datetime.strptime(" ".join(s.split()), "%b %d %H:%M:%S %Y %Z").replace(
        tzinfo=datetime.timezone.utc)

src = open(HEADER).read()
begins = src.count("-----BEGIN CERTIFICATE-----")
ends = src.count("-----END CERTIFICATE-----")
pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", src, re.S)
if not (begins == ends == len(pems)):
    fail("%s has %d BEGIN and %d END markers but %d complete certificates: a PEM block in the "
         "bundle is truncated, and mbedTLS would silently trust only the ones before it"
         % (HEADER, begins, ends, len(pems)))
    sys.exit(1)
if len(pems) < MIN_ROOTS:
    fail("%s pins %d root(s); at least %d are required. One root means a CA transition takes the "
         "whole fleet offline with no second trust anchor to fall back to." % (HEADER, len(pems), MIN_ROOTS))

now = datetime.datetime.now(datetime.timezone.utc)
seen = {}

for i, pem in enumerate(pems):
    with tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False) as fh:
        fh.write(pem + "\n")
        path = fh.name
    try:
        out, err = openssl(["x509", "-in", path, "-noout",
                            "-subject", "-issuer", "-startdate", "-enddate",
                            "-fingerprint", "-sha256"])
        if out is None:
            fail("certificate #%d in %s does not parse as X.509: %s" % (i + 1, HEADER, err))
            continue

        subject = field(out, "subject")
        issuer = field(out, "issuer")
        fp = field(out, "sha256 Fingerprint")
        not_before = parse_date(field(out, "notBefore"))
        not_after = parse_date(field(out, "notAfter"))
        days = (not_after - now).days
        name = re.search(r"CN\s*=\s*([^,/]+)", subject)
        name = name.group(1).strip() if name else subject

        print("  %-14s notAfter %s  (%d days, %.1f years)"
              % (name, not_after.strftime("%Y-%m-%d"), days, days / 365.2425))
        print("  %-14s sha256:%s" % ("", fp))

        # Pinning DEPTH. A root is self-issued and self-signed; an intermediate or a leaf is not.
        # Pinning either of those is the same code with a vastly shorter fuse: Let's Encrypt
        # rotates intermediates on a few-year cadence and leaves every 90 days, and repairing a
        # broken pin needs an OTA over the connection the pin just broke.
        if subject != issuer:
            fail("%s is not a root: its issuer (%s) differs from its subject. Only self-signed "
                 "roots may be pinned here: an intermediate rotates far more often than a root."
                 % (name, issuer))
        else:
            _, verr = openssl(["verify", "-no_check_time", "-CAfile", path, path])
            if verr is not None:
                fail("%s claims to be self-issued but its signature does not verify against its "
                     "own key: %s" % (name, verr))

        ext, _ = openssl(["x509", "-in", path, "-noout", "-ext", "basicConstraints"])
        if ext is None or "CA:TRUE" not in ext:
            fail("%s is not a CA certificate (basicConstraints CA:TRUE absent); it cannot anchor "
                 "a chain." % name)

        if fp in seen:
            fail("%s is the same certificate as %s (sha256 %s). Two copies of one root is one "
                 "root, and provides no transition headroom." % (name, seen[fp], fp))
        seen[fp] = name

        if not_before > now:
            fail("%s is not valid until %s: it is in the future, and every handshake against it "
                 "fails today." % (name, not_before.strftime("%Y-%m-%d")))

        # Remaining validity, not just expiry. A root that expires in 2035 passes an expiry check
        # every single day until the day it does not; the point of the floor is to fail while an
        # OTA can still reach the fleet.
        if days < 0:
            fail("%s EXPIRED on %s (%d days ago). Devices in the field can no longer complete a "
                 "handshake against it." % (name, not_after.strftime("%Y-%m-%d"), -days))
        elif days < FAIL_DAYS:
            fail("%s expires %s, in %d days, under the %d-day (24-month) floor. Replacing a "
                 "pinned root needs an OTA, and that OTA is commanded over the MQTT/TLS link "
                 "these roots anchor, so it has to land BEFORE the root dies. Twenty-four months "
                 "is two full seasonal cycles: the time it takes a fleet of intermittently "
                 "powered consumer controllers to come back online and take an update. Add the "
                 "replacement root to the bundle now."
                 % (name, not_after.strftime("%Y-%m-%d"), days, FAIL_DAYS))
        elif days < WARN_DAYS:
            print("warning: %s expires %s, in %d days (%.1f years), under the %d-day "
                  "(36-month) warning line. Schedule the replacement root into a planned release; "
                  "at %d days this build starts failing."
                  % (name, not_after.strftime("%Y-%m-%d"), days, days / 365.2425, WARN_DAYS,
                     FAIL_DAYS))
    finally:
        os.unlink(path)

if failed:
    sys.exit(1)
print("%d pinned roots, all self-signed CAs, all with more than %d days left." % (len(pems), FAIL_DAYS))
