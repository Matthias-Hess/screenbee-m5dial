#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <vector>
#include <map>
#include <functional>
#include "interfaces/IMessagingClient.h"

/**
 * MQTT implementation of IMessagingClient
 * 
 * Responsibilities:
 * - Connect to MQTT broker with credentials
 * - Subscribe to topics
 * - Handle incoming messages via callback
 * - Maintain connection and auto-reconnect
 */
class MqttClient : public IMessagingClient {
public:
  MqttClient();
  
  /**
   * Configure MQTT broker connection
   * @param host Broker hostname or IP
   * @param port Broker port (default 1883)
   * @param username Username for authentication (optional)
   * @param password Password for authentication (optional)
   */
  void configure(const String& host, uint16_t port, const String& username = "", const String& password = "") override;
  
  /**
   * Set the callback for receiving messages
   */
  void setCallback(MessageCallback callback) override;
  
  /**
   * Connect to MQTT broker
   * @return true if connected
   */
  bool connect() override;
  
  /**
   * Check if connected to broker
   */
  bool isConnected() override;
  
  /**
   * Subscribe to a message key
   * @param key Message key to subscribe to
   * @return true if subscription successful
   */
  bool subscribe(const String& key) override;
  
  /**
   * Subscribe to multiple message keys
   * @param keys List of message keys to subscribe to
   */
  void subscribeToKeys(const std::vector<String>& keys) override;
  
  /**
   * Unsubscribe from a message key
   * @param key Message key to unsubscribe from
   * @return true if unsubscription successful
   */
  bool unsubscribe(const String& key) override;
  
  /**
   * Publish an action event
   * @param actionName Name of the action to publish
   */
  bool publishAction(const String& actionName) override;

  /**
   * Publish an arbitrary topic/payload pair
   */
  bool publish(const String& topic, const String& payload, bool retain = false) override;
  
  /**
   * Clear all subscriptions
   */
  void clearSubscriptions() override;
  
  /**
   * Process incoming messages - must be called regularly in loop()
   */
  void loop() override;

  /**
   * This client's own stable identity ("M5Dial-" + MAC-derived hex) - also
   * used as the MQTT topic namespace segment for the deploy/hello/status
   * topics (see Application::setupMQTT() and DeployManager), so a deploy
   * trigger/discovery message always lines up with the same id this client
   * connects under.
   */
  String getClientId() const { return clientId_; }

  /**
   * Fires every time connect() succeeds - both the initial connect() call
   * and every automatic reconnect via loop()'s reconnect() path, since a
   * dropped connection also means any MQTT Last Will (see connect()'s
   * will-bearing overload below) fired "offline" on the broker, so
   * whatever needs to be freshly (re-)announced - status=online, the
   * "hello" payload - must happen again each time, not just once at boot.
   */
  using ConnectedCallback = std::function<void()>;
  void setConnectedCallback(ConnectedCallback callback) { connectedCallback_ = callback; }

private:
  WiFiClient wifiClient_;
  PubSubClient mqttClient_;
  MessageCallback messageCallback_;
  
  String host_;
  uint16_t port_;
  String username_;
  String password_;
  String clientId_;
  
  std::vector<String> subscribedTopics_;

  // Action counters (one per action name)
  std::map<String, unsigned long> actionCounters_;

  ConnectedCallback connectedCallback_;
  
  // Static callback for PubSubClient (forwards to instance method)
  static void staticCallback(char* topic, byte* payload, unsigned int length);
  static MqttClient* instance_;
  
  // Instance callback
  void handleMessage(char* topic, byte* payload, unsigned int length);
  
  // Auto-reconnect
  void reconnect();
  unsigned long lastReconnectAttempt_;
  static const unsigned long RECONNECT_INTERVAL_MS = 5000;
};

