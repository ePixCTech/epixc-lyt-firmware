#!/usr/bin/env python3
"""Every row of the vault's Claims Checklist that the firmware settles, and its evidence.

[[44-claims-check-against-code]]. The full reasoning is in `epixc-web/scripts/claims_evidence.py`;
the short version is that the Claims Checklist went stale four times in a week and never on a day
anyone edited it, so the state moves into the repositories and the note becomes generated.

**This repository holds the rows that were wrong in the dangerous direction.** The checklist said
Art-Net, sACN, E1.31, DDP, DMX and Bluetooth were *"none implemented anywhere in the stack"* while
`e131.begin` and `ddp.begin` ran unconditionally on every shipped unit. A buyer told the controller
has no unauthenticated LAN listeners cannot make an informed decision about putting it on a shared
network. That row is a `must: present` here now: the day someone removes the listener the row goes
red and has to be rewritten, and the day someone adds one back the same thing happens.

**The OTA row is why the two-part shape exists at all.** *"Signed updates"* is neither true nor
false: the device verifies an ECDSA P-256 signature and refuses anything unsigned, and
`PIXC_OTA_PUBKEY_PEM` is empty so every build refuses every update. A human reading a one-line
status flipped it the wrong way twice. Two symbols on one row cannot be flipped wholesale — and
note that the row fails when the *key lands*, which is correct. That is not a regression; it is the
check insisting the sentence be rewritten on the day the truth changes, which is the one day it has
never been rewritten before.

    python3 tools/claims_evidence.py          # what CI runs
    python3 tools/claims_evidence.py --json   # for the vault generator

The engine below is byte-identical in epixc-web, epixc-backend and epixc-firmware. The vault
generator hashes the region in all three and refuses to render if they differ.
"""

REPO = "epixc-firmware"

CLAIMS = [
    {
        "id": "sacn-e131-artnet-listening",
        "section": "Protocols",
        "ticket": "161, D167",
        "row": "**sACN / E1.31 and Art-Net are listening on every shipped unit**, on one socket, "
               "with no enable flag in front of them.",
        "evidence": [
            {"file": "wled00/wled.cpp", "must": "present", "pattern": r"e131\.begin\("},
        ],
    },
    {
        "id": "ddp-listening",
        "section": "Protocols",
        "ticket": "161, D167",
        "row": "**DDP is listening on every shipped unit**, port 4048, unconditionally.",
        "evidence": [
            {"file": "wled00/wled.cpp", "must": "present", "pattern": r"ddp\.begin\("},
        ],
    },
    {
        "id": "dmx-input-compiled-inactive",
        "section": "Protocols",
        "ticket": "161",
        "row": "**DMX input is compiled in and inactive** — not absent. Its three pins default to "
               "`-1` and `DMXInput::init` returns early unless all three are set.",
        "evidence": [
            {"file": "platformio.ini", "must": "present", "pattern": r"WLED_ENABLE_DMX_INPUT"},
        ],
    },
    {
        "id": "dmx-output-absent",
        "section": "Protocols",
        "ticket": "161",
        "row": "**DMX output is genuinely absent** — `WLED_ENABLE_DMX` is never set.",
        # The negative lookahead is load-bearing: `WLED_ENABLE_DMX_INPUT` contains
        # `WLED_ENABLE_DMX` as a substring, so the naive pattern reports the flag as present and
        # this row fails on a truthful build. The check would then be corrected by loosening it,
        # which is how a guard ends up passing for the wrong reason.
        "evidence": [
            {"file": "platformio.ini", "must": "absent", "pattern": r"WLED_ENABLE_DMX(?!_INPUT)"},
        ],
    },
    {
        "id": "ble-absent",
        "section": "Protocols",
        "ticket": "161",
        "row": "**Bluetooth / BLE is genuinely absent** — no NimBLE, no `BLEDevice`, nothing.",
        "evidence": [
            {"file": "platformio.ini", "must": "absent", "pattern": r"NimBLE|BLEDevice|ESP32_BLE"},
        ],
    },
    {
        "id": "ota-verifies-but-has-no-key",
        "section": "Product capability",
        "ticket": "16, 174, D34/D35/D165",
        "row": "**The device verifies an ECDSA P-256 signature and refuses anything unsigned — and "
               "no customer receives a signed update**, because `PIXC_OTA_PUBKEY_PEM` is still "
               "empty and every build therefore refuses every image. The first half is claimable. "
               "The second is not.",
        # Two symbols, one row, and this is the shape the ticket was written around. Whichever half
        # moves, the row goes red and a person has to decide what the sentence should now say. On
        # the day `174` generates the key the second item fails, and that is the intended
        # behaviour: it is the day this sentence has always needed rewriting and never got it.
        "evidence": [
            {"file": "usermods/pixc_connect_blink/pixc_connect_blink.cpp", "must": "present",
             "pattern": r"verifySignature\("},
            {"file": "usermods/pixc_connect_blink/pixc_ota_pubkey.h", "must": "present",
             "pattern": r'#define PIXC_OTA_PUBKEY_PEM ""'},
        ],
    },
    {
        "id": "provisioning-is-https-only",
        "section": "Product capability",
        "ticket": "33",
        "row": "**Provisioning speaks HTTPS with ISRG Root X1 compiled in and has no plaintext "
               "fallback** — unvalidated on hardware, so it is built and not yet demonstrated.",
        "evidence": [
            {"file": "usermods/pixc_connect_blink/pixc_https.cpp", "must": "present",
             "pattern": r"esp_http_client"},
        ],
    },
    {
        "id": "handshake-on-a-real-unit",
        "section": "Product capability",
        "ticket": "33, 75",
        "row": "The provisioning handshake has completed against `api.epixc.in` on a real unit.",
        "unverifiable": "no file decides this. It needs a board, a network and the box being up — "
                        "`75`'s bench checklist owns it, and `api.epixc.in` does not resolve yet.",
    },
]

# ─── ENGINE BEGIN — byte-identical in every repository. The vault generator hashes this
# region in each copy and refuses to render if they differ, so the duplication is policed
# rather than trusted. Editing it means editing every copy in the same commit. ───────────
import hashlib
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def flattened(path):
    """The file's text with every run of whitespace collapsed to one space.

    Not a convenience. Four separate guards in this project have reported the wrong answer
    because they read a line at a time and the thing they were looking for was wrapped
    across two: the website brand check reported the brand it protects, the claims corpus
    could not see a wrapped claim, and both were fixed the same way. A `must: absent`
    pattern that a line break can hide from is a guard that passes by accident.
    """
    return re.sub(r"\s+", " ", path.read_text(errors="replace"))


def check_one(item):
    """(ok, detail, capture) for a single evidence item."""
    path = ROOT / item["file"]
    if not path.exists():
        return False, "the file named as evidence does not exist", None
    text = flattened(path)

    if "capture" in item:
        hits = re.findall(item["capture"], text)
        # Exactly one, and this is not pedantry — it is the first thing this check caught, in
        # itself. `priceInr:\s*(\d+)` matched the free plan's 0 before it reached ePixC+'s 199,
        # and the row rendered "₹0/month" and reported itself VERIFIED. An evidence pointer that
        # resolves to the wrong symbol is worse than no pointer, because it carries the authority
        # of having been checked. So an ambiguous capture is a failure, not a first match.
        if not hits:
            return False, "the constant this row reads is no longer there", None
        if len(hits) > 1:
            return False, (f"ambiguous — this pattern matches {len(hits)} places, so which "
                           f"constant the row reads is decided by file order"), None
        return True, None, (item.get("name", item["file"]), hits[0])

    found = re.search(item["pattern"], text) is not None
    want = item["must"] == "present"
    if found is want:
        return True, None, None
    return False, ("the symbol this row rests on is gone" if want
                   else "the thing this row says is absent is present"), None


def evaluate():
    """Every claim, with what the evidence says right now."""
    out = []
    for c in CLAIMS:
        has_evidence = bool(c.get("evidence"))
        if has_evidence == bool(c.get("unverifiable")):
            out.append({**c, "state": "malformed", "detail":
                        "a claim carries evidence or a stated reason it cannot, never both "
                        "and never neither"})
            continue
        if not has_evidence:
            out.append({**c, "state": "unverifiable", "captures": {}})
            continue
        captures, failures = {}, []
        for item in c["evidence"]:
            ok, detail, cap = check_one(item)
            if cap:
                captures[cap[0]] = cap[1]
            if not ok:
                failures.append(f"{item['file']}: {detail}")
        out.append({**c, "captures": captures,
                    "state": "failed" if failures else "verified",
                    "detail": "; ".join(failures) or None})
    return out


def rendered(result):
    """The row's sentence with any captured values substituted in.

    This is the half that makes the whole thing worth building. A row that prints a figure
    is a fifth copy of that figure; a row that names the constant and has the value put in
    at render time cannot disagree with the code, because it never held a number of its own.
    """
    try:
        return result["row"].format(**result.get("captures", {}))
    except KeyError as e:
        return f"{result['row']}  [no capture named {e}]"


def engine_digest():
    """SHA-256 of this file's engine region, for the vault generator's drift check."""
    text = pathlib.Path(__file__).read_text()
    body = text.split("# ─── ENGINE BEGIN")[1].split("# ─── ENGINE END")[0]
    return hashlib.sha256(body.encode()).hexdigest()[:16]


def main():
    results = evaluate()
    if "--json" in sys.argv:
        print(json.dumps({"repo": REPO, "engine": engine_digest(),
                          "claims": [{**r, "rendered": rendered(r)} for r in results]}, indent=1))
        return 0

    bad = [r for r in results if r["state"] in ("failed", "malformed")]
    counts = {s: sum(1 for r in results if r["state"] == s)
              for s in ("verified", "unverifiable", "failed", "malformed")}
    print(f"{REPO}: {counts['verified']} verified, {counts['unverifiable']} unverifiable, "
          f"{counts['failed']} failed, {counts['malformed']} malformed "
          f"(engine {engine_digest()})")
    for r in results:
        mark = {"verified": "ok  ", "unverifiable": "—   ", "failed": "FAIL", "malformed": "BAD "}
        print(f"  {mark[r['state']]} {r['id']}: {rendered(r)}")
        if r.get("detail"):
            print(f"       {r['detail']}")
    if bad:
        print("\nA row above no longer matches the code it points at. Either the claim stopped\n"
              "being true — fix the claim — or the evidence moved, in which case repoint it.\n"
              "Do not delete the row to make this pass.")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
# ─── ENGINE END ──────────────────────────────────────────────────────────────────────────
