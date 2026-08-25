#include "wled.h"
#include <HTTPClient.h>
#include <Update.h>
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
// The public half of the firmware signing key. Empty by default, which refuses every update.
#include "pixc_ota_pubkey.h"
// esp_reset_reason(). Reached through Arduino's headers on ESP32 anyway, but named here because
// this file uses it directly and an implicit include is a trap when the framework moves.
#include <esp_system.h>
// esp_ota_get_state_partition() / esp_ota_mark_app_valid_cancel_rollback(): the app half of
// bootloader rollback. See _confirmImageIfPending().
#include <esp_ota_ops.h>
// The TLS fetch lives in its own translation unit: ESP-IDF's HTTP client header and
// ESPAsyncWebServer both declare HTTP_GET/HTTP_PATCH, and a WLED usermod needs both. See
// pixc_https.cpp for why Arduino's WiFiClientSecure is not an option here.
#include "pixc_https.h"

// ePixC API host the device calls to learn its MQTT broker (see provision()).
// Normally written by the app during pairing (um.PixcConnect.apiHost); this is
// only the fallback for a unit that never completed setup. The broker itself is
// never hardcoded — it is always fetched from this API.
//
// The default used to be "api.pixc.app", a domain ePixC does not own. Anyone who
// registered it would have been asked, by every un-paired unit, which MQTT broker
// to connect to. The real host is api.epixc.in (see deploy/caddy/Caddyfile).
//
// The dev LAN address lives in the PixC_V1_dev build env, not here, so it cannot
// reach a release build by being the value someone forgot to change back.
#ifndef PIXC_API_HOST
  #define PIXC_API_HOST "api.epixc.in"
#endif
#ifndef PIXC_API_PORT
  #define PIXC_API_PORT 443
#endif
// Last-resort broker fallback ONLY if the API can't be reached at all.
#ifndef PIXC_MQTT_HOST
  #define PIXC_MQTT_HOST ""
#endif
#ifndef PIXC_MQTT_PORT
  #define PIXC_MQTT_PORT 1883
#endif

// PixC device-edge usermod. Two jobs:
//
//  1. MQTT bridge to the PixC cloud (pixc-mqtt broker). WLED's native MQTT
//     topics/payloads don't match the PixC contract, so this usermod:
//       - forces the device topic to `pixc/d/{mac}` (full MAC, lowercase) so
//         WLED subscribes `pixc/d/{mac}/api` for commands;
//       - publishes `announce` (once on connect), `state` and `health` to
//         `pixc/d/{mac}/{kind}` in the shapes pixc-mqtt expects.
//
//  2. On the first successful MQTT (cloud) connect after Wi-Fi is up — i.e.
//     right after initial setup / pairing — flash the strip solid green for
//     3 seconds, then restore the previous color / brightness / effect.
//
// Requires MQTT compiled in (do NOT set WLED_DISABLE_MQTT).
class PixcConnectBlink : public Usermod {
  private:
    bool _blinkDone = false;   // green blink only on the first connect
    bool _flashing = false;
    bool _announced = false;
    unsigned long _start = 0;
    unsigned long _lastState = 0;
    unsigned long _lastHealth = 0;
    unsigned long _lastPower = 0;
    bool _statePending = false;      // set by onStateChange, drained in loop()
    // Last seen realtimeMode, so a stream starting or stopping publishes immediately instead of
    // waiting for the 5 s timer. WLED does not call onStateChange for realtime lock: the segment
    // config never changes, only what is being drawn over it.
    uint8_t _lastRealtime = 0;
    // Whether the boot event has gone out yet. One per power cycle, not per MQTT reconnect:
    // "this unit restarted" and "this unit lost the broker for a moment" are different facts and
    // support needs to tell them apart.
    bool _bootReported = false;

    // True when this boot is running an image the bootloader has marked PENDING_VERIFY — i.e. a
    // freshly installed OTA that will be rolled back on the next boot unless this firmware says it
    // is working. False on an ordinary boot, and also on a build whose bootloader does not have
    // rollback compiled in, which is why it is reported rather than assumed.
    bool _awaitingConfirm = false;
    bool _confirmed = false;
    byte _savedCol[4] = {0, 0, 0, 0};
    byte _savedBri = 0;
    uint8_t _savedFx = 0;

    // PixC API host (provisioned by the app, persisted in usermod config). The
    // device calls GET {apiHost}:{apiPort}/api/v1/provision?mac=... to learn the
    // MQTT broker, then hands off to MQTT.
    String _apiHost = PIXC_API_HOST;
    uint16_t _apiPort = PIXC_API_PORT;
    bool _provisioned = false;          // got the broker from the API yet?
    unsigned long _lastProvisionTry = 0;

    // OTA (cloud-triggered, MQTT-pull). The trigger arrives on `pixc/d/{mac}/ota`;
    // the actual HTTP download + flash is deferred to loop() so it never blocks
    // the MQTT receive callback. Progress is reported on `.../ota/progress`.
    bool _otaPending = false;
    String _otaJob, _otaUrl, _otaSha, _otaVer, _otaSigUrl;

    static constexpr unsigned long kStateIntervalMs  = 5000;
    static constexpr unsigned long kHealthIntervalMs = 30000;
    static constexpr unsigned long kPowerIntervalMs  = 30000;
    static constexpr unsigned long kProvisionRetryMs = 10000;
    // A state change can be a slider being dragged. Without a floor, one drag is a
    // publish per frame; with it, the cloud still sees the change within 250 ms.
    static constexpr unsigned long kStateChangeMinMs = 250;

    // Ask the ePixC API which broker to use, then point WLED's MQTT at it. The broker is dynamic
    // (the bench Mac's address rotates, and production can move it without reflashing), so it is
    // never hardcoded — always fetched.
    //
    // **This call decides which MQTT broker the device trusts**, which makes it the most
    // security-sensitive request the firmware makes: whoever answers it names the broker, and a
    // wrong answer bypasses both the per-device credential and MQTT TLS at once. Ticket 33.
    //
    // Production therefore goes over **HTTPS with a pinned ISRG Root X1** and refuses on a failed
    // chain. It never falls back to plaintext — a fallback would mean an attacker who can break
    // the TLS connection (trivially, by refusing it) gets the plaintext path back, which is the
    // same as having no TLS at all.
    //
    // Port 80/8080 stays plaintext for bench work. That is deliberate and safe in the shipped
    // build because `default_envs` names only `PixC_V1`, whose compiled default is
    // `api.epixc.in:443`; the LAN address lives in `PixC_V1_dev`, which a release build cannot
    // reach.
    void fetchProvisionConfig() {
      if (WiFi.status() != WL_CONNECTED || _apiHost.length() == 0) return;

      const bool useTls = (_apiPort == 443);

      // A TLS handshake needs roughly 40 KB of heap for the record buffers and the certificate
      // chain. Attempting one below that does not fail cleanly — it fails somewhere inside
      // mbedTLS. Better to skip and retry in ten seconds, by which time whatever ate the heap may
      // have finished.
      if (useTls && ESP.getFreeHeap() < 50000) {
        DEBUG_PRINTF("[ePixC] provisioning deferred: %u bytes free, need ~50k for TLS\n",
                     (unsigned)ESP.getFreeHeap());
        return;
      }

      char url[200];
      snprintf(url, sizeof(url), "%s://%s:%u/api/v1/provision?mac=%s",
               useTls ? "https" : "http", _apiHost.c_str(), _apiPort, escapedMac.c_str());

      char body[768];
      const int len = useTls ? pixcHttpsGet(url, body, sizeof(body))
                             : httpGet(url, body, sizeof(body));
      if (len <= 0) return;

      DynamicJsonDocument doc(640);
      if (deserializeJson(doc, body, len)) return;
      JsonObject d = doc["data"].isNull() ? doc.as<JsonObject>() : doc["data"].as<JsonObject>();
      // API serializes snake_case (mqtt_host/mqtt_port); accept camelCase too.
      const char* host = d["mqtt_host"] | (d["mqttHost"] | "");
      int port = d["mqtt_port"] | (d["mqttPort"] | PIXC_MQTT_PORT);
      if (host && strlen(host) > 0) {
        strlcpy(mqttServer, host, MQTT_MAX_SERVER_LEN + 1);
        mqttPort = port;
        mqttEnabled = true;
        _provisioned = true;
        DEBUG_PRINTF("[ePixC] broker from API over %s: %s:%d\n",
                     useTls ? "HTTPS" : "HTTP", mqttServer, mqttPort);
        // Force WLED to (re)connect MQTT to the new broker.
        if (mqtt != nullptr && mqtt->connected()) mqtt->disconnect();
        initMqtt();
      }
    }

    // Plaintext GET, bench only. Reached only when the port is not 443, which a release build
    // cannot arrange: `default_envs` names `PixC_V1`, whose compiled host is api.epixc.in:443.
    int httpGet(const char* url, char* out, size_t cap) {
      WiFiClient client;
      HTTPClient http;
      http.setConnectTimeout(4000);
      if (!http.begin(client, url)) return -1;
      int len = -1;
      if (http.GET() == 200) {
        String payload = http.getString();
        len = payload.length() < cap ? payload.length() : cap - 1;
        memcpy(out, payload.c_str(), len);
        out[len] = 0;
      }
      http.end();
      return len;
    }



    void publishKind(const char* kind, const char* payload) {
      if (!WLED_MQTT_CONNECTED) return;
      char topic[MQTT_MAX_TOPIC_LEN + 16];
      snprintf(topic, sizeof(topic), "%s/%s", mqttDeviceTopic, kind);
      mqtt->publish(topic, 0, false, payload);
    }

    void publishAnnounce() {
      char buf[160];
      // led_channels, NOT a chip name. hasWhiteChannel() only reveals the channel
      // count; it cannot tell a 5 V WS2812B from a 12 V WS2815. Publishing a guessed
      // chip name meant the device overwrote the strip model the app had set
      // accurately, destroying the one value the server needs to know the voltage.
      // The app owns led_chip; the device owns led_channels, which is what it knows.
      const char* ledChannels = strip.hasWhiteChannel() ? "RGBW" : "RGB";
      snprintf(buf, sizeof(buf),
        "{\"fw_version\":\"%s\",\"led_channels\":\"%s\",\"led_count\":%u}",
        versionString, ledChannels, strip.getLengthTotal());
      publishKind("announce", buf);
    }

    // Estimated current draw. AMPS ONLY — deliberately not watts.
    //
    // BusManager::currentMilliamps() is WLED's Automatic Brightness Limiter estimate
    // ("estimate used current from summed colors"); there is no shunt on the board, so
    // this is arithmetic over the framebuffer, not a measurement. Voltage is NOT applied
    // here: the strip could be 5 V, 12 V or 24 V, the device cannot tell, and a
    // hardcoded 5.0 made reported power wrong by up to 4.8x. The server multiplies by
    // the voltage implied by led_chip, so a wrong voltage is a config change instead of
    // a fleet reflash.
    void publishPower() {
      char buf[96];
      snprintf(buf, sizeof(buf), "{\"amps\":%.3f,\"estimated\":true}",
               BusManager::currentMilliamps() / 1000.0f);
      publishKind("power", buf);
    }

    // WLED's name for whatever is streaming, matching the strings its own UI shows so the app and
    // the device's web page never disagree about what took the strip over.
    static const char* realtimeSourceName(uint8_t mode) {
      switch (mode) {
        case REALTIME_MODE_UDP:      return "UDP";
        case REALTIME_MODE_HYPERION: return "Hyperion";
        case REALTIME_MODE_E131:     return "E1.31";
        case REALTIME_MODE_ADALIGHT: return "USB Adalight/TPM2";
        case REALTIME_MODE_ARTNET:   return "Art-Net";
        case REALTIME_MODE_TPM2NET:  return "tpm2.net";
        case REALTIME_MODE_DDP:      return "DDP";
        case REALTIME_MODE_DMX:      return "DMX";
        default:                     return "";
      }
    }

    void publishState() {
      char buf[288];
      Segment& seg = strip.getSegment(strip.getMainSegmentId());
      // `live` is not decoration, and it is not the same question as `on`.
      //
      // While something streams over DDP or E1.31, WLED holds a realtimeLock and draws that
      // instead of the segment. Everything else in this payload — bri, col, fx — is the
      // *configured* state, which is no longer what is on the strip. Without this flag the cloud
      // cannot tell the difference, so the app confidently showed the last colour it set while a
      // PC played video on the wall, and a brightness drag appeared to work and changed nothing
      // anyone could see. Ticket 37.
      //
      // `lor` (WLED's live override) is reported too, because it decides who wins: with an
      // override set, cloud commands do take effect despite the stream.
      snprintf(buf, sizeof(buf),
        "{\"on\":%s,\"bri\":%u,\"seg\":[{\"col\":[[%u,%u,%u,%u]],\"fx\":%u,\"pal\":%u,\"sx\":%u,\"ix\":%u}]"
        ",\"live\":%s,\"live_source\":\"%s\",\"live_override\":%u}",
        (bri > 0 ? "true" : "false"), bri,
        colPri[0], colPri[1], colPri[2], colPri[3], effectCurrent,
        seg.palette, seg.speed, seg.intensity,
        (realtimeMode ? "true" : "false"), realtimeSourceName(realtimeMode),
        (unsigned)realtimeOverride);
      publishKind("state", buf);
    }

    // Something worth writing down happened. Lands in `device_events` via the gateway and is read
    // back by the app's device history.
    //
    // The topic has been ingested since the broker cutover and **nothing has ever published it**,
    // so the table could only ever be empty. Two events to start, both answering the question
    // support actually gets when a customer says "it stopped working": did it restart, and did it
    // lose Wi-Fi.
    void publishEvent(const char* type, const char* extraJson = nullptr) {
      char buf[192];
      if (extraJson != nullptr) {
        snprintf(buf, sizeof(buf), "{\"type\":\"%s\",%s}", type, extraJson);
      } else {
        snprintf(buf, sizeof(buf), "{\"type\":\"%s\"}", type);
      }
      publishKind("event", buf);
    }

    // Why this boot happened, in esp_reset_reason()'s terms. A panic or a brownout loop is the
    // single most useful thing to know about a unit that "keeps going offline", and it is
    // indistinguishable from a Wi-Fi problem from the outside.
    static const char* resetReasonName() {
      switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power_on";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "int_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP:return "deep_sleep";
        case ESP_RST_EXT:      return "external";
        default:               return "unknown";
      }
    }

    void publishHealth() {
      char buf[256];
      String ssid = WiFi.SSID();
      String ip = WiFi.localIP().toString();
      int signal = constrain(2 * (WiFi.RSSI() + 100), 0, 100);
      snprintf(buf, sizeof(buf),
        "{\"rssi\":%d,\"signal\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"free_heap\":%u,\"uptime\":%lu,\"fw_version\":\"%s\"}",
        (int)WiFi.RSSI(), signal, ssid.c_str(), ip.c_str(),
        (unsigned)ESP.getFreeHeap(),
        (unsigned long)(millis() / 1000), versionString);
      publishKind("health", buf);
    }

    // Apply a WLED config fragment pushed by the cloud over `pixc/d/{mac}/cfg`
    // (power plan -> def.bri, restore-on-power -> def.on, slow-fade -> light.tr.dur).
    // Only native WLED cfg keys take effect; unknown keys are ignored safely.
    void applyCfg(const char* payload) {
      // This WLED fork uses ArduinoJson v6 — JsonDocument is abstract; use a
      // sized DynamicJsonDocument. The cfg fragment (def/light/pixc) is small.
      DynamicJsonDocument doc(2048);
      if (deserializeJson(doc, payload)) return;
      JsonObject root = doc.as<JsonObject>();
      deserializeConfig(root, false);
      serializeConfigToFS();
    }

    // Factory reset: wipe config + Wi-Fi credentials and reboot back into ePixC-AP.
    void factoryReset() {
      WLED_FS.remove("/cfg.json");
      WLED_FS.remove("/wsec.json");
      doReboot = true;
    }

    // Report OTA progress to the cloud on `pixc/d/{mac}/ota/progress`. `status`
    // is one of downloading/installing/done/failed (pixc-mqtt treats done|failed
    // as terminal). Failures carry the reason in `err`.
    void publishOtaProgress(const char* status, int percent, const char* err = nullptr) {
      if (!WLED_MQTT_CONNECTED) return;
      char buf[224];
      if (err && err[0]) {
        snprintf(buf, sizeof(buf), "{\"job_id\":\"%s\",\"status\":\"%s\",\"percent\":%d,\"error\":\"%s\"}",
                 _otaJob.c_str(), status, percent, err);
      } else {
        snprintf(buf, sizeof(buf), "{\"job_id\":\"%s\",\"status\":\"%s\",\"percent\":%d}",
                 _otaJob.c_str(), status, percent);
      }
      publishKind("ota/progress", buf);
    }

    static void hex32(const uint8_t* d, char* out /* >=65 */) {
      static const char* H = "0123456789abcdef";
      for (int i = 0; i < 32; i++) { out[i*2] = H[d[i] >> 4]; out[i*2+1] = H[d[i] & 0xF]; }
      out[64] = 0;
    }

    // Fetch the detached signature for the image being installed.
    //
    // Small — a P-256 signature is 70-72 bytes of DER — so it is read whole rather than streamed.
    // Fetched *after* the image so a signature cannot be swapped between the check and the flash:
    // both come from the same URLs the same command named.
    bool fetchSignature(uint8_t* out, size_t cap, size_t* outLen) {
      if (_otaSigUrl.length() == 0) return false;
      WiFiClient client;
      HTTPClient http;
      http.setConnectTimeout(8000);
      http.setTimeout(10000);
      if (!http.begin(client, _otaSigUrl)) return false;
      if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
      int len = http.getSize();
      if (len <= 0 || (size_t)len > cap) { http.end(); return false; }
      int got = http.getStreamPtr()->readBytes(out, (size_t)len);
      http.end();
      if (got != len) return false;
      *outLen = (size_t)len;
      return true;
    }

    // Verify a detached ECDSA P-256 signature over the image's SHA-256 digest.
    //
    // This is the check that makes an OTA safe to apply: the SHA-256 alone only proves the image
    // arrived intact from whoever sent it, and the digest travels in the same MQTT command as the
    // URL. The signature proves it came from the holder of the ePixC signing key, which no broker,
    // proxy or DNS answer can forge.
    //
    // Returns a failure reason, or nullptr when the image is trustworthy.
    const char* verifySignature(const uint8_t* digest) {
      if (sizeof(PIXC_OTA_PUBKEY_PEM) <= 1) return "no signing key in firmware";

      uint8_t sig[80];
      size_t sigLen = 0;
      if (!fetchSignature(sig, sizeof(sig), &sigLen)) return "signature fetch";

      mbedtls_pk_context pk;
      mbedtls_pk_init(&pk);
      // The length includes the terminating NUL: mbedtls treats a PEM as a NUL-counted buffer.
      int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)PIXC_OTA_PUBKEY_PEM,
                                           sizeof(PIXC_OTA_PUBKEY_PEM));
      if (rc != 0) { mbedtls_pk_free(&pk); return "bad signing key"; }

      rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig, sigLen);
      mbedtls_pk_free(&pk);
      return rc == 0 ? nullptr : "signature mismatch";
    }

    // Download the firmware image over HTTP and flash it to the inactive OTA
    // partition, verifying SHA-256 before committing the new image as bootable.
    // Streams in chunks so a ~1.8MB image fits in RAM. Blocking by design — runs
    // from loop(), and the hardware watchdog is disabled (WLED_WATCHDOG_TIMEOUT=0).
    void runOta() {
      if (WiFi.status() != WL_CONNECTED || _otaUrl.length() == 0) {
        publishOtaProgress("failed", 0, "no wifi");
        return;
      }
      DEBUG_PRINTF("[PixC] OTA start v%s <- %s\n", _otaVer.c_str(), _otaUrl.c_str());
      publishOtaProgress("downloading", 0);

      WiFiClient client;
      HTTPClient http;
      http.setConnectTimeout(8000);
      http.setTimeout(20000);
      if (!http.begin(client, _otaUrl)) { publishOtaProgress("failed", 0, "begin"); return; }
      int code = http.GET();
      if (code != HTTP_CODE_OK) {
        char e[24]; snprintf(e, sizeof(e), "http %d", code);
        publishOtaProgress("failed", 0, e); http.end(); return;
      }
      int total = http.getSize();                       // -1 if chunked/unknown
      if (!Update.begin(total > 0 ? (size_t)total : UPDATE_SIZE_UNKNOWN)) {
        publishOtaProgress("failed", 0, "no space"); http.end(); return;
      }

      mbedtls_sha256_context sha;
      mbedtls_sha256_init(&sha);
      mbedtls_sha256_starts(&sha, 0);                   // 0 = SHA-256 (not SHA-224)

      WiFiClient* stream = http.getStreamPtr();
      uint8_t buf[1024];
      size_t written = 0;
      int remaining = total;
      int lastPct = -1;
      unsigned long lastData = millis();
      bool ok = true;
      const char* failMsg = nullptr;

      while (http.connected() && (remaining > 0 || remaining < 0)) {
        size_t avail = stream->available();
        if (avail) {
          int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
          if (n <= 0) { yield(); continue; }
          if (Update.write(buf, n) != (size_t)n) { ok = false; failMsg = "flash write"; break; }
          mbedtls_sha256_update(&sha, buf, n);
          written += n;
          if (remaining > 0) {
            remaining -= n;
            int pct = (int)((written * 90ULL) / (size_t)total);   // 0..90 while downloading
            if (pct != lastPct && pct % 10 == 0) { publishOtaProgress("downloading", pct); lastPct = pct; }
          }
          lastData = millis();
        } else {
          if (millis() - lastData > 20000) { ok = false; failMsg = "stalled"; break; }
          delay(1);
        }
        if (remaining == 0) break;
        yield();
      }
      http.end();

      uint8_t digest[32];
      mbedtls_sha256_finish(&sha, digest);
      mbedtls_sha256_free(&sha);

      // Integrity first, then provenance. The digest is cheap and rules out a truncated download
      // before a signature check that costs a round trip.
      //
      // The digest is required now rather than checked only when present: it used to be skipped
      // entirely if the command carried no `sha256`, so a command with the field omitted flashed
      // whatever arrived.
      if (ok) {
        if (_otaSha.length() != 64) {
          ok = false;
          failMsg = "no sha256 in command";
        } else {
          char got[65]; hex32(digest, got);
          if (!_otaSha.equalsIgnoreCase(got)) { ok = false; failMsg = "sha256 mismatch"; }
        }
      }

      if (ok) {
        const char* sigFail = verifySignature(digest);
        if (sigFail != nullptr) { ok = false; failMsg = sigFail; }
      }

      if (!ok) {
        Update.abort();
        DEBUG_PRINTF("[PixC] OTA failed: %s\n", failMsg ? failMsg : "?");
        publishOtaProgress("failed", 0, failMsg);
        return;
      }

      publishOtaProgress("installing", 95);
      if (!Update.end(true)) {           // commit: set the new image bootable
        char e[40]; snprintf(e, sizeof(e), "finalize %u", Update.getError());
        publishOtaProgress("failed", 0, e);
        return;
      }

      DEBUG_PRINTLN("[PixC] OTA done; rebooting");
      publishOtaProgress("done", 100);
      delay(1200);                       // let the QoS0 progress publish flush
      doReboot = true;                   // WLED reboots from its own loop
    }

  public:
    void setup() override {
      // ePixC-AP is a recovery hotspot, not an always-on beacon: once the device
      // joins the home Wi-Fi the AP is torn down (WLED shuts it on connect for
      // any non-ALWAYS behaviour). It only (re)opens when the station can't
      // connect — after the ~30s grace below — so a mispaired/dropped device is
      // still reachable for re-provisioning.
      apBehavior = AP_BEHAVIOR_NO_CONN;
      mqttEnabled = true;

      // Is this boot on probation? Only true when the bootloader actually armed rollback, which
      // makes this a runtime fact rather than a claim about a prebuilt binary: the Tasmota
      // framework this fork used to build against had CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE off,
      // so the rollback API linked and did nothing.
      esp_ota_img_states_t otaState;
      const esp_partition_t* running = esp_ota_get_running_partition();
      if (running != nullptr && esp_ota_get_state_partition(running, &otaState) == ESP_OK) {
        _awaitingConfirm = (otaState == ESP_OTA_IMG_PENDING_VERIFY);
      }
      // Broker is NOT hardcoded — it is fetched from the PixC API (see
      // fetchProvisionConfig) on every Wi-Fi connect. Only seed the last-resort
      // fallback if a build-time broker was explicitly provided.
      if (strlen(PIXC_MQTT_HOST) > 0 && strlen(mqttServer) == 0) {
        strlcpy(mqttServer, PIXC_MQTT_HOST, MQTT_MAX_SERVER_LEN + 1);
        mqttPort = PIXC_MQTT_PORT;
      }
      // Force the ePixC topic scheme before MQTT connects so WLED subscribes
      // epixc/v1/d/{mac}/api and publishes its LWT under the same prefix.
      // "epixc/v1/d/" (11) + 12-char MAC = 23 chars, within MQTT_MAX_TOPIC_LEN (32).
      // The version lives in the TOPIC, not in the payload: the topic is already the
      // routing key, so the broker ACL, the subscriber and the version all move
      // together, and a device publishing every few seconds carries no extra field.
      snprintf(mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1, "epixc/v1/d/%s",
               escapedMac.c_str());

      // WLED core subscribes mqttGroupTopic plus its /col and /api children, and the
      // default is "wled/all" — a channel EVERY device joins, on which one compromised
      // device could command the entire fleet. Nothing in ePixC ever publishes there.
      // The broker ACL also denies wled/#, so this is the first of two layers.
      mqttGroupTopic[0] = 0;
    }

    // Called by WLED when the station connects to Wi-Fi — fetch the broker.
    void connected() override {
      _provisioned = false;
      _lastProvisionTry = 0;   // provision asap in loop()
    }

    // Persist the PixC API host/port so the app provisions it once and it
    // survives reboots (cfg.json -> um.PixcConnect.{apiHost,apiPort}).
    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject("PixcConnect");
      top["apiHost"] = _apiHost;
      top["apiPort"] = _apiPort;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root["PixcConnect"];
      if (top.isNull()) return false;
      _apiHost = top["apiHost"] | _apiHost;
      _apiPort = top["apiPort"] | _apiPort;
      return true;
    }

    // Tell the bootloader this image works, so it stops being a candidate for rollback.
    //
    // "Works" is defined as: it reached the broker. That is the whole point of the firmware — a
    // build that boots but cannot talk to the cloud is exactly the build that should be rolled
    // back, and it is the failure a bad OTA most plausibly produces. Confirming in setup(), which
    // is the obvious place, would confirm every image including one that never connects again.
    void confirmImageIfPending() {
      if (!_awaitingConfirm || _confirmed) return;
      _confirmed = true;
      if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        DEBUG_PRINTLN("[PixC] OTA image confirmed; rollback cancelled");
      } else {
        DEBUG_PRINTLN("[PixC] OTA image confirm FAILED; this image may be rolled back");
      }
    }

    void onMqttConnect(bool /*sessionPresent*/) override {
      _announced = false; // re-announce on every (re)connect

      // First broker connection since power-on is a boot; every later one is a reconnect. The
      // uptime is carried because it turns "it rebooted" into "it rebooted four minutes in",
      // which is the difference between a power problem and a firmware one.
      char extra[192];
      if (!_bootReported) {
        _bootReported = true;
        // `rollback_armed` says whether the bootloader put this image on probation. It is the only
        // honest way to know: the API links either way, and with the option off nothing ever
        // reverts. A fleet where this is false everywhere is a fleet with no rollback, whatever the
        // build was supposed to do.
        snprintf(extra, sizeof(extra),
                 "\"reason\":\"%s\",\"fw_version\":\"%s\",\"rollback_armed\":%s",
                 resetReasonName(), versionString, _awaitingConfirm ? "true" : "false");
        publishEvent("boot", extra);
      } else {
        snprintf(extra, sizeof(extra), "\"uptime\":%lu", (unsigned long)(millis() / 1000));
        publishEvent("reconnect", extra);
      }

      // Reaching the broker is the proof. Done after the event publish so the record of this boot
      // exists before the image stops being reversible.
      confirmImageIfPending();

      // Subscribe the PixC control subtopics the core doesn't handle. `/api`
      // (state commands) is subscribed by WLED core; `/cfg` carries config
      // fragments (power plan, restore-on-power, default brightness, slow-fade)
      // and `/reset` triggers a factory wipe.
      if (mqtt != nullptr) {
        String base = mqttDeviceTopic;
        mqtt->subscribe((base + "/cfg").c_str(), 0);
        mqtt->subscribe((base + "/reset").c_str(), 0);
        mqtt->subscribe((base + "/ota").c_str(), 0);
      }

      if (!_blinkDone) {
        _blinkDone = true;
        memcpy(_savedCol, colPri, 4);
        _savedBri = bri;
        _savedFx = effectCurrent;
        colPri[0] = 0; colPri[1] = 255; colPri[2] = 0; colPri[3] = 0;
        effectCurrent = FX_MODE_STATIC;
        bri = 255;
        colorUpdated(CALL_MODE_DIRECT_CHANGE);
        _start = millis();
        _flashing = true;
      }
    }

    bool onMqttMessage(char* topic, char* payload) override {
      String base = mqttDeviceTopic;
      if (strstr(topic, (base + "/cfg").c_str()) != nullptr) {
        applyCfg(payload);
        return true;
      }
      if (strstr(topic, (base + "/reset").c_str()) != nullptr) {
        factoryReset();
        return true;
      }
      // Exact `/ota` (not `/ota/progress`, which we only publish). Stage the job
      // and let loop() run the blocking download/flash.
      if (base + "/ota" == topic) {
        DynamicJsonDocument doc(512);
        if (!deserializeJson(doc, payload)) {
          _otaJob = (const char*)(doc["job_id"] | "");
          _otaUrl = (const char*)(doc["url"] | "");
          _otaVer = (const char*)(doc["version"] | "");
          _otaSha = (const char*)(doc["sha256"] | "");
          // The signature the image has to carry. The gateway has always sent this field and the
          // firmware ignored it, which is why an unsigned image was applied without complaint.
          _otaSigUrl = (const char*)(doc["sig_url"] | "");
          if (_otaUrl.length() > 0 && !_otaPending) _otaPending = true;
        }
        return true;
      }
      return false;
    }

    void loop() override {
      const unsigned long now = millis();

      if (_flashing && (now - _start > 3000)) {
        memcpy(colPri, _savedCol, 4);
        bri = _savedBri;
        effectCurrent = _savedFx;
        colorUpdated(CALL_MODE_DIRECT_CHANGE);
        _flashing = false;
      }

      // Once on Wi-Fi, fetch the broker from the PixC API (retry until it
      // sticks). Broker is never hardcoded — this is the device→Java handoff.
      if (!_provisioned && WiFi.status() == WL_CONNECTED &&
          (_lastProvisionTry == 0 || now - _lastProvisionTry >= kProvisionRetryMs)) {
        _lastProvisionTry = now;
        fetchProvisionConfig();
      }

      if (!WLED_MQTT_CONNECTED) return;

      // A cloud OTA was staged by onMqttMessage — run it now (blocking) so it
      // stays off the MQTT receive callback.
      if (_otaPending) {
        _otaPending = false;
        runOta();
        return;
      }

      if (!_announced) {
        _announced = true;
        publishAnnounce();
        publishState();
        publishPower();
        _lastState = now;
        _lastHealth = now;
        _lastPower = now;
      }
      // A state change reported by WLED — an Alexa command, a LAN call, a physical
      // button, a preset firing. Publishing only on the timer meant any of those was
      // invisible to the cloud for up to kStateIntervalMs, so the app could show
      // lights on moments after they were turned off by voice.
      // A stream starting or stopping is a state change WLED never reports, because the segment
      // config is untouched — only what is drawn over it changes. Publishing on the 5 s timer alone
      // meant up to five seconds of the app showing a colour that is not on the wall.
      if (realtimeMode != _lastRealtime) {
        _lastRealtime = realtimeMode;
        _statePending = true;
      }
      if (_statePending && now - _lastState >= kStateChangeMinMs) {
        _statePending = false;
        _lastState = now;
        publishState();
      }
      if (now - _lastState >= kStateIntervalMs) {
        _lastState = now;
        publishState();
      }
      if (now - _lastHealth >= kHealthIntervalMs) {
        _lastHealth = now;
        publishHealth();
      }
      if (now - _lastPower >= kPowerIntervalMs) {
        _lastPower = now;
        publishPower();
      }
    }

    // WLED fires this on every state change, from led.cpp. Only a flag is set: this
    // runs inside the state-change path, and publishing from here would put MQTT I/O
    // on it. loop() does the actual publish.
    void onStateChange(uint8_t) override {
      _statePending = true;
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

static PixcConnectBlink pixc_connect_blink;
REGISTER_USERMOD(pixc_connect_blink);
