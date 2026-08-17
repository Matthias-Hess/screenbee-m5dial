#!/usr/bin/env node
// Regenerates src/ddf_zip.h from this repo's own ddf-source/ (the DDF's
// real editable source - device.json + adornment.svg + fonts/*.bdf), and
// keeps DeviceInfo.h's DDF_VERSION in sync with the version inside it.
//
// This used to read a pre-built zip from the designer repo
// (v0-screenman-editor-design/public/ddf/m5stack-m5dial.ddf.zip) - that
// stopped being the DDF's source of truth on 2026-08-16: the designer
// carries no baked-in device knowledge, and adornment.svg is now
// hand-edited only here, in ddf-source/. This script builds the zip
// itself instead of importing one.
//
// This used to be "a one-off script" that was never checked in, which is
// exactly why designer/firmware versions drifted before: the designer's
// DDF went 1.4 -> 1.5 -> 1.6 while this firmware kept announcing and
// serving 1.4, so a designer that picked this device from "Announced
// Devices" got a DDF missing object types the firmware had actually
// gained.
//
// Run after any change to ddf-source/, then rebuild and flash - the
// header is compiled in, so a rebuild is what actually publishes it:
//
//   node tools/generate-ddf-header.js
//   pio run -e m5dial -t upload
//
// Pass --check to verify without writing (exit 1 on drift).

const fs = require("fs")
const path = require("path")
const zlib = require("zlib")

const REPO = path.join(__dirname, "..")
const DEFAULT_SOURCE = path.join(REPO, "ddf-source")
const HEADER = path.join(REPO, "src", "ddf_zip.h")
const DEVICE_INFO = path.join(REPO, "src", "DeviceInfo.h")

const checkOnly = process.argv.includes("--check")
const sourceDir = process.argv.slice(2).find((a) => !a.startsWith("--")) || DEFAULT_SOURCE

if (!fs.existsSync(sourceDir) || !fs.existsSync(path.join(sourceDir, "device.json"))) {
  console.error(`DDF source not found: ${sourceDir} (expected a device.json in it)`)
  process.exit(1)
}

// --- Minimal in-memory ZIP writer (Node built-ins only - this repo has no
// package.json/node_modules, and the old version of this script already
// hand-rolled zip *reading* the same way, so a hand-rolled *writer* stays
// consistent with that rather than introducing a dependency for one script). ---

// Standard ZIP/PKZIP CRC-32 (same polynomial as gzip/PNG).
const CRC_TABLE = (() => {
  const table = new Uint32Array(256)
  for (let n = 0; n < 256; n++) {
    let c = n
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
    table[n] = c >>> 0
  }
  return table
})()

function crc32(buf) {
  let c = 0xffffffff
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8)
  return (c ^ 0xffffffff) >>> 0
}

// Fixed DOS date/time (1980-01-01 00:00:00, the format's own epoch) for
// every entry instead of the real mtime - regenerating from unchanged
// source must produce byte-identical output, or --check would spuriously
// report "stale" on every run just from wall-clock drift.
const DOS_TIME = 0
const DOS_DATE = (1 << 5) | 1 // day=1, month=1, year offset=0 -> 1980-01-01

function buildZip(entries) {
  // entries: [{ name, data: Buffer }], already in the exact order to write.
  const localParts = []
  const centralParts = []
  let offset = 0

  for (const { name, data } of entries) {
    const nameBuf = Buffer.from(name, "utf8")
    const crc = crc32(data)
    const compressed = zlib.deflateRawSync(data)
    const useStore = compressed.length >= data.length
    const method = useStore ? 0 : 8
    const payload = useStore ? data : compressed

    const localHeader = Buffer.alloc(30)
    localHeader.writeUInt32LE(0x04034b50, 0)
    localHeader.writeUInt16LE(20, 4) // version needed
    localHeader.writeUInt16LE(0, 6) // flags
    localHeader.writeUInt16LE(method, 8)
    localHeader.writeUInt16LE(DOS_TIME, 10)
    localHeader.writeUInt16LE(DOS_DATE, 12)
    localHeader.writeUInt32LE(crc, 14)
    localHeader.writeUInt32LE(payload.length, 18)
    localHeader.writeUInt32LE(data.length, 22)
    localHeader.writeUInt16LE(nameBuf.length, 26)
    localHeader.writeUInt16LE(0, 28) // extra field length
    localParts.push(localHeader, nameBuf, payload)

    const centralHeader = Buffer.alloc(46)
    centralHeader.writeUInt32LE(0x02014b50, 0)
    centralHeader.writeUInt16LE(20, 4) // version made by
    centralHeader.writeUInt16LE(20, 6) // version needed
    centralHeader.writeUInt16LE(0, 8) // flags
    centralHeader.writeUInt16LE(method, 10)
    centralHeader.writeUInt16LE(DOS_TIME, 12)
    centralHeader.writeUInt16LE(DOS_DATE, 14)
    centralHeader.writeUInt32LE(crc, 16)
    centralHeader.writeUInt32LE(payload.length, 20)
    centralHeader.writeUInt32LE(data.length, 24)
    centralHeader.writeUInt16LE(nameBuf.length, 28)
    centralHeader.writeUInt16LE(0, 30) // extra field length
    centralHeader.writeUInt16LE(0, 32) // comment length
    centralHeader.writeUInt16LE(0, 34) // disk number start
    centralHeader.writeUInt16LE(0, 36) // internal attrs
    centralHeader.writeUInt32LE(0, 38) // external attrs
    centralHeader.writeUInt32LE(offset, 42) // local header offset
    centralParts.push(centralHeader, nameBuf)

    offset += localHeader.length + nameBuf.length + payload.length
  }

  const centralDirStart = offset
  const centralDir = Buffer.concat(centralParts)

  const eocd = Buffer.alloc(22)
  eocd.writeUInt32LE(0x06054b50, 0)
  eocd.writeUInt16LE(0, 4) // disk number
  eocd.writeUInt16LE(0, 6) // disk with central dir
  eocd.writeUInt16LE(entries.length, 8)
  eocd.writeUInt16LE(entries.length, 10)
  eocd.writeUInt32LE(centralDir.length, 12)
  eocd.writeUInt32LE(centralDirStart, 16)
  eocd.writeUInt16LE(0, 20) // comment length

  return Buffer.concat([...localParts, centralDir, eocd])
}

// Walk sourceDir recursively, collecting { name, data } in a fixed,
// deterministic order (sorted by zip-entry name) - required for byte-stable
// output, same reasoning as the fixed timestamp above.
function collectEntries(dir, prefix = "") {
  const out = []
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name)
    const zipName = prefix + entry.name
    if (entry.isDirectory()) {
      out.push(...collectEntries(full, zipName + "/"))
    } else {
      out.push({ name: zipName, data: fs.readFileSync(full) })
    }
  }
  out.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
  return out
}

const entries = collectEntries(sourceDir)
const version = JSON.parse(fs.readFileSync(path.join(sourceDir, "device.json"), "utf8")).ddfVersion
const zip = buildZip(entries)

const lines = []
for (let i = 0; i < zip.length; i += 16) {
  const row = Array.from(zip.subarray(i, i + 16))
    .map((b) => `0x${b.toString(16).padStart(2, "0")}`)
    .join(", ")
  lines.push(`  ${row},`)
}

const header = `#pragma once
// Built from this repo's own ddf-source/ (device.json + adornment.svg +
// fonts/*.bdf) - GENERATED by tools/generate-ddf-header.js, do not
// hand-edit ddf_zip.h itself, edit ddf-source/ and regenerate. Served at
// GET /ddf.zip (TestInterfaceServer) so the designer's
// device-scan-section.tsx can fetch+cache this device's own DDF directly
// from a live device, the same way the e-paper firmware already does.
//
// Regenerate whenever ddf-source/ changes - the generator also bumps
// DeviceInfo.h's DDF_VERSION, which is what tells the designer a cached
// copy is stale and needs refetching.
//
// DDF version embedded here: ${version}
static const uint8_t DDF_ZIP[${zip.length}] = {
${lines.join("\n")}
};
`

const existingHeader = fs.existsSync(HEADER) ? fs.readFileSync(HEADER, "utf8") : ""
const deviceInfo = fs.readFileSync(DEVICE_INFO, "utf8")
const versionPattern = /#define DDF_VERSION "([^"]*)"/
const currentVersion = (deviceInfo.match(versionPattern) || [])[1]

if (checkOnly) {
  const headerStale = existingHeader !== header
  const versionStale = currentVersion !== version
  if (headerStale || versionStale) {
    if (headerStale) console.error(`src/ddf_zip.h is stale (DDF is ${zip.length} bytes, version ${version})`)
    if (versionStale) console.error(`DeviceInfo.h DDF_VERSION is "${currentVersion}", DDF says "${version}"`)
    console.error("Run: node tools/generate-ddf-header.js")
    process.exit(1)
  }
  console.log(`Up to date (DDF version ${version}, ${zip.length} bytes)`)
  process.exit(0)
}

fs.writeFileSync(HEADER, header)
if (currentVersion !== version) {
  fs.writeFileSync(DEVICE_INFO, deviceInfo.replace(versionPattern, `#define DDF_VERSION "${version}"`))
  console.log(`DDF_VERSION ${currentVersion} -> ${version}`)
}
console.log(`Wrote src/ddf_zip.h (${zip.length} bytes) from ${sourceDir}`)
