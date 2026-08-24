#pragma once

#include <stddef.h>

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
