#include "MqttClient.h"
#include "DeviceInfo.h"

// Static instance pointer for callback forwarding
MqttClient* MqttClient::instance_ = nullptr;

MqttClient::MqttClient() 
  : mqttClient_(wifiClient_),
    port_(1883),
    lastReconnectAttempt_(0) {
  
  // Generate unique client ID
  clientId_ = "M5Dial-" + String(ESP.getEfuseMac(), HEX);
  
  // Set static instance for callback forwarding
  instance_ = this;

  // Set PubSubClient callback
  mqttClient_.setCallback(staticCallback);

  // PubSubClient defaults to a 256-byte buffer for topic+payload+overhead
  // combined (MQTT_MAX_PACKET_SIZE) - too small for a realistic multi-field
  // JSON payload (a "json"-type Topic's message, see ProjectTypes.h's
  // MQTTTopic), which would otherwise just get silently truncated/dropped.
  // Must be set before connect() - PubSubClient allocates the buffer at
  // that size up front, so this belongs here at construction, not later.
  mqttClient_.setBufferSize(1024);

  // PubSubClient's default 15s keepalive (docs/device-contract.md's
  // shared broker also runs a dozen+ other MQTT clients, e.g. Home
  // Assistant's mqttthing integrations, all configured with a 60s
  // keepalive) made this device far more sensitive to any transient
  // broker/network hiccup than everything else on the same broker -
  // found 2026-08-10 as the real cause of a live-published MQTT message
  // (a deploy trigger, specifically) intermittently never reaching this
  // device at all: `mosquitto` logs showed it reconnecting every ~5-15s
  // fairly continuously, meaning it spent a meaningful fraction of time
  // in a disconnected gap where nothing could be delivered. A message
  // published *while already connected* could land in exactly that gap
  // and simply never arrive - the same trigger DOES eventually get
  // through if the device (or its connection) resets, since the trigger
  // is retained and gets redelivered on the next successful subscribe,
  // which is what made this look like "works after a manual reset,
  // silently drops otherwise" rather than a clean pass/fail. Matching the
  // other clients' 60s keepalive gives this device the same tolerance for
  // brief hiccups they already have.
  mqttClient_.setKeepAlive(60);
}

void MqttClient::configure(const String& host, uint16_t port, const String& username, const String& password) {
  host_ = host;
  port_ = port;
  username_ = username;
  password_ = password;
  
  mqttClient_.setServer(host_.c_str(), port_);
}

void MqttClient::setCallback(MessageCallback callback) {
  messageCallback_ = callback;
}

bool MqttClient::connect() {
  if (host_.isEmpty()) {
    return false;
  }
  
  if (!WiFi.isConnected()) {
    return false;
  }

  // Last Will and Testament: the broker publishes this (retained) on our
  // behalf the moment our TCP connection drops without a clean
  // disconnect - the only way a subscriber (the designer's deploy dialog)
  // can tell "was online, went away unexpectedly" apart from "still
  // connected" without waiting for a timeout of its own. We publish the
  // "online" counterpart ourselves right below, once connect() actually
  // succeeds.
  String statusTopic = String(TOPIC_PREFIX) + "/" + clientId_ + "/status";
  bool connected = mqttClient_.connect(
    clientId_.c_str(),
    username_.isEmpty() ? nullptr : username_.c_str(),
    username_.isEmpty() ? nullptr : password_.c_str(),
    statusTopic.c_str(),
    1,     // willQos
    true,  // willRetain
    "offline"
  );

  if (connected) {
    mqttClient_.publish(statusTopic.c_str(), "online", true);

    // Re-subscribe to keys after connection
    if (!subscribedTopics_.empty()) {
      for (const String& key : subscribedTopics_) {
        mqttClient_.subscribe(key.c_str());
      }
    }

    if (connectedCallback_) connectedCallback_();
  }

  return connected;
}

bool MqttClient::isConnected() {
  return mqttClient_.connected();
}

bool MqttClient::subscribe(const String& key) {
  if (key.isEmpty()) {
    return false;
  }
  
  // Check if already subscribed
  for (const String& subscribedKey : subscribedTopics_) {
    if (subscribedKey == key) {
      if (!isConnected()) {
        return true;
      }
      return mqttClient_.subscribe(key.c_str());
    }
  }
  
  subscribedTopics_.push_back(key);

  if (!isConnected()) {
    return true;
  }

  return mqttClient_.subscribe(key.c_str());
}

void MqttClient::subscribeToKeys(const std::vector<String>& keys) {
  for (const String& key : keys) {
    subscribe(key);
  }
}

bool MqttClient::unsubscribe(const String& key) {
  if (!isConnected()) {
    return false;
  }
  
  bool success = mqttClient_.unsubscribe(key.c_str());
  
  if (success) {
    // Remove from subscribed list
    for (auto it = subscribedTopics_.begin(); it != subscribedTopics_.end(); ++it) {
      if (*it == key) {
        subscribedTopics_.erase(it);
        break;
      }
    }
  }
  
  return success;
}

bool MqttClient::publishAction(const String& actionName) {
  if (actionName.isEmpty()) {
    return false;
  }

  if (!isConnected()) {
    return false;
  }

  // Increment counter for this action (or initialize to 1 if new)
  actionCounters_[actionName]++;
  unsigned long counter = actionCounters_[actionName];

  // Convert counter to string payload
  String payload = String(counter);

  // Publish: topic = actionName, payload = counter value, retain = false
  bool success = mqttClient_.publish(actionName.c_str(), payload.c_str(), false);
  return success;
}

bool MqttClient::publish(const String& topic, const String& payload, bool retain) {
  if (topic.isEmpty() || !isConnected()) {
    return false;
  }

  return mqttClient_.publish(topic.c_str(), payload.c_str(), retain);
}

void MqttClient::clearSubscriptions() {
  for (const String& key : subscribedTopics_) {
    mqttClient_.unsubscribe(key.c_str());
  }
  subscribedTopics_.clear();
}

void MqttClient::loop() {
  if (!host_.isEmpty()) {
    // Try to reconnect if disconnected
    if (!mqttClient_.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt_ > RECONNECT_INTERVAL_MS) {
        // Temporary diagnostic (2026-08-10): this device was found
        // reconnecting to the broker roughly every 5s continuously
        // (matching RECONNECT_INTERVAL_MS exactly), which explains why a
        // live-published MQTT message (e.g. a deploy trigger) so often
        // failed to reach it - it was almost never in a stable, long-lived
        // connected state. PubSubClient::state() reports why *it* thinks
        // the connection is down (MQTT_CONNECTION_TIMEOUT=-4 means a
        // PINGREQ went unanswered, MQTT_CONNECTION_LOST=-3 means the
        // underlying socket read failed, etc. - see PubSubClient.h's own
        // #defines). Logged right here (throttled to the same cadence as
        // the reconnect attempt itself), not on every loop() iteration,
        // to avoid flooding the log while waiting out the throttle.
        Serial.printf("[MqttClient] Disconnected, PubSubClient state=%d\n", mqttClient_.state());
        lastReconnectAttempt_ = now;
        reconnect();
      }
    } else {
      // Process incoming messages
      mqttClient_.loop();
    }
  }
}

void MqttClient::reconnect() {
  if (WiFi.isConnected()) {
    connect();
  }
}

void MqttClient::staticCallback(char* topic, byte* payload, unsigned int length) {
  if (instance_) {
    instance_->handleMessage(topic, payload, length);
  }
}

void MqttClient::handleMessage(char* topic, byte* payload, unsigned int length) {
  // Convert payload to String
  String payloadStr;
  payloadStr.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    payloadStr += (char)payload[i];
  }
  
  // Call user callback
  if (messageCallback_) {
    messageCallback_(String(topic), payloadStr);
  }
}

