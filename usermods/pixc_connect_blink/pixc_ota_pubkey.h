#pragma once

// The public half of the ePixC firmware signing key, as PEM.
//
// The device refuses to commit an OTA image unless the signature at `sig_url` verifies against
// this key. **An empty key means every update is refused** — deliberately. An OTA path that
// applies whatever the broker hands it is a fleet-wide compromise, and a build that silently
// skipped the check would be worse than one that has no OTA at all, because it would look
// protected.
//
// ECDSA P-256 over SHA-256, not Ed25519: this chip's mbedTLS has no EdDSA at all — `nm -g
// libmbedcrypto.a` returns zero Ed25519 symbols, while ECDSA, SHA-256 and X.509 key parsing are
// all present and the S3 accelerates SHA in hardware. Ed25519 would have meant bundling
// third-party crypto into the one code path where a bug loses the whole fleet.
//
// Generate the pair once, offline, and keep the private half off this machine:
//
//   openssl ecparam -genkey -name prime256v1 -noout -out ota.pem
//   openssl pkey -in ota.pem -pubout -outform PEM          # paste the result below
//   openssl pkcs8 -topk8 -nocrypt -in ota.pem -outform DER | base64   # ADMIN_SIGNING_KEY_B64
//   openssl pkey -in ota.pem -pubout -outform DER | base64            # ADMIN_SIGNING_PUBKEY_B64
//
// Losing the private half means never being able to sign an update these devices will accept.

#define PIXC_OTA_PUBKEY_PEM ""

// -----------------------------------------------------------------------------
// Build-time guard. The runtime check in verifySignature() is fail-closed and stays that way —
// this is about *when* you find out.
//
// Without this, an empty key builds cleanly, flashes cleanly, boots cleanly, and only reveals
// itself as "no signing key in firmware" on a device in somebody's home, after the image has been
// built, published and installed. The signing key is the one secret whose loss is unrecoverable,
// so the moment to notice it is missing is the moment someone tries to produce a shippable image.
//
// The checks are on the macro's own text, not on a separate "is it set" flag, so they cannot go
// stale: paste a key and they pass, remove it and they fail again. Nobody has to remember to
// delete anything.
//
// The deliberate way out is -D EPIXC_OTA_UNSIGNED_DEV_BUILD (see PixC_V1_dev in
// platformio_override.ini). It suppresses only this guard — the device still refuses every OTA,
// and an image built that way says so at boot and in the version it reports.
// -----------------------------------------------------------------------------
#ifndef EPIXC_OTA_UNSIGNED_DEV_BUILD

namespace pixc_ota_key_guard {
  // constexpr rather than strncmp: this has to be answerable by the compiler, not the device.
  constexpr bool startsWith(const char* s, const char* prefix) {
    return *prefix == '\0' ? true
         : (*s != *prefix ? false : startsWith(s + 1, prefix + 1));
  }
}

static_assert(sizeof(PIXC_OTA_PUBKEY_PEM) > 1,
  "PIXC_OTA_PUBKEY_PEM is empty: this image could never accept an OTA update, and would only "
  "say so once it was installed on a device. Paste the PEM public half of the ePixC signing key "
  "into usermods/pixc_connect_blink/pixc_ota_pubkey.h (the founder holds the private half; see "
  "epixc-backend/deploy/.env.example -> ADMIN_SIGNING_KEY_B64). For a bench build that is not "
  "meant to take updates, build PixC_V1_dev or add -D EPIXC_OTA_UNSIGNED_DEV_BUILD.");

// The two checks below tolerate an empty key so that removing it produces the one error above,
// not three at once.
static_assert(sizeof(PIXC_OTA_PUBKEY_PEM) <= 1 ||
              pixc_ota_key_guard::startsWith(PIXC_OTA_PUBKEY_PEM, "-----BEGIN PUBLIC KEY-----"),
  "PIXC_OTA_PUBKEY_PEM is not an X.509 SubjectPublicKeyInfo PEM. mbedtls_pk_parse_public_key "
  "reads exactly that, and anything else fails as 'bad signing key' on the device rather than "
  "here. Use: openssl pkey -in ota.pem -pubout -outform PEM. A private key, a bare base64 DER "
  "blob, or a certificate will all land here.");

// A P-256 SubjectPublicKeyInfo is 91 bytes of DER -> 124 base64 chars -> a PEM of 178 bytes with
// the header, footer and newlines. Shorter than this is a truncated paste; much longer is a
// different (bigger) curve, which mbedTLS would accept and the signer could not match.
static_assert(sizeof(PIXC_OTA_PUBKEY_PEM) <= 1 ||
              (sizeof(PIXC_OTA_PUBKEY_PEM) >= 170 && sizeof(PIXC_OTA_PUBKEY_PEM) <= 220),
  "PIXC_OTA_PUBKEY_PEM is not the size of a P-256 public key PEM (~178 bytes). Either the paste "
  "is truncated, or this is not prime256v1 — and the curve is not negotiable: it is what the "
  "admin service signs with and what mbedTLS on the S3 can verify.");

#endif  // EPIXC_OTA_UNSIGNED_DEV_BUILD

// -----------------------------------------------------------------------------
// When the key exists, it looks like this — one string, trailing newline included,
// because mbedtls_pk_parse_public_key wants the PEM terminated and NUL-counted:
//
// #define PIXC_OTA_PUBKEY_PEM \
//   "-----BEGIN PUBLIC KEY-----\n" \
//   "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE...\n" \
//   "-----END PUBLIC KEY-----\n"
// -----------------------------------------------------------------------------
