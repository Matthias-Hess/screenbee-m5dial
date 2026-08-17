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

// Must match public/ddf/m5stack-m5dial.ddf.zip's own device.json
// "ddfVersion" exactly (2026-08-10) - announced in "hello" alongside a
// GET /ddf.zip URL (TestInterfaceServer::handleDdfZip(), serving
// ddf_zip.h - regenerate that header whenever the DDF file changes) so
// the designer's device-scan-section.tsx can fetch+cache this device's
// DDF straight from a live device, matching the e-paper firmware's own
// "announced device" support. Bump this whenever the DDF changes so a
// stale cached copy gets refetched.
#define DDF_VERSION "1.9"

// The device-facing export file format's own schemaVersion this firmware
// understands (2026-08-15, docs/nested-provenance.md's "Version
// compatibility" section, designer repo) - a single integer, separate
// from DDF_VERSION above (that's this device's own *capabilities*; this
// is whether the export's project.json *shape* can be parsed at all).
// Bumped only on a real structural break in what lib/project-zip.ts's
// buildDeviceProjectZip() produces; additive fields never need this
// bumped. DeployManager.cpp rejects any deploy whose own schemaVersion is
// higher than this, before touching /PROJECT.
#define EXPORT_SCHEMA_VERSION 1

// Where DeployManager.cpp promotes a verified deploy download to, and
// TestInterfaceServer.cpp's recovery endpoint (GET /recovery-project)
// reads back from - shared here (rather than staying private to
// DeployManager.cpp, as DEPLOY_DOWNLOAD_PATH still is) specifically so both
// files agree on it. See docs/nested-provenance.md's "Version
// compatibility" > Fall 3 (designer repo).
#define RECOVERY_PROJECT_PATH "/recovery_project.zip"

// The MQTT topic namespace every device/browser participant in the deploy
// flow shares (screenbee/<clientId>/...) - same shared protocol as every
// other ScreenBee device, not device-specific.
#define TOPIC_PREFIX "screenbee"
