#!/usr/bin/env python3
"""The retired brand must not reach a customer from the firmware either.

The product is ePixC. `PixC` is the pre-rename spelling and `HyperAtlas` a retired company name.

**This repository had no such guard until 2026-08-28, and that is exactly why it needed one.** The
factory-reset page told the customer *"Connect to PixC-AP to setup again"* — a network no unit has
broadcast since `DEFAULT_AP_BASE` became "ePixC", months after the app had pinned the new spelling
in `pairing_platform_test.dart`. Of every string in this firmware it is close to the worst one to
get wrong: it is shown at the exact moment somebody has erased their configuration and has nothing
to go on but that sentence.

Scope is `wled00/` and `usermods/`, which is where anything the device says lives. Whole files are
read, comments included, for the reason the website's equivalent gives: **a comment today is copy
tomorrow**, and three separate strings in these repositories were copied out of a comment that had
gone stale first.

Whitespace is flattened before matching, so a name split across a line break cannot hide — the
blind spot that let a split wordmark defeat the website's line-based check.

Identifiers are exempt because renaming them breaks a build or a path and no customer ever reads
them: `PixC_V1` (and `_dev`) are PlatformIO environments named in `default_envs`, and the
`pixc_connect_blink` usermod directory is a path in `platformio_override.ini` and every include.
The exemption is by exact shape, never by file, so a new *sentence* in those files still fails.

**Keep the exemption list narrow, and check each entry against a plant.** The first version of it
carried a blanket `pixc-` prefix, which swallowed the retired AP name — the guard read green
against the exact string it was written to catch, and only planting that string revealed it. The
one thing `pixc-` was protecting turned out to be `pixc-mqtt`, which is a RETIRED service name
too (the Rust crate is `epixc-mqtt`), so the exemption existed solely to hide two defects.

A second entry went the same way an hour later. `pixc/d/` was exempted as "the MQTT topic prefix
every shipped unit publishes on" — and it is not. The usermod builds `epixc/v1/d/%s`, which is
what the broker subscribes to; the exemption was protecting six stale comments describing a topic
that has never existed. **An exemption is a claim about the code, and it decays like any other.**
Both were written by the same hand that wrote the guard, in the same sitting.
"""
import pathlib
import re
import sys

ROOTS = ("wled00", "usermods")
SUFFIXES = {".cpp", ".h", ".ino", ".htm", ".html", ".js", ".json", ".md"}

# `(?<![eE])` so ePixC itself passes. Case-insensitive, because the string that started this was
# capitalised correctly and still wrong.
FORBIDDEN = re.compile(r"hyperatlas|(?<![eE])\bPixC\b", re.I)

# On-disk identifiers, not copy. Build environments, the usermod path, and the source filenames
# that carry it. Ordered longest-first so `PixC_V1_dev` is consumed before `PixC_V1`.
IDENTIFIERS = re.compile(
    r"PixC_V1_dev|PixC_V1|pixc_connect_blink|pixc_https|pixc_roots|pixc_led_bus|"
    r"pixc_ota_pubkey|PIXC_[A-Z0-9_]+",
    re.I,
)

# `pixc` is also a KEY ON THE WIRE, which is a stronger reason to keep it than any of the above:
# `DeviceService.java` writes `cfg.put("pixc", pixc)` and this firmware reads
# `root["pixc"]["led_channels"]`. Renaming it is a coordinated change across the server, the
# firmware and every unit already in a customer's house, for a string no customer ever sees.
#
# Matched by SHAPE — the bracket access, the dotted path, and the cfg-fragment list — never by
# file. A sentence in these same files that happens to use the retired name still fails, which is
# the whole distinction: the exemption covers the protocol, not the prose around it.
WIRE_KEYS = re.compile(r"""\["pixc"\]|\bpixc\.led_channels\b|def/light/pixc""")


def main() -> int:
    hits = []
    for root in ROOTS:
        base = pathlib.Path(root)
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            flat = re.sub(r"\s+", " ", text)
            flat = WIRE_KEYS.sub(" ", flat)
            flat = IDENTIFIERS.sub(" ", flat)
            for m in FORBIDDEN.finditer(flat):
                hits.append(f"  {path}: …{flat[max(0, m.start() - 50):m.start() + 50].strip()}…")

    if hits:
        print(f"Retired brand name in the firmware ({len(hits)}):\n")
        print("\n".join(hits))
        print("\nThe product is ePixC. Identifiers are exempt by shape; sentences are not.")
        return 1

    print("Firmware carries no retired brand name.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
