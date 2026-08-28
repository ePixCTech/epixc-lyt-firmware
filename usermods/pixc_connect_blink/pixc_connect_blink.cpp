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
// bootloader rollback. See confirmImageIfPending().
#include <esp_ota_ops.h>
// CONFIG_APP_ROLLBACK_ENABLE. Reached implicitly through Arduino's headers, but named here for
// the same reason esp_system.h is: this file reads it directly, and the value comes from the
// prebuilt framework's sdkconfig rather than from anything in this repository.
#include <sdkconfig.h>
// The TLS fetch lives in its own translation unit: ESP-IDF's HTTP client header and
// ESPAsyncWebServer both declare HTTP_GET/HTTP_PATCH, and a WLED usermod needs both. See
// pixc_https.cpp for why Arduino's WiFiClientSecure is not an option here.
#include "pixc_https.h"
// The RGB/RGBW width table, and the build-time assertion that the compiled-in bus is a width the
// cloud is able to set. Read the header before touching applyLedWidth().
#include "pixc_led_bus.h"

// ePixC API host the device calls to learn its MQTT broker (see provision()).
// Normally written by the app during pairing (um.PixcConnect.apiHost); this is
// only the fallback for a unit that never completed setup. The broker itself is
// never hardcoded — it is always fetched from this API.
//
// The default used to be a host on the pre-rename domain, which ePixC does not own. Anyone who
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

// ePixC device-edge usermod. Two jobs:
//
//  1. MQTT bridge to the ePixC cloud (epixc-mqtt broker). WLED's native MQTT
//     topics/payloads don't match the ePixC contract, so this usermod:
//       - forces the device topic to `epixc/v1/d/{mac}` (full MAC, lowercase) so
//         WLED subscribes `epixc/v1/d/{mac}/api` for commands;
//       - publishes `announce` (once on connect), `state` and `health` to
//         `epixc/v1/d/{mac}/{kind}` in the shapes epixc-mqtt expects.
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

    // A config push changed the bus width and the re-init has not finished yet. The cloud's record
    // of `led_channels` is written from the announce, so until this goes out the server still
    // believes the device is whatever width it was before — and `led_channels` is the only
    // evidence anywhere that the push actually landed. Set by applyLedWidth(), drained in loop()
    // once WLED::loop() has cleared doInitBusses.
    bool _reannouncePending = false;

    // True when this boot is running an image the bootloader has marked PENDING_VERIFY — i.e. a
    // freshly installed OTA that will be rolled back on the next boot unless this firmware says it
    // is working.
    //
    // On every current build it is ALWAYS false, and that is not a failure of the read — it is the
    // whole state of play. confirmImageIfPending() names the two callers that confirm the image
    // before this usermod ever runs. The flag is kept, and still published, because the pair
    // (supported, armed) is the fingerprint of exactly that.
    bool _awaitingConfirm = false;
    bool _confirmed = false;

    // Whether the framework this image linked against has bootloader rollback compiled in — a
    // BUILD fact, taken from the prebuilt Arduino framework's sdkconfig, which is the same value
    // CI asserts (.github/workflows/epixc-firmware.yaml, "Check bootloader rollback is compiled
    // in").
    //
    // Reported alongside the runtime state rather than instead of it, because the two answer
    // different questions and were previously conflated: "can this fleet revert at all" is decided
    // at build time, "is this particular boot on probation" is decided at run time, and reading
    // the second as the first makes a device that auto-confirmed look identical to a framework
    // with the option switched off.
    #ifdef CONFIG_APP_ROLLBACK_ENABLE
    static constexpr bool kRollbackSupported = true;
    #else
    static constexpr bool kRollbackSupported = false;
    #endif
    byte _savedCol[4] = {0, 0, 0, 0};
    byte _savedBri = 0;
    uint8_t _savedFx = 0;

    // ePixC API host (provisioned by the app, persisted in usermod config). The
    // device calls GET {apiHost}:{apiPort}/api/v1/provision?mac=... to learn the
    // MQTT broker, then hands off to MQTT.
    String _apiHost = PIXC_API_HOST;
    uint16_t _apiPort = PIXC_API_PORT;
    bool _provisioned = false;          // got the broker from the API yet?
    unsigned long _lastProvisionTry = 0;

    // OTA (cloud-triggered, MQTT-pull). The trigger arrives on `epixc/v1/d/{mac}/ota`;
    // the actual HTTP download + flash is deferred to loop() so it never blocks
    // the MQTT receive callback. Progress is reported on `.../ota/progress`.
    bool _otaPending = false;
    String _otaJob, _otaUrl, _otaSha, _otaVer, _otaSigUrl;

    // True when this image was built with no signing key baked in. Compile-time constant, so the
    // release build pays nothing for it and the branches below vanish.
    //
    // Derived from the key itself, not from EPIXC_OTA_UNSIGNED_DEV_BUILD: the flag says what
    // someone intended, the key says what actually got compiled in, and only the second one
    // decides whether verifySignature() can ever return nullptr. On such an image OTA is not
    // "likely to fail" — every update is refused, permanently, and the only fix is a cable.
    static constexpr bool kOtaDisabled = (sizeof(PIXC_OTA_PUBKEY_PEM) <= 1);

    // The version this firmware reports — to the app, to the cloud, and to whoever is reading a
    // support ticket. An image that can never take an update says so here, because this is the
    // one field every consumer of a device's state already looks at. Without it, a bench build
    // and a shipping build are distinguishable only by reading the binary, and a dev image
    // flashed onto a customer unit would look exactly like a healthy one until the day an update
    // was published and that single device silently refused it.
    static const char* fwVersion() {
      if (!kOtaDisabled) return versionString;
      static char v[WLED_VERSION_MAX_LEN + 16];
      if (v[0] == '\0') snprintf(v, sizeof(v), "%s-UNSIGNED-DEV", versionString);
      return v;
    }

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

      // The transport is a COMPILE-TIME decision, not a runtime one. Founder's call, 2026-08-28,
      // grilling ticket 33.
      //
      // This read `_apiPort == 443`, and `_apiPort` comes from `readFromConfig` — that is,
      // from `POST /json/cfg`, which `wled_server.cpp` gates on `settingsPIN` and which shipped
      // with no PIN set. So anyone on the customer's Wi-Fi could write `apiPort: 8080`, and the
      // device would drop to plaintext on the next reconnect and ask them which broker to trust,
      // handing over the per-device credential from ticket 09 in the process. That is exactly
      // the attack this ticket was opened for, restored through a door the ticket never looked
      // at, in a build whose stated guarantee was "never falls back to plaintext".
      //
      // A guarantee that a configuration value can revoke is not a guarantee. `PixC_V1` has no
      // plaintext path compiled into it at all now, so there is no value any attacker can write
      // that produces one.
#ifdef EPIXC_ALLOW_PLAINTEXT_PROVISION
      const bool useTls = (_apiPort == 443);   // PixC_V1_dev only
#else
      const bool useTls = true;
#endif

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
      // Both arms of a ternary have to compile even when one is unreachable, so the CALL has to
      // be behind the same guard as the function. Caught by the compiler on the first release
      // build after the change, which is the argument for compiling the path out rather than
      // reasoning that nothing can reach it: an absent function fails loudly at build time, an
      // unreachable one fails quietly in the field.
#ifdef EPIXC_ALLOW_PLAINTEXT_PROVISION
      const int len = useTls ? pixcHttpsGet(url, body, sizeof(body))
                             : httpGet(url, body, sizeof(body));
#else
      const int len = pixcHttpsGet(url, body, sizeof(body));
#endif
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

    // Plaintext GET, bench only — and now genuinely absent from a release image rather than
    // merely unreachable in it. The previous comment said a release build "cannot arrange" a
    // non-443 port; it could, through `readFromConfig`. Unreachable-by-argument is how this
    // defect survived, so the function is compiled out instead of reasoned about.
#ifdef EPIXC_ALLOW_PLAINTEXT_PROVISION
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
#endif  // EPIXC_ALLOW_PLAINTEXT_PROVISION



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
        fwVersion(), ledChannels, strip.getLengthTotal());
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
        (unsigned long)(millis() / 1000), fwVersion());
      publishKind("health", buf);
    }

    // Put the LED bus on the width the cloud says the customer's reel is: "RGB" or "RGBW".
    //
    // The device keeps its own wiring. Pin, length, colour order, reversal, skip, frequency and
    // both current limits are read back off the LIVE buses and handed straight back — the only
    // fields this rewrites are the bus type and the auto-white mode. That is the whole reason the
    // cloud sends a two-value string instead of an `hw.led.ins` array: it cannot express a pin, so
    // it cannot get one wrong. See pixc_led_bus.h.
    //
    // Returns true if a re-init was staged.
    //
    // The equality check at the top is load-bearing, not an optimisation. This runs on EVERY
    // config push, and a config push happens whenever anything about the device changes in the
    // cloud — a rename, a room move, a brightness plan. Re-initialising the buses tears down and
    // rebuilds them (WLED::loop() -> WS2812FX::finalizeInit(), wled00/wled.cpp:217-226), and with
    // `autoSegments` on, makeAutoSegments() clears and rebuilds the segment list
    // (wled00/FX_fcn.cpp:1988-1998). Doing that on every rename would drop a customer's segments
    // because they renamed their lamp. So: same width, no work, no visible event at all.
    bool applyLedWidth(const char* channels) {
      const pixc_led_bus::Width* want = pixc_led_bus::find(channels);
      if (want == nullptr) return false;   // unknown width: leave the strip exactly as it is

      size_t busCount = BusManager::getNumBusses();
      if (busCount == 0) return false;

      // The auto-white mode counts as part of the width, not as a separate preference. It decides
      // where the white byte comes from, and a four-channel bus with the wrong mode is a strip
      // whose white die is driven by the wrong thing — which is a fault of the same kind as not
      // driving it at all.
      //
      // It is also the state some units are already in. The app used to rewrite the bus over the
      // LAN with rgbwm=1 (AUTO_BRIGHTER), which derives white from RGB and *ignores* the app's own
      // white value — so on those units the white control was plumbed end to end and did nothing.
      // Comparing the mode as well as the type is what lets a push correct them; comparing only
      // the type would leave them alone forever, because their type is already right.
      //
      // The cost is that a mode set by hand in WLED's own LED settings page is overwritten by the
      // next cloud push. That is the deliberate consequence of the cloud owning the width.
      bool changed = false;
      for (size_t i = 0; i < busCount; i++) {
        const Bus* bus = BusManager::getBus(i);
        if (bus == nullptr) continue;
        // getType() keeps bit 7 (the off-refresh hack); the width lives in the low seven bits.
        if ((bus->getType() & 0x7F) != want->type ||
            bus->getAutoWhiteMode() != want->autoWhite) { changed = true; break; }
      }
      if (!changed) return false;

      // Rebuild the staged config from the live buses. busConfigs is a leftover-prone global —
      // WLED's own deserializer appends to it without clearing (wled00/cfg.cpp:220-253), so two
      // pushes inside one loop iteration would produce duplicate buses. Clear it first.
      busConfigs.clear();
      for (size_t i = 0; i < busCount; i++) {
        const Bus* bus = BusManager::getBus(i);
        if (bus == nullptr) break;
        uint8_t pins[OUTPUT_MAX_PINS] = {255, 255, 255, 255, 255};
        bus->getPins(pins);
        // Bit 7 back on if this bus needed off-refresh, because BusConfig's constructor reads the
        // flag out of the type byte rather than taking it separately.
        uint8_t type = want->type | (bus->isOffRefreshRequired() ? 0x80 : 0x00);
        busConfigs.emplace_back(type, pins, bus->getStart(), bus->getLength(),
                                bus->getColorOrder(), bus->isReversed(), bus->skippedLeds(),
                                want->autoWhite, bus->getFrequency(), bus->getLEDCurrent(),
                                bus->getMaxCurrent(), bus->getDriverType());
      }
      // Finalisation is deferred to WLED::loop(), which is also where the result gets written to
      // cfg.json (configNeedsWrite). Doing it here would tear the buses down underneath whatever
      // is drawing.
      doInitBusses = true;
      _reannouncePending = true;
      DEBUG_PRINTF("[ePixC] LED bus -> %s (type %u, aw %u)\n",
                   want->channels, (unsigned)want->type, (unsigned)want->autoWhite);
      return true;
    }

    // Apply a WLED config fragment pushed by the cloud over `epixc/v1/d/{mac}/cfg`
    // (power plan -> def.bri, restore-on-power -> def.on, slow-fade -> light.tr.dur).
    // Only native WLED cfg keys take effect; unknown keys are ignored safely.
    //
    // `pixc.led_channels` is the one exception: a key the core knows nothing about, read here.
    // The cloud has to be able to say how wide the strip is, and there is no native WLED cfg key
    // that says it without also saying which pin it is on — which the cloud has no business
    // knowing. See pixc_led_bus.h for the boundary and why it is drawn there.
    void applyCfg(const char* payload) {
      // This WLED fork uses ArduinoJson v6 — JsonDocument is abstract; use a
      // sized DynamicJsonDocument. The cfg fragment (def/light/pixc) is small.
      DynamicJsonDocument doc(2048);
      if (deserializeJson(doc, payload)) return;
      JsonObject root = doc.as<JsonObject>();
      deserializeConfig(root, false);

      bool busChange = applyLedWidth(root["pixc"]["led_channels"] | (const char*)nullptr);

      // Only write now if the buses are staying put. When they are not, WLED::loop() re-inits them
      // and sets configNeedsWrite itself, and serializing at this moment would persist the buses
      // as they are about to stop being — the width would be correct in RAM and stale on flash
      // until the next push. WLED's own deserializer refuses the save for the same reason
      // (wled00/cfg.cpp:770).
      if (!busChange) serializeConfigToFS();
    }

    // Factory reset: wipe config + Wi-Fi credentials and reboot back into ePixC-AP.
    void factoryReset() {
      WLED_FS.remove("/cfg.json");
      WLED_FS.remove("/wsec.json");
      doReboot = true;
    }

    // Report OTA progress to the cloud on `epixc/v1/d/{mac}/ota/progress`. `status`
    // is one of downloading/installing/done/failed (epixc-mqtt treats done|failed
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
      // Over TLS against the pinned roots, like the image, and read by LENGTH rather than as a
      // string: a DER signature contains NUL bytes, so anything that NUL-terminates corrupts it.
      const int got = pixcHttpsGetBinary(_otaSigUrl.c_str(), out, cap, 10000);
      if (got <= 0) return false;
      *outLen = (size_t)got;
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

    // Download the firmware image over TLS and flash it to the inactive OTA partition, verifying
    // SHA-256 and then the detached signature before committing the new image as bootable.
    // Streams in chunks so a ~1.8MB image never has to fit in RAM. Blocking by design — runs from
    // loop(), and the hardware watchdog is disabled (WLED_WATCHDOG_TIMEOUT=0).
    //
    // **Why this is TLS with the same pinned roots as everything else**, rather than the plaintext
    // fetch it used to be, or a deliberately weaker transport chosen to keep CA risk apart.
    //
    // It used to build a bare `WiFiClient` and hand it to `HTTPClient::begin()` with an https URL.
    // That call ACCEPTS the scheme — it sets `_port = 443` and `_secure = true` and returns true —
    // but `_secure` only ever reaches cookie handling; `connect()` calls `_client->connect()` on
    // the plain client it was given. So the device opened a TCP socket to Caddy's TLS listener and
    // wrote a plaintext request into it. That connection cannot complete, which means the cloud
    // OTA download could never have worked against production at all.
    //
    // The reflex worry about fixing it this way is blast radius: PIXC_TRUSTED_ROOTS also gates the
    // provisioning fetch, `PixC_V1` sets WLED_DISABLE_OTA so a shipped unit has no local update
    // path, and the only repair channel for a bad root is an OTA — so putting the download behind
    // the same roots looks like betting both halves on one CA event.
    //
    // It is not, because the download is already strictly downstream of those roots. Reaching this
    // function requires, in order: `fetchProvisionConfig()` to succeed over HTTPS pinned to
    // PIXC_TRUSTED_ROOTS (and `connected()` clears `_provisioned` on every Wi-Fi connect, so this
    // happens every boot — the broker is never remembered across one); then an MQTT connection on
    // 8883, which `pixc_mqtt_client.cpp` also pins to PIXC_TRUSTED_ROOTS; then an `ota` command
    // arriving on that connection to set `_otaPending`; then the `WLED_MQTT_CONNECTED` gate in
    // loop(). If both roots go bad, no OTA command is ever delivered and the fleet is unreachable
    // long before the download's trust store matters.
    //
    // That also disposes of the alternatives, which all pay something for nothing:
    //   - plaintext, leaning on the signature: gives up confidentiality and lets a network
    //     attacker choose which signed version a unit installs (an old one with a known bug is
    //     still correctly signed), and buys no recoverability, since the command that starts the
    //     download still needs the roots.
    //   - `setInsecure()`: encrypts against a passive observer but authenticates nobody, so an
    //     active attacker substitutes the image freely — and again buys no recoverability.
    //   - a DIFFERENT root pinned for the firmware host: strictly worse. It cannot help (the
    //     command channel still needs the original roots) and it ADDS a second independent CA that
    //     can break OTA, turning one failure mode into two.
    //
    // The signature is what makes a bad image unflashable; TLS is what makes the download reach
    // the right server at all. They answer different questions, and this one is reachability.
    void runOta() {
      if (WiFi.status() != WL_CONNECTED || _otaUrl.length() == 0) {
        publishOtaProgress("failed", 0, "no wifi");
        return;
      }

      // A TLS handshake needs roughly 40 KB of heap for the record buffers and the certificate
      // chain, and failing inside mbedTLS reads as a mysterious OTA failure rather than as memory
      // pressure. Say so instead; the job can be retried.
      if (ESP.getFreeHeap() < 50000) {
        publishOtaProgress("failed", 0, "low memory");
        return;
      }

      DEBUG_PRINTF("[ePixC] OTA start v%s <- %s\n", _otaVer.c_str(), _otaUrl.c_str());
      publishOtaProgress("downloading", 0);

      int status = 0;
      int total = -1;                                   // -1 if chunked/unknown
      PixcHttpsStream* stream = pixcHttpsOpen(_otaUrl.c_str(), &status, &total, 20000);
      if (stream == nullptr) {
        char e[32];
        if (status > 0) snprintf(e, sizeof(e), "http %d", status);
        else            snprintf(e, sizeof(e), "tls/connect");
        publishOtaProgress("failed", 0, e);
        return;
      }

      if (!Update.begin(total > 0 ? (size_t)total : UPDATE_SIZE_UNKNOWN)) {
        publishOtaProgress("failed", 0, "no space"); pixcHttpsClose(stream); return;
      }

      mbedtls_sha256_context sha;
      mbedtls_sha256_init(&sha);
      mbedtls_sha256_starts(&sha, 0);                   // 0 = SHA-256 (not SHA-224)

      uint8_t buf[1024];
      size_t written = 0;
      int lastPct = -1;
      bool ok = true;
      const char* failMsg = nullptr;

      while (true) {
        const int n = pixcHttpsRead(stream, buf, sizeof(buf));
        if (n < 0) { ok = false; failMsg = "read"; break; }
        if (n == 0) break;                              // end of body, clean or otherwise
        if (Update.write(buf, n) != (size_t)n) { ok = false; failMsg = "flash write"; break; }
        mbedtls_sha256_update(&sha, buf, n);
        written += n;
        if (total > 0) {
          int pct = (int)((written * 90ULL) / (size_t)total);   // 0..90 while downloading
          if (pct != lastPct && pct % 10 == 0) { publishOtaProgress("downloading", pct); lastPct = pct; }
          if (written >= (size_t)total) break;
        }
        yield();
      }

      // A connection cut mid-image ends the loop exactly like a clean finish. The SHA-256 below
      // would catch it anyway — that is the guarantee, and it does not depend on this check — but
      // "stalled" is a far more useful thing to put in front of support than "sha256 mismatch".
      if (ok && !pixcHttpsComplete(stream)) { ok = false; failMsg = "stalled"; }
      pixcHttpsClose(stream);

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
        DEBUG_PRINTF("[ePixC] OTA failed: %s\n", failMsg ? failMsg : "?");
        publishOtaProgress("failed", 0, failMsg);
        return;
      }

      publishOtaProgress("installing", 95);
      if (!Update.end(true)) {           // commit: set the new image bootable
        char e[40]; snprintf(e, sizeof(e), "finalize %u", Update.getError());
        publishOtaProgress("failed", 0, e);
        return;
      }

      DEBUG_PRINTLN("[ePixC] OTA done; rebooting");
      publishOtaProgress("done", 100);
      delay(1200);                       // let the QoS0 progress publish flush
      doReboot = true;                   // WLED reboots from its own loop
    }

  public:
    void setup() override {
      // Say it out loud, once, at boot. The build-time guard in pixc_ota_pubkey.h means nobody
      // reaches this state by accident — only by setting EPIXC_OTA_UNSIGNED_DEV_BUILD — but the
      // image outlives the decision, and the person holding the board later is not necessarily
      // the person who built it.
      if (kOtaDisabled) {
        DEBUG_PRINTLN(F("[ePixC] ********************************************************"));
        DEBUG_PRINTLN(F("[ePixC] *  UNSIGNED DEV BUILD - NO OTA SIGNING KEY COMPILED IN  *"));
        DEBUG_PRINTLN(F("[ePixC] *  Every firmware update WILL BE REFUSED on this unit.  *"));
        DEBUG_PRINTLN(F("[ePixC] *  Not shippable. Do not flash onto a customer device.  *"));
        DEBUG_PRINTLN(F("[ePixC] ********************************************************"));
      }

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
      // Broker is NOT hardcoded — it is fetched from the ePixC API (see
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

    // Persist the ePixC API host/port so the app provisions it once and it
    // survives reboots (cfg.json -> um.PixcConnect.{apiHost,apiPort}).
    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject("PixcConnect");
      top["apiHost"] = _apiHost;
      top["apiPort"] = _apiPort;
      // `settingsPin` is deliberately NOT written back. This same function answers
      // `GET /json/cfg`, which is unauthenticated, so echoing the PIN here would publish the
      // credential to exactly the caller it exists to stop. WLED keeps settingsPIN in wsec.json
      // for the same reason, and that is where this one is persisted.
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root["PixcConnect"];
      if (top.isNull()) return false;
#ifdef EPIXC_ALLOW_PLAINTEXT_PROVISION
      // Bench builds keep the host configurable, which is the entire reason the dev env exists:
      // the bench Mac's address rotates.
      _apiHost = top["apiHost"] | _apiHost;
      _apiPort = top["apiPort"] | _apiPort;
#else
      // A release unit provisions from the compiled host and nothing else. Founder's call,
      // 2026-08-28, ticket 33.
      //
      // Pinning ISRG Root X1 proves the peer holds a Let's Encrypt certificate. It does NOT
      // prove the peer is ePixC — an attacker with a domain and a free certificate satisfies a
      // root pin completely. Binding to the CA was never the goal; binding to api.epixc.in is,
      // and the only way to do that is to stop letting a config write choose the host.
      //
      // Nothing legitimate is lost: the app writes exactly this value already
      // (`app_config.dart` -> provisionApiHost/provisionApiPort, 443 in a release build), so
      // ignoring the write is a no-op on the real pairing path and a refusal on every other.
      // The keys are still ACCEPTED and still serialized, so an app that writes them gets its
      // 200 and the config round-trips; they simply do not move the host.
#endif

      // The settings PIN, set by the app during pairing. Founder's call, 2026-08-28, ticket 33.
      //
      // WLED's own `settingsPIN` lives in wsec.json and is only ever written by the settings FORM
      // (`set.cpp`), which the app does not speak. Accepting it here lets the app set it in the
      // same `/json/cfg` POST it already sends while it is connected directly to the device's own
      // access point — the one moment in a device's life when an unauthenticated write on that
      // link is not a weakness, because there is nothing else on the network.
      //
      // Why a PIN at all, when the provisioning host is now compiled in: `POST /json/cfg` can
      // still rewrite the LED bus, the MQTT settings and the access point. Closing the host was
      // the specific fix; this closes the door.
      //
      // Four digits, because that is WLED's format (`set.cpp` accepts length 4 or 0) and the
      // whole point is to reuse the gate the core already enforces rather than invent a second
      // one. "0000" is WLED's placeholder for "unchanged" and is rejected there, so it is
      // rejected here too — otherwise the app could believe it had set a PIN that was ignored.
      const char* pin = top["settingsPin"] | (const char*)nullptr;
      if (pin != nullptr && strlen(pin) == 4 && strcmp(pin, "0000") != 0 &&
          strcmp(pin, settingsPIN) != 0) {
        strlcpy(settingsPIN, pin, 5);
        // Persist immediately. wsec.json is written by serializeConfigSec() and NOT by the
        // ordinary config save that follows this call, so without this the PIN would hold until
        // the next reboot and then silently vanish — a lock that quietly stops locking.
        serializeConfigSec();
        DEBUG_PRINTLN(F("[ePixC] settings PIN set"));
      }

      return true;
    }

    // Tell the bootloader this image works, so it stops being a candidate for rollback.
    //
    // "Works" is MEANT to be: it reached the broker. That is the whole point of the firmware — a
    // build that boots but cannot talk to the cloud is exactly the build that should be rolled
    // back, and it is the failure a bad OTA most plausibly produces. Confirming in setup(), which
    // is the obvious place, would confirm every image including one that never connects again.
    //
    // That is not what the built image does, and nothing in this function can change it, because
    // two other callers confirm first — both unconditional, both long before Wi-Fi exists:
    //
    //   1. initArduino(), in the prebuilt Arduino core (framework-arduinoespressif32,
    //      cores/esp32/esp32-hal-misc.c). Under CONFIG_APP_ROLLBACK_ENABLE it calls
    //      esp_ota_mark_app_valid_cancel_rollback() itself. app_main() (cores/esp32/main.cpp) runs
    //      it BEFORE creating the task that calls setup(), so it beats all of WLED, this usermod
    //      included — which is why _awaitingConfirm, read in setup(), is always false.
    //   2. WLED's own markOTAvalid() (wled00/ota_update.cpp), called unconditionally at the end of
    //      WLED::setup() (wled00/wled.cpp). A no-op by the time it runs, but a second confirm site
    //      to remember to handle.
    //
    // Read out of the linked image rather than inferred from source: in the PixC_V1_dev ELF,
    // initArduino, WLED::setup (via markOTAvalid) and PixcConnectBlink::onMqttConnect each carry a
    // call to esp_ota_mark_app_valid_cancel_rollback. Three confirm sites; the two that win are
    // the unconditional ones.
    //
    // So the rule actually in force is "an image is good if it reaches initArduino()". That covers
    // an image which will not start at all, and nothing else — not the LED path, not the usermod,
    // not the web UI, not the heap under a render loop, and not the broker.
    //
    // Making the intended rule real takes three changes TOGETHER, and a bench unit:
    //   a. defer the framework's confirm by overriding its weak hook —
    //      `extern "C" bool verifyRollbackLater() { return true; }`. esp32-hal-misc.c declares it
    //      __attribute__((weak)) precisely for this, so a strong definition here wins at link time
    //      and no framework patch is needed.
    //   b. stop WLED core confirming in setup() (markOTAvalid(), wled00/ota_update.cpp).
    //   c. choose the criterion, and the deadline by which a good image must meet it.
    //
    // (c) is not a code question, and it is why this is still as it is. Arming rollback means a
    // GOOD image gets reverted whenever the criterion is missed for reasons that have nothing to
    // do with the image: a router reboot, a broker restart, a root that expired (the pinned roots
    // carry a build-enforced deadline). The probation window is exactly ONE reset long — the
    // bootloader reverts on the next boot, and it does not care whether that boot came from a
    // crash or from a customer pulling the plug — so an update followed by a power cycle reverts
    // even though nothing was wrong. A revert loop on a healthy build is its own outage, and it
    // looks like the update system is broken.
    //
    // And the criterion the intent names depends on a TLS path that has never completed a
    // handshake on hardware (platformio_override.ini, env:PixC_V1, says so). Arming a fleet-wide
    // revert on an untested path is not a change to make from a desk. Bench first: a signed image
    // on a real unit, confirm the revert actually fires, and measure boot-to-broker on a real
    // network before anyone picks a window.
    void confirmImageIfPending() {
      if (!_awaitingConfirm || _confirmed) return;
      _confirmed = true;
      if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        DEBUG_PRINTLN("[ePixC] OTA image confirmed; rollback cancelled");
      } else {
        DEBUG_PRINTLN("[ePixC] OTA image confirm FAILED; this image may be rolled back");
      }
    }

    void onMqttConnect(bool /*sessionPresent*/) override {
      _announced = false; // re-announce on every (re)connect

      // First broker connection since power-on is a boot; every later one is a reconnect. The
      // uptime is carried because it turns "it rebooted" into "it rebooted four minutes in",
      // which is the difference between a power problem and a firmware one.
      char extra[256];
      if (!_bootReported) {
        _bootReported = true;
        // Two facts, published separately, because reading one as the other is what hid the
        // problem for as long as it was hidden.
        //
        //   rollback_supported — the linked framework has bootloader rollback compiled in.
        //   rollback_armed     — THIS boot was still on probation by the time the usermod looked.
        //
        // The pair is the diagnostic:
        //   !supported             the fleet cannot revert at all, whatever the code intends.
        //   supported && armed     rollback is real and this boot is an image on trial.
        //   supported && !armed    something confirmed the image before the usermod ran. This is
        //                          the state of every current build — see confirmImageIfPending().
        //
        // Only rollback_armed used to go out, described as "the only honest way to know" whether
        // the bootloader has rollback. It is not, and it never was: it is structurally always
        // false here, so it read as "the framework has the option off" — flatly contradicting the
        // CI gate that asserts it is on, and sending the next person to audit the build when the
        // answer is in the boot order.
        snprintf(extra, sizeof(extra),
                 "\"reason\":\"%s\",\"fw_version\":\"%s\",\"rollback_armed\":%s,"
                 "\"rollback_supported\":%s",
                 resetReasonName(), fwVersion(),
                 _awaitingConfirm ? "true" : "false",
                 kRollbackSupported ? "true" : "false");
        publishEvent("boot", extra);
      } else {
        snprintf(extra, sizeof(extra), "\"uptime\":%lu", (unsigned long)(millis() / 1000));
        publishEvent("reconnect", extra);
      }

      // Reaching the broker is meant to be the proof, and this is where it would be applied.
      // Done after the event publish so the record of this boot exists before the image stops
      // being reversible. Today it is a no-op — read confirmImageIfPending() before changing
      // anything here, including the placement of this call.
      confirmImageIfPending();

      // Subscribe the ePixC control subtopics the core doesn't handle. `/api`
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

      // Once on Wi-Fi, fetch the broker from the ePixC API (retry until it
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
      // A bus-width change has finished re-initialising. Say so.
      //
      // `led_channels` is device-reported by design — it is the one part of the strip's identity
      // the device can actually observe (strip.hasWhiteChannel()). That property is now also the
      // only receipt that a width push worked: the cloud asked for RGBW, and this is the device
      // answering with what it ended up on. If the re-init fell back to a placeholder bus for want
      // of memory (wled00/FX_fcn.cpp:1231-1236) the announce says RGB, and the disagreement is
      // visible in the devices table instead of only on the customer's wall.
      //
      // Gated on doInitBusses because WLED::loop() runs the re-init AFTER usermods
      // (wled00/wled.cpp:82 vs :217), so on the iteration the push arrives the buses are still the
      // old ones. One extra loop is nothing; announcing the old width would be wrong forever.
      if (_reannouncePending && !doInitBusses) {
        _reannouncePending = false;
        publishAnnounce();
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
