#pragma once

// An AsyncMqttClient-shaped client built on ESP-IDF's esp-mqtt, so the connection can use TLS.
//
// **Why this file exists.** WLED talks to the broker through `AsyncMqttClient`, which guards
// `setSecure()` behind `ASYNC_TCP_SSL_ENABLED` — and `AsyncTCP` for the ESP32 has no SSL at all.
// So on this chip that library cannot do TLS under any build flag, and ePixC's broker is TLS-only
// on 8883 with per-device credentials. Every other option was worse: a plaintext broker port to
// support one library, or a second MQTT stack running beside WLED's with two sets of subscriptions
// and two ideas of what "connected" means.
//
// It presents exactly the surface WLED and the ePixC usermod already call, so `wled00/mqtt.cpp`,
// `button.cpp` and the usermod compile unchanged and there is one connection, one subscription
// list, one state.
//
// **Threading: every callback runs on the LED loop task, never on esp-mqtt's.** esp-mqtt delivers
// events on its own task. Running WLED's handlers there (deserializeState, the usermod's config
// apply, colorUpdated) raced the loop task over WLED globals and ran all of it on esp-mqtt's
// 6 KB stack (audit R2). So the event handler only copies each event into a FreeRTOS queue, and
// `loop()` - called from WLED::loop() - drains it and calls onConnect/onMessage there. The one
// thing left on the esp-mqtt task is the network itself.
//
// **Reconnects are ours, with backoff and jitter** (audit R3). esp-mqtt's own reconnect is a fixed
// 10 s for every unit, so a broker restart brought the whole fleet back in the same second. Auto
// reconnect is disabled and `loop()` calls esp_mqtt_client_reconnect() on an exponential schedule
// with jitter (pixc::backoffWithJitter).
//
// Payload chunking is passed through as `index`/`total`, which is what `onMqttMessage` already
// reassembles — the esp-mqtt names for those are `current_data_offset` and `total_data_len`.

#include <Arduino.h>
#include <IPAddress.h>
#include <mqtt_client.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <functional>

/// Present so the existing callback signature compiles. WLED reads nothing from it.
struct AsyncMqttClientMessageProperties {
  uint8_t qos = 0;
  bool dup = false;
  bool retain = false;
};

class PixcMqttClient {
 public:
  using MessageCallback = std::function<void(char* topic, char* payload,
                                             AsyncMqttClientMessageProperties properties,
                                             size_t len, size_t index, size_t total)>;
  using ConnectCallback = std::function<void(bool sessionPresent)>;

  PixcMqttClient() = default;

  PixcMqttClient& onMessage(MessageCallback cb) { _onMessage = std::move(cb); return *this; }
  PixcMqttClient& onConnect(ConnectCallback cb) { _onConnect = std::move(cb); return *this; }

  PixcMqttClient& setServer(const char* host, uint16_t port);
  PixcMqttClient& setServer(IPAddress ip, uint16_t port);
  PixcMqttClient& setClientId(const char* clientId);
  PixcMqttClient& setCredentials(const char* username, const char* password);
  PixcMqttClient& setKeepAlive(uint16_t seconds);
  PixcMqttClient& setWill(const char* topic, uint8_t qos, bool retain, const char* payload);

  /// Starts the client, or applies a changed server/credentials by restarting it.
  void connect();
  void disconnect(bool force = false);
  /// The LOOP task's view: true from the moment onConnect ran on the loop until the disconnect
  /// event is drained. Never written by the esp-mqtt task.
  bool connected() const { return _connected; }

  /// Drain queued esp-mqtt events and run due reconnects. Call from the main loop only.
  void loop();

  uint16_t publish(const char* topic, uint8_t qos, bool retain, const char* payload);
  uint16_t publish(const char* topic, uint8_t qos, bool retain, const char* payload, size_t length);
  uint16_t subscribe(const char* topic, uint8_t qos);

 private:
  static esp_err_t eventHandler(esp_mqtt_event_handle_t event);
  void handle(esp_mqtt_event_handle_t event);
  bool start();

  bool enqueue(uint8_t kind, esp_mqtt_event_handle_t event);
  void scheduleReconnect();
  void flushEvents();

  esp_mqtt_client_handle_t _client = nullptr;
  QueueHandle_t _events = nullptr;
  bool _connected = false;
  /// Consecutive failed attempts since the last successful connect; drives the backoff.
  uint8_t _attempt = 0;
  bool _reconnectDue = false;
  bool _bootJitterDone = false;
  uint32_t _nextAttemptAt = 0;
  /// Events dropped because the queue was full (debug visibility only).
  uint32_t _dropped = 0;
  /// Whether the esp-mqtt task is running. A stopped client is still allocated, and will sit there
  /// doing nothing until it is started again — which is not what `connect()` used to notice.
  bool _started = false;
  /// Set when a setter changes something esp-mqtt only reads at start; drives a restart.
  bool _dirty = true;

  String _host;
  uint16_t _port = 1883;
  String _clientId;
  String _user;
  String _pass;
  String _willTopic;
  String _willPayload;
  uint8_t _willQos = 0;
  bool _willRetain = false;
  uint16_t _keepAlive = 60;

  /// The topic of the message being delivered, for continuation chunks that arrive without one.
  /// Touched only on the esp-mqtt task.
  String _lastTopic;

  MessageCallback _onMessage;
  ConnectCallback _onConnect;
};
