#pragma once

#include <stddef.h>
#include <stdint.h>

/// GET [url] over TLS, verifying the server against the two pinned roots and nothing else.
///
/// Returns the number of body bytes written to [out], or -1 on any failure — a failed handshake, a
/// certificate that does not chain to the pinned root, a non-200 status, or an empty body.
///
/// **There is deliberately no plaintext fallback.** An attacker who can break a TLS connection can
/// break it by refusing it, so a fallback hands the plaintext path back to exactly the person the
/// TLS was protecting against.
///
/// Declared here, in a header that pulls in nothing, because the ESP-IDF HTTP client's own header
/// drags in `http_parser.h`, whose method enum (`HTTP_GET`, `HTTP_PATCH`, …) collides with
/// `ESPAsyncWebServer`'s `WebRequestMethod`. Both are unavoidable in a WLED usermod, so they are
/// kept in separate translation units instead.
int pixcHttpsGet(const char* url, char* out, size_t cap);

// ---------------------------------------------------------------------------------------------
// Streaming GET, for bodies too large to buffer.
//
// A firmware image is ~1.8 MB against ~300 KB of usable heap, so the OTA download cannot use the
// buffered call above — it has to be consumed a chunk at a time, hashed and written to flash as it
// arrives. That is the only reason this second API exists; the trust decision is identical and is
// made in exactly one place inside the .cpp.
//
// Opaque by design: the handle is an `esp_http_client_handle_t`, and naming that type here would
// pull `esp_http_client.h` into every file that wants to download something — which is precisely
// the header collision this file exists to avoid.
// ---------------------------------------------------------------------------------------------
struct PixcHttpsStream;

/// Open [url] over TLS against the pinned roots and read the response headers.
///
/// Returns a handle on HTTP 200, or nullptr on any failure. [outStatus] receives the HTTP status
/// (0 if the connection never got far enough to have one) and [outLen] the Content-Length, or -1
/// when the response is chunked. Both may be nullptr.
///
/// A non-200 response is a failure and closes the connection: there is no body worth reading from
/// a 404 or a 500, and returning one invites a caller to flash it.
///
/// The caller MUST call pixcHttpsClose() on a non-null result.
PixcHttpsStream* pixcHttpsOpen(const char* url, int* outStatus, int* outLen, int timeoutMs);

/// Read up to [cap] bytes of body. Returns the byte count, 0 at end of body, or -1 on error.
int pixcHttpsRead(PixcHttpsStream* s, uint8_t* buf, size_t cap);

/// True when the whole body arrived — i.e. Content-Length was satisfied, or the final chunk of a
/// chunked response was seen. A TLS connection cut mid-body ends a read loop exactly like a clean
/// finish, so this is what separates "downloaded" from "stopped early".
bool pixcHttpsComplete(PixcHttpsStream* s);

/// Close and free. Safe on nullptr.
void pixcHttpsClose(PixcHttpsStream* s);

/// GET a small binary body whole — the detached OTA signature, ~70 bytes of DER.
///
/// Distinct from pixcHttpsGet() because that one NUL-terminates and returns text; a DER signature
/// contains NUL bytes and must be returned by length. Fails rather than truncates if the body does
/// not fit in [cap]. Returns the byte count, or -1.
int pixcHttpsGetBinary(const char* url, uint8_t* out, size_t cap, int timeoutMs);
