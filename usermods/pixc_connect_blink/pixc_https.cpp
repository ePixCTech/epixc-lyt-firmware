// The TLS half of provisioning and of the OTA download, deliberately alone in this file.
//
// It must NOT include `wled.h`: that pulls in ESPAsyncWebServer, whose `WebRequestMethod` enum
// declares HTTP_GET/HTTP_POST/HTTP_PATCH, and ESP-IDF's `esp_http_client.h` pulls in
// `http_parser.h`, which declares the same names. Either header is fine on its own; together they
// do not compile.

#include "pixc_https.h"
#include "pixc_roots.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "epixc";

// Every TLS request this firmware makes is configured here, in one function, so the trust decision
// cannot drift between the provisioning fetch and the firmware download. Both are gated on the same
// two pinned roots, and that is not a coincidence to be tidied away later — see the note on
// blast radius in pixc_connect_blink.cpp's runOta().
static void pixcHttpsConfigure(esp_http_client_config_t& cfg, const char* url, int timeoutMs) {
  cfg.url = url;
  // The one line that makes this worth doing. Without `cert_pem` the client completes a handshake
  // with any certificate, so an https:// URL on its own proves nothing at all.
  //
  // Two roots, pinned at the root rather than the leaf: ISRG Root X1 for a Caddy-terminated
  // Let's Encrypt certificate, GTS Root R4 for Cloudflare's edge. Which one a device meets depends
  // on whether the DNS record is proxied — see pixc_roots.h. Flash is memory-mapped on ESP32, so
  // the PROGMEM array is directly usable here with no copy to RAM.
  cfg.cert_pem = PIXC_TRUSTED_ROOTS;
  cfg.timeout_ms = timeoutMs;
  // A redirect is another host answering the question this request exists to ask safely — which
  // broker to trust, or which bytes to flash. Following one silently would move the trust decision
  // to whoever controls the Location header.
  cfg.disable_auto_redirect = true;
  cfg.method = HTTP_METHOD_GET;
}

int pixcHttpsGet(const char* url, char* out, size_t cap) {
  if (url == nullptr || out == nullptr || cap < 2) return -1;

  esp_http_client_config_t cfg = {};
  pixcHttpsConfigure(cfg, url, 8000);

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return -1;

  int len = -1;
  const esp_err_t err = esp_http_client_open(c, 0);
  if (err != ESP_OK) {
    // Overwhelmingly a certificate that did not chain to the pinned root, or an unreachable host.
    // Named explicitly because the bare error code has sent people looking at Wi-Fi for hours.
    ESP_LOGW(TAG, "provisioning refused: TLS failed (%s) - certificate did not chain to either "
                  "pinned root, or the host is unreachable", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return -1;
  }

  esp_http_client_fetch_headers(c);
  const int status = esp_http_client_get_status_code(c);
  if (status == 200) {
    const int n = esp_http_client_read(c, out, cap - 1);
    if (n > 0) {
      out[n] = 0;
      len = n;
    }
  } else {
    ESP_LOGW(TAG, "provisioning failed: HTTP %d", status);
  }

  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return len;
}

// ---------------------------------------------------------------------------------------------
// Streaming GET
// ---------------------------------------------------------------------------------------------

struct PixcHttpsStream {
  esp_http_client_handle_t c;
};

PixcHttpsStream* pixcHttpsOpen(const char* url, int* outStatus, int* outLen, int timeoutMs) {
  if (outStatus != nullptr) *outStatus = 0;
  if (outLen != nullptr) *outLen = -1;
  if (url == nullptr) return nullptr;

  esp_http_client_config_t cfg = {};
  pixcHttpsConfigure(cfg, url, timeoutMs);

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return nullptr;

  const esp_err_t err = esp_http_client_open(c, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "download refused: TLS failed (%s) - certificate did not chain to either "
                  "pinned root, or the host is unreachable", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return nullptr;
  }

  const int contentLen = esp_http_client_fetch_headers(c);
  const int status = esp_http_client_get_status_code(c);
  if (outStatus != nullptr) *outStatus = status;
  if (outLen != nullptr) *outLen = contentLen;

  if (status != 200) {
    ESP_LOGW(TAG, "download failed: HTTP %d", status);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return nullptr;
  }

  PixcHttpsStream* s = (PixcHttpsStream*)malloc(sizeof(PixcHttpsStream));
  if (s == nullptr) {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return nullptr;
  }
  s->c = c;
  return s;
}

int pixcHttpsRead(PixcHttpsStream* s, uint8_t* buf, size_t cap) {
  if (s == nullptr || buf == nullptr || cap == 0) return -1;
  // esp_http_client_read() fills the buffer or stops at the end of the body, whichever comes
  // first, and returns <= 0 once there is nothing more to read. A read that ends early because the
  // connection dropped is indistinguishable here from a clean finish — which is what
  // pixcHttpsComplete() is for.
  const int n = esp_http_client_read(s->c, (char*)buf, (int)cap);
  return n < 0 ? -1 : n;
}

bool pixcHttpsComplete(PixcHttpsStream* s) {
  return s != nullptr && esp_http_client_is_complete_data_received(s->c);
}

void pixcHttpsClose(PixcHttpsStream* s) {
  if (s == nullptr) return;
  esp_http_client_close(s->c);
  esp_http_client_cleanup(s->c);
  free(s);
}

int pixcHttpsGetBinary(const char* url, uint8_t* out, size_t cap, int timeoutMs) {
  if (out == nullptr || cap == 0) return -1;

  int contentLen = -1;
  PixcHttpsStream* s = pixcHttpsOpen(url, nullptr, &contentLen, timeoutMs);
  if (s == nullptr) return -1;

  // Refuse rather than truncate. A signature cut to fit a buffer verifies against nothing, so the
  // only thing truncation could buy is a confusing failure later instead of a clear one here.
  if (contentLen > (int)cap) {
    ESP_LOGW(TAG, "signature too large: %d bytes, buffer is %u", contentLen, (unsigned)cap);
    pixcHttpsClose(s);
    return -1;
  }

  size_t got = 0;
  while (got < cap) {
    const int n = pixcHttpsRead(s, out + got, cap - got);
    if (n < 0) { pixcHttpsClose(s); return -1; }
    if (n == 0) break;
    got += (size_t)n;
  }

  const bool complete = pixcHttpsComplete(s);
  pixcHttpsClose(s);

  if (!complete || got == 0) return -1;
  if (contentLen >= 0 && got != (size_t)contentLen) return -1;
  return (int)got;
}
