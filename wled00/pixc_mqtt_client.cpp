#include "wled.h"

#ifdef PIXC_MQTT_ESP_IDF

#include "pixc_mqtt_client.h"
#include "../usermods/pixc_connect_blink/pixc_roots.h"

// TLS is chosen by port rather than by a flag: 8883 is the registered MQTTS port and the only one
// ePixC publishes. A bench broker on 1883 stays plaintext without a second build.
static constexpr uint16_t kMqttsPort = 8883;

PixcMqttClient& PixcMqttClient::setServer(const char* host, uint16_t port) {
  if (_host != host || _port != port) {
    _host = host;
    _port = port;
    _dirty = true;
  }
  return *this;
}

PixcMqttClient& PixcMqttClient::setServer(IPAddress ip, uint16_t port) {
  return setServer(ip.toString().c_str(), port);
}

PixcMqttClient& PixcMqttClient::setClientId(const char* clientId) {
  if (_clientId != clientId) { _clientId = clientId; _dirty = true; }
  return *this;
}

PixcMqttClient& PixcMqttClient::setCredentials(const char* username, const char* password) {
  const char* u = username ? username : "";
  const char* p = password ? password : "";
  if (_user != u || _pass != p) { _user = u; _pass = p; _dirty = true; }
  return *this;
}

PixcMqttClient& PixcMqttClient::setKeepAlive(uint16_t seconds) {
  if (_keepAlive != seconds) { _keepAlive = seconds; _dirty = true; }
  return *this;
}

// Compares before dirtying, like every other setter, and that matters more here than it looks:
// WLED re-runs `initMqtt()` every 30 seconds while disconnected, calling all of these each time.
// A setter that dirtied unconditionally would tear the client down and rebuild it on every one of
// those passes — aborting an in-flight TLS handshake and restarting esp-mqtt's reconnect backoff
// from zero, so a device on a slow link could never finish connecting.
PixcMqttClient& PixcMqttClient::setWill(const char* topic, uint8_t qos, bool retain,
                                        const char* payload) {
  const char* t = topic ? topic : "";
  const char* p = payload ? payload : "";
  if (_willTopic != t || _willPayload != p || _willQos != qos || _willRetain != retain) {
    _willTopic = t;
    _willPayload = p;
    _willQos = qos;
    _willRetain = retain;
    _dirty = true;
  }
  return *this;
}

esp_err_t PixcMqttClient::eventHandler(esp_mqtt_event_handle_t event) {
  auto* self = static_cast<PixcMqttClient*>(event->user_context);
  if (self != nullptr) self->handle(event);
  return ESP_OK;
}

void PixcMqttClient::handle(esp_mqtt_event_handle_t event) {
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      _connected = true;
      if (_onConnect) _onConnect(event->session_present != 0);
      break;

    case MQTT_EVENT_DISCONNECTED:
      _connected = false;
      break;

    case MQTT_EVENT_DATA: {
      if (!_onMessage) break;
      // esp-mqtt hands out pointers into its own receive buffer, neither of which is
      // NUL-terminated. WLED's handler wants a C string for the topic and reassembles the payload
      // itself from index/total, so the topic is copied and terminated and the payload is passed
      // through as the chunk it is.
      //
      // A continuation chunk carries no topic at all (topic_len == 0) — the topic arrives only
      // with the first. Remembering it is what makes a payload larger than the receive buffer
      // survive: without this a long preset or config push would deliver its tail under an empty
      // topic and be dropped.
      if (event->topic_len > 0) {
        _lastTopic = String();
        _lastTopic.concat(event->topic, event->topic_len);
      }
      if (_lastTopic.length() == 0) break;

      AsyncMqttClientMessageProperties props;
      props.qos = static_cast<uint8_t>(event->qos);
      props.retain = event->retain;
      props.dup = event->dup;

      _onMessage(const_cast<char*>(_lastTopic.c_str()), event->data,
                 props, static_cast<size_t>(event->data_len),
                 static_cast<size_t>(event->current_data_offset),
                 static_cast<size_t>(event->total_data_len));
      break;
    }

    case MQTT_EVENT_ERROR:
      // Worth a line: a TLS failure here is otherwise a silent reconnect loop, and the two causes
      // that matter — a certificate the pinned roots do not cover, and credentials the broker
      // rejects — look identical from the outside without it.
      if (event->error_handle != nullptr) {
        DEBUG_PRINTF("[ePixC] MQTT error type=%d tls=%d sock_errno=%d\n",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->esp_tls_last_esp_err,
                     (int)event->error_handle->esp_transport_sock_errno);
      }
      break;

    default:
      break;
  }
}

bool PixcMqttClient::start() {
  if (_host.length() == 0) return false;

  esp_mqtt_client_config_t cfg = {};
  cfg.event_handle = &PixcMqttClient::eventHandler;
  cfg.user_context = this;
  cfg.host = _host.c_str();
  cfg.port = _port;
  cfg.keepalive = _keepAlive;
  // esp-mqtt defaults to a 1 KB buffer for both directions. AsyncMqttClient had no such limit, so
  // the default would silently shrink two things WLED already does: a full state publish with
  // several segments, and an inbound preset or config push. Inbound over the limit is chunked and
  // reassembled; **outbound over the limit simply fails**, which is the one that would have been
  // hard to see. 2 KB in, 4 KB out, at a one-off cost of about 6 KB of RAM.
  cfg.buffer_size = 2048;
  cfg.out_buffer_size = 4096;
  if (_clientId.length() > 0) cfg.client_id = _clientId.c_str();
  if (_user.length() > 0) cfg.username = _user.c_str();
  if (_pass.length() > 0) cfg.password = _pass.c_str();
  if (_willTopic.length() > 0) {
    cfg.lwt_topic = _willTopic.c_str();
    cfg.lwt_msg = _willPayload.c_str();
    cfg.lwt_qos = _willQos;
    cfg.lwt_retain = _willRetain ? 1 : 0;
  }
  if (_port == kMqttsPort) {
    cfg.transport = MQTT_TRANSPORT_OVER_SSL;
    // The same pinned roots the provisioning fetch uses. Not the certificate bundle: a bundle
    // trusts every public CA, which means any of them can mint a certificate for our broker.
    cfg.cert_pem = PIXC_TRUSTED_ROOTS;
  }

  _client = esp_mqtt_client_init(&cfg);
  if (_client == nullptr) return false;
  if (esp_mqtt_client_start(_client) != ESP_OK) {
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
    return false;
  }
  _dirty = false;
  _started = true;
  return true;
}

void PixcMqttClient::connect() {
  // esp-mqtt reads host, credentials and TLS settings once, at start. A changed broker — which is
  // what re-provisioning does — therefore needs a restart, not a reconnect.
  if (_client != nullptr && _dirty) {
    esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
    _connected = false;
    _started = false;
  }
  if (_client == nullptr) {
    start();
    return;
  }
  // Allocated but stopped. This is the path re-provisioning takes: the usermod calls
  // `disconnect()` and then `initMqtt()`, and when the broker has not actually changed there is
  // nothing dirty to trigger a rebuild — so without this the client would sit stopped forever and
  // the device would never come back to the broker it was already using.
  if (!_started) {
    _started = esp_mqtt_client_start(_client) == ESP_OK;
    return;
  }
  // Already running: esp-mqtt reconnects on its own, so there is nothing to do.
}

void PixcMqttClient::disconnect(bool) {
  if (_client == nullptr || !_started) return;
  esp_mqtt_client_stop(_client);
  _connected = false;
  _started = false;
}

uint16_t PixcMqttClient::publish(const char* topic, uint8_t qos, bool retain, const char* payload) {
  return publish(topic, qos, retain, payload, payload ? strlen(payload) : 0);
}

uint16_t PixcMqttClient::publish(const char* topic, uint8_t qos, bool retain, const char* payload,
                                 size_t length) {
  if (_client == nullptr || !_connected) return 0;
  int id = esp_mqtt_client_publish(_client, topic, payload, (int)length, qos, retain ? 1 : 0);
  // QoS 0 returns 0 on success, which is also the failure value. Only -1 is a failure, so it is
  // mapped to 0 and everything else to something non-zero, matching what callers expect.
  return id < 0 ? 0 : (uint16_t)(id == 0 ? 1 : id);
}

uint16_t PixcMqttClient::subscribe(const char* topic, uint8_t qos) {
  if (_client == nullptr) return 0;
  int id = esp_mqtt_client_subscribe(_client, topic, qos);
  return id < 0 ? 0 : (uint16_t)(id == 0 ? 1 : id);
}

#endif  // PIXC_MQTT_ESP_IDF
