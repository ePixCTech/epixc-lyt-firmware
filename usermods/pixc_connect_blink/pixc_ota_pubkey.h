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
// When the key exists, it looks like this — one string, trailing newline included,
// because mbedtls_pk_parse_public_key wants the PEM terminated and NUL-counted:
//
// #define PIXC_OTA_PUBKEY_PEM \
//   "-----BEGIN PUBLIC KEY-----\n" \
//   "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE...\n" \
//   "-----END PUBLIC KEY-----\n"
// -----------------------------------------------------------------------------
