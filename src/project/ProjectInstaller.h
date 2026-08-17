#pragma once
#include <Arduino.h>

// Shared "take a project zip already sitting in a file on LittleFS and turn
// it into the live /PROJECT directory" logic - ported ~verbatim from
// MqttEPaperDisplay2's ProjectInstaller (same miniz-based extraction, same
// /PROJECT layout convention) so TestInterfaceServer's project-upload
// endpoint doesn't have to reinvent it. Kept as its own namespace (not
// folded into TestInterfaceServer) in case a future MQTT self-deploy path
// for this device needs it too, same reason it's separate on the e-paper
// side.
namespace ProjectInstaller {

// Reads just project.json out of the zip at zipPath (without touching
// /PROJECT at all) and returns its top-level "deviceId" field, or an empty
// string if the zip/entry/field is missing or unparsable. Callers should
// treat an empty result as "unknown, don't trust it" rather than
// "compatible" - see DEVICE_ID in DeviceInfo.h for what this gets compared
// against.
String peekProjectDeviceId(const String& zipPath);

// Same idea as peekProjectDeviceId() above, for the export file format's
// own schemaVersion (docs/nested-provenance.md's "Version compatibility"
// section, designer repo) rather than a device's identity. Returns -1 if
// the zip/entry/field couldn't be read/parsed at all (treat as "unknown,
// don't trust it" - same convention as peekProjectDeviceId()'s empty
// string); a *present but absent field* returns 1, not -1 (schemaVersion
// 1 is the implicit version every export used before this field existed).
// Compare against EXPORT_SCHEMA_VERSION in DeviceInfo.h.
long peekProjectSchemaVersion(const String& zipPath);

// Wipes and recreates /PROJECT, extracts every entry from the zip at
// zipPath into it, and confirms project.json ended up there. Returns false
// (with errorOut set) on any failure. Not rollback-safe on its own -
// /PROJECT is already wiped by the time this can fail on extraction.
// Callers are expected to have already validated the zip (checksum,
// peekProjectDeviceId()) before calling this, so a failure here is
// expected to be rare (e.g. genuine flash trouble), not the normal way a
// bad upload gets rejected.
bool installProjectZipFromFile(const String& zipPath, String& errorOut);

}
