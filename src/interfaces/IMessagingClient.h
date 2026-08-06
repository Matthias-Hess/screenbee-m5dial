#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>

/**
 * Abstract messaging interface for publish/subscribe communication
 * 
 * Transport-agnostic interface that supports multiple messaging implementations.
 * Provides the same conceptual operations regardless of underlying transport.
 * 
 * Responsibilities:
 * - Connect to messaging broker/service with credentials
 * - Subscribe to message keys
 * - Handle incoming messages via callback
 * - Maintain connection and auto-reconnect
 * - Publish and receive messages
 */
class IMessagingClient {
public:
  // Callback type for receiving messages: void callback(key, payload)
  using MessageCallback = std::function<void(const String&, const String&)>;
  
  virtual ~IMessagingClient() = default;
  
  /**
   * Configure messaging broker/service connection
   * @param host Service hostname or IP
   * @param port Service port (may be ignored by some implementations)
   * @param username Username for authentication (optional)
   * @param password Password for authentication (optional)
   */
  virtual void configure(const String& host, uint16_t port, const String& username = "", const String& password = "") = 0;
  
  /**
   * Set the callback for receiving messages
   */
  virtual void setCallback(MessageCallback callback) = 0;
  
  /**
   * Connect to messaging service
   * @return true if connected
   */
  virtual bool connect() = 0;
  
  /**
   * Check if connected to broker/service
   */
  virtual bool isConnected() = 0;
  
  /**
   * Subscribe to a message key
   * @param key Message key to subscribe to
   * @return true if subscription successful
   */
  virtual bool subscribe(const String& key) = 0;
  
  /**
   * Subscribe to multiple message keys
   * @param keys List of message keys to subscribe to
   */
  virtual void subscribeToKeys(const std::vector<String>& keys) = 0;
  
  /**
   * Unsubscribe from a message key
   * @param key Message key to unsubscribe from
   * @return true if unsubscription successful
   */
  virtual bool unsubscribe(const String& key) = 0;
  
  /**
   * Publish an action event
   * @param actionName Name of the action to publish
   */
  virtual bool publishAction(const String& actionName) = 0;

  /**
   * Publish an arbitrary topic/payload pair - unlike publishAction (which
   * always sends an auto-incrementing counter as the payload), this sends
   * exactly the payload given. Used for a hardware button configured with
   * a "send-mqtt" action (designer-specified topic and message).
   * @param topic Topic to publish to
   * @param payload Exact payload to send
   * @param retain Whether the broker should retain this message
   */
  virtual bool publish(const String& topic, const String& payload, bool retain = false) = 0;
  
  /**
   * Clear all subscriptions
   */
  virtual void clearSubscriptions() = 0;
  
  /**
   * Process incoming messages - must be called regularly in loop()
   */
  virtual void loop() = 0;
};

