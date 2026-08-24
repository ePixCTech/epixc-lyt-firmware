#include "wled.h"
#include <HTTPClient.h>
#include <Update.h>
#include "mbedtls/sha256.h"

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
    String _otaJob, _otaUrl, _otaSha, _otaVer;

    static constexpr unsigned long kStateIntervalMs  = 5000;
    static constexpr unsigned long kHealthIntervalMs = 30000;
    static constexpr unsigned long kPowerIntervalMs  = 30000;
    static constexpr unsigned long kProvisionRetryMs = 10000;
    // A state change can be a slider being dragged. Without a floor, one drag is a
    // publish per frame; with it, the cloud still sees the change within 250 ms.
    static constexpr unsigned long kStateChangeMinMs = 250;

    // Ask the PixC API which broker to use, then point WLED's MQTT at it. The
    // broker is dynamic (dev LAN IP rotates), so it is never hardcoded — fetched.
    void fetchProvisionConfig() {
      if (WiFi.status() != WL_CONNECTED || _apiHost.length() == 0) return;

      // This call decides which MQTT broker the device trusts, and it is still
      // plaintext HTTP with an unverified reply — so anyone on the same Wi-Fi can
      // answer first and name their own broker, bypassing both the per-device
      // credential and MQTT TLS. Ticket 33 replaces it with HTTPS and a pinned
      // ISRG Root X1; that needs a bench unit to validate the handshake, the same
      // wait ticket 10's TLS work is in.
      //
      // Until then: REFUSE to speak plaintext to a TLS port. Doing it anyway would
      // send a provisioning request in the clear to :443, fail confusingly, and
      // hide the fact that the fallback host is unreachable by design right now.
      if (_apiPort == 443) {
        DEBUG_PRINTLN(F("[ePixC] provisioning skipped: port 443 needs TLS (ticket 33)"));
        return;
      }

      char url[200];
      snprintf(url, sizeof(url), "http://%s:%u/api/v1/provision?mac=%s",
               _apiHost.c_str(), _apiPort, escapedMac.c_str());
      WiFiClient client;
      HTTPClient http;
      http.setConnectTimeout(4000);
      if (!http.begin(client, url)) return;
      int code = http.GET();
      if (code == 200) {
        DynamicJsonDocument doc(640);
        if (!deserializeJson(doc, http.getString())) {
          JsonObject d = doc["data"].isNull() ? doc.as<JsonObject>()
                                              : doc["data"].as<JsonObject>();
          // API serializes snake_case (mqtt_host/mqtt_port); accept camelCase too.
          const char* host = d["mqtt_host"] | (d["mqttHost"] | "");
          int port = d["mqtt_port"] | (d["mqttPort"] | PIXC_MQTT_PORT);
          if (host && strlen(host) > 0) {
            strlcpy(mqttServer, host, MQTT_MAX_SERVER_LEN + 1);
            mqttPort = port;
            mqttEnabled = true;
            _provisioned = true;
            DEBUG_PRINTF("[PixC] broker from API: %s:%d\n", mqttServer, mqttPort);
            // Force WLED to (re)connect MQTT to the new broker.
            if (mqtt != nullptr && mqtt->connected()) mqtt->disconnect();
            initMqtt();
          }
        }
      }
      http.end();
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

    void publishState() {
      char buf[224];
      Segment& seg = strip.getSegment(strip.getMainSegmentId());
      // WLED-native shape (pixc-mqtt prefers on/bri/seg[].col/fx).
      snprintf(buf, sizeof(buf),
        "{\"on\":%s,\"bri\":%u,\"seg\":[{\"col\":[[%u,%u,%u,%u]],\"fx\":%u,\"pal\":%u,\"sx\":%u,\"ix\":%u}]}",
        (bri > 0 ? "true" : "false"), bri,
        colPri[0], colPri[1], colPri[2], colPri[3], effectCurrent,
        seg.palette, seg.speed, seg.intensity);
      publishKind("state", buf);
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

      if (ok && _otaSha.length() == 64) {
        char got[65]; hex32(digest, got);
        if (!_otaSha.equalsIgnoreCase(got)) { ok = false; failMsg = "sha256 mismatch"; }
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

    void onMqttConnect(bool /*sessionPresent*/) override {
      _announced = false; // re-announce on every (re)connect

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
