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

