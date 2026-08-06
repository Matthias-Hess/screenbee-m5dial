#pragma once

// This device's own identity, compiled in - mirrors MqttEPaperDisplay2's
// DeviceInfo.h. DEVICE_ID must match the designer repo's
// public/ddf/m5stack-m5dial.ddf.zip device.json's "device.id" exactly -
// how a device tells a project "you weren't built for me" before applying
// it (once checkpoint 4 builds that check), and how it identifies its own
// type in its MQTT "hello" announcement.
#define DEVICE_ID "m5stack-m5dial-v1-1"

// Freeform, only used in the MQTT "hello" payload for humans/tooling to
// see what's running - not parsed/compared against anything.
#define FIRMWARE_VERSION "0.1.0"

// No DDF_VERSION/url in "hello" yet - unlike the e-paper firmware, this
// device doesn't serve its own DDF over HTTP yet (that's tied to
// checkpoint 4's self-deploy work, which is also when a stale-DDF-version
// check would first matter). Add both once that exists - advertising a
// version with nothing to fetch would be worse than not advertising one.

// The MQTT topic namespace every device/browser participant in the deploy
// flow shares (screenbee/<clientId>/...) - same shared protocol as every
// other ScreenBee device, not device-specific.
#define TOPIC_PREFIX "screenbee"
