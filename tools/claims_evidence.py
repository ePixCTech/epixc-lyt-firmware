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

REPO = "epixc-lyt-firmware"

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
        # Pinned because the FAQ's security answer depends on it, in another repository.
        #
        # Until 2026-09-05 that answer told a customer to "treat a controller as an unauthenticated
        # device". Gating the LAN API made the sentence false, and NOTHING would have noticed - the
        # firmware has no idea the website exists. It was caught by a person reading, which is the
        # weakest mechanism this project has. See Writing Checks, rule 3.
        #
        # A claim row is the one thing that catches this class automatically: the Alexa row went red
        # by itself the moment `va.put("alexa", true)` disappeared (D296), because it rested on a
        # symbol rather than on somebody's memory. So the new disclosure gets the same treatment.
        #
        # Both directions, deliberately. `pixcLanAuthorised` present says the gate exists; the
        # `-D PIXC_LAN_AUTH` flag present says it is COMPILED, which is the half D297 proved a green
        # build says nothing about. Either one alone can be true while the device is wide open.
        "id": "lan-api-needs-the-device-pin",
        "section": "Protocols",
        "ticket": "33, D295, D297, D300",
        "row": "**The controller's own JSON API refuses an unauthorised caller** — a state write, "
               "the legacy `/win` query API and every `GET /json` (including `cfg`) need the "
               "settings PIN the app writes during pairing. The realtime pixel inputs above stay "
               "open because none of those protocols can carry a credential.",
        "evidence": [
            {"file": "wled00/util.cpp", "must": "present",
             "pattern": r"bool pixcLanAuthorised\(\)"},
            # THE ENGINE FLATTENS WHITESPACE before matching (`flattened()`, a few lines up:
            # `re.sub(r"\s+", " ", text)`), so this file arrives as one long line. Two patterns
            # were written and both went red on a correct build before that was read: `^...$`
            # anchors, which need `re.M` the engine does not pass, and then a leading `\n`, which
            # cannot exist after flattening. The lookbehind is the version that works on the text
            # the engine actually sees.
            #
            # It still has to reject a DISABLED flag, which is the whole point - D297 planted
            # exactly that (`;   -D PIXC_LAN_AUTH`) and the firmware built green with the LAN API
            # wide open. Flattening turns that into `; -D PIXC_LAN_AUTH`, so refusing a `;` or `#`
            # immediately before is enough, and it is checked in both directions.
            {"file": "platformio_override.ini", "must": "present",
             "pattern": r"(?<![;#] )-D PIXC_LAN_AUTH\b"},
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


# The digest the region above is REQUIRED to have, checked by this script in its own repository's
# CI. Written 2026-08-29 after the drift it exists to catch had already happened.
#
# The cross-repository check lives in the vault (99-Wayfinder/claims/render.py), which is the only
# place that can see all three copies at once - and it is hand-run. So when epixc-web's copy drifted
# by three characters, nothing failed: each repository's CI went on passing, the vault generator
# refused to render, and the public Claims Checklist silently froze on stale pricing rows for three
# days. A check that only one person running one script by hand can perform is a check that reports
# nothing on the day it matters.
#
# Pinning the digest moves the FIRST detection into every repository's own CI, where a diff that
# touches this region fails the build that carries it, without any repository needing to see
# another. It cannot replace the vault's check - a pin says "this copy is what it was", not "the
# three copies agree" - so both exist, and the pin is the fast one.
#
# It lives BELOW engine_digest() on purpose. The hashed region ends at the FIRST occurrence of the
# end marker, which is the string literal inside engine_digest itself, so this constant is inside
# the shared region but outside the hash. Putting it above would make the pin cover itself, and no
# value would ever be correct.
#
# EDITING THE ENGINE: change it in all three copies in one commit, run any copy to read the new
# digest out of the failure message, and update this line in all three. If that feels like friction,
# it is the friction the header at the top of the region already asks for.
ENGINE_PIN = "a784e1bf69b43713"


def engine_pin_ok():
    """None if this copy matches the pin, or the sentence explaining what to do about it."""
    actual = engine_digest()
    if actual == ENGINE_PIN:
        return None
    return (f"the engine region has drifted: this copy hashes {actual}, the pin says {ENGINE_PIN}.\n"
            "Either an edit to the shared region was not made in all three repositories, or it was\n"
            "and the pin was not updated. Do not change the pin to make this pass without checking\n"
            "the other copies first - a pin updated alone hides exactly the drift it is here for.")


def main():
    drift = engine_pin_ok()
    if drift is not None:
        print(f"{REPO}: {drift}", file=sys.stderr)
        return 1

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
