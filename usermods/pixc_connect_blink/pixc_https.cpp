// The TLS half of provisioning, deliberately alone in this file.
//
// It must NOT include `wled.h`: that pulls in ESPAsyncWebServer, whose `WebRequestMethod` enum
// declares HTTP_GET/HTTP_POST/HTTP_PATCH, and ESP-IDF's `esp_http_client.h` pulls in
// `http_parser.h`, which declares the same names. Either header is fine on its own; together they
// do not compile.

#include "pixc_https.h"
#include "pixc_roots.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "epixc";

int pixcHttpsGet(const char* url, char* out, size_t cap) {
  if (url == nullptr || out == nullptr || cap < 2) return -1;

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  // The one line that makes this worth doing. Without `cert_pem` the client completes a handshake
  // with any certificate, so an https:// URL on its own proves nothing at all.
  //
  // Two roots, pinned at the root rather than the leaf: ISRG Root X1 for a Caddy-terminated
  // Let's Encrypt certificate, GTS Root R4 for Cloudflare's edge. Which one a device meets depends
  // on whether the DNS record is proxied — see pixc_roots.h. Flash is memory-mapped on ESP32, so
  // the PROGMEM array is directly usable here with no copy to RAM.
  cfg.cert_pem = PIXC_TRUSTED_ROOTS;
  cfg.timeout_ms = 8000;
  // A redirect is another host answering the question of which broker to trust, which is the
  // question this whole path exists to answer safely.
  cfg.disable_auto_redirect = true;
  cfg.method = HTTP_METHOD_GET;

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
