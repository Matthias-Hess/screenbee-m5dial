#pragma once
#include <Arduino.h>
#include <functional>

// Handles the MQTT self-deploy flow (docs/device-contract.md, designer
// repo, §4's "Deploy-flow topics") - screenbee/<clientId>/deploy in,
// screenbee/<clientId>/deploy-status out. Referenced but never
// implemented here until 2026-08-10 (see MqttClient.h's getClientId()
// doc comment, ported verbatim from the e-paper firmware, which already
// mentioned "DeployManager" by name). Found missing when a real deploy
// from the designer got stuck at "Downloading" forever - the browser-side
// UI optimistically shows that the moment it publishes the trigger, with
// nothing on the device side ever picking it up or reporting back.
//
// Deliberately blocking (download+verify+install all happen inline
// inside handleDeployMessage(), not spread across multiple loop()
// iterations) - a deploy is a rare, deliberate action, not something
// loop() needs to stay responsive through, same reasoning as
// setupWiFi()'s own blocking WiFi.begin() wait.
//
// The `deploy` topic is retained (docs/device-contract.md §4) - the
// broker redelivers the last message on every fresh subscribe, which
// happens on every single reconnect (setupMQTT()'s subscribe() call gets
// replayed by MqttClient::connect() on every reconnect, not just the
// first). Confirmed live on real hardware (2026-08-10): a successful
// deploy calls ESP.restart(), which reconnects, which redelivers the same
// retained trigger, which deploys the same project again, which restarts
// again - forever. handleDeployMessage() clears the retained message via
// clearTrigger_ before doing anything else specifically to break this.
class DeployManager {
public:
  // publishStatus: caller-owned MQTT publish (deployId, state, error,
  // percent) - installed via setPublishStatusHandler() after construction,
  // matching TestInterfaceServer::setScreenSwitchHandler()'s identical
  // convention (this object is a global, constructed before mqttClient/
  // DEVICE_ID's topic-prefix pieces it would need to reference are
  // meaningful to capture - a setter called from setup() sidesteps any
  // global construction-order question entirely). percent is -1 for
  // states that don't carry one (matches docs/device-contract.md §4's
  // `percent?` being optional) - only "downloading" ever passes a real
  // value, reported from downloadToFile()'s own byte-count progress
  // (found 2026-08-10: deploy-dialog.tsx's progress bar reads
  // `status.percent ?? 0` and nothing else, so never publishing one at
  // all left it visibly stuck at 0% for the whole deploy despite it
  // actually working end-to-end).
  using PublishStatusFn = std::function<void(const String& deployId, const String& state, const String& error, int percent)>;
  void setPublishStatusHandler(PublishStatusFn handler) { publishStatus_ = handler; }

  // Publishes an empty retained payload to screenbee/<clientId>/deploy -
  // the only way to clear a *retained* MQTT message, called once at the
  // very start of handling a deploy (see handleDeployMessage()'s own
  // comment for why this is load-bearing, not cleanup busywork).
  using ClearTriggerFn = std::function<void()>;
  void setClearTriggerHandler(ClearTriggerFn handler) { clearTrigger_ = handler; }

  // Call once a message on screenbee/<clientId>/deploy has been received
  // - parses {deployId, url, crc32} and runs the full download -> verify
  // -> install sequence, publishing status at each step. Must be called
  // from loop(), *after* mqttClient.loop() has fully returned, never from
  // directly inside the MQTT message callback itself - see main.cpp's
  // onMqttMessage()/pendingDeploy comment for why (this method's own
  // publish() calls corrupt outgoing packets if they're still nested
  // inside PubSubClient's own receive-processing call stack, found
  // 2026-08-10 on real hardware). A deploy already in progress makes a
  // new one report "busy" instead of running concurrently.
  void handleDeployMessage(const String& payload);

private:
  PublishStatusFn publishStatus_;
  ClearTriggerFn clearTrigger_;
  bool busy_ = false;

  bool downloadToFile(const String& url, const String& destPath, std::function<void(int percent)> onProgress);
  bool verifyCrc32(const String& path, uint32_t expectedCrc32);
};
