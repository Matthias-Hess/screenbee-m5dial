#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <functional>
#include "ClippedCanvas16.h"

// HTTP endpoints an external HIL test orchestrator needs to drive this
// device the same way hil/epaper/orchestrator.js drives MqttEPaperDisplay2
// - see docs/device-contract.md (designer repo) §6 for the testInterface
// contract this implements, and §8 for why it didn't exist until now (no
// project-upload/screen-switch/snapshot endpoint at all meant every
// checkpoint-4 verification had to be eyes-on-the-real-screen).
//
// Unlike the e-paper firmware (DisplaySnapshot on port 8080, always-on;
// UnifiedConfigurator's /api/project on port 80, setup-mode-gated unless a
// HIL-only build flag opens it permanently), every endpoint here - project
// upload included - lives on one always-on server once WiFi connects. This
// device has no "field deployment hardening" story yet (see
// docs/device-contract.md §8's open items), so there's no safety property
// being traded away by not gating upload behind a physical-button setup
// mode the way e-paper does - revisit if/when that changes.
class TestInterfaceServer {
public:
  TestInterfaceServer(ClippedCanvas16* canvas, uint16_t port = 80);

  // Only call once WiFi is connected - mirrors DisplaySnapshot::start()'s
  // own guard, same reasoning (nothing useful to serve without an IP to
  // reach it on).
  bool start();
  void handleClient();
  bool isRunning() const { return serverRunning_; }

  // Install the handler that actually switches/renders a screen for
  // POST /api/screen. Takes a screen index, returns true on success (index
  // valid and render completed). Always a full render - test snapshots
  // need to be reproducible, not dependent on whatever was on screen
  // before. Owned by main.cpp (which owns ProjectLoader/ColorScreenRenderer),
  // same split as DisplaySnapshot's setScreenSwitchHandler.
  void setScreenSwitchHandler(std::function<bool(int)> handler) { screenSwitchHandler_ = handler; }

  // Install the handler that looks up a single topic's currently-cached
  // value for GET /api/topic-values - lets a HIL client confirm the device
  // actually received/applied a just-published MQTT value before forcing a
  // render, instead of guessing a fixed settle delay (the same race the
  // e-paper firmware's own /api/topic-values was built to close - see
  // hil/README.md's "combo 0 immediately after a fresh upload" writeup).
  void setGetTopicValueHandler(std::function<String(const String&)> handler) { getTopicValueHandler_ = handler; }

private:
  ClippedCanvas16* canvas_;
  WebServer* webServer_;
  uint16_t port_;
  bool serverRunning_;

  std::function<bool(int)> screenSwitchHandler_;
  std::function<String(const String&)> getTopicValueHandler_;

  File uploadFile_;

  void sendJSONResponse(bool success, const String& message);

  // POST /api/project - multipart-zip upload (uploadContentType in the DDF's
  // testInterface). Two-phase like every WebServer file upload: the upload
  // handler (Chunk) streams UPLOAD_FILE_START/WRITE/END to a temp file and
  // does the real validate+extract+restart work at END; the request handler
  // (Complete) just sends a generic response once that's done, mirroring
  // MqttEPaperDisplay2's UnifiedConfigurator::handleProjectUpload/
  // handleProjectUploadFile split exactly.
  void handleProjectUploadChunk();
  void handleProjectUploadComplete();
  bool validateAndExtractZip();

  // POST /api/screen - form-urlencoded "index=N" (screenSwitchBody in the
  // DDF's testInterface, matching every other control endpoint in this
  // codebase's convention - not a JSON body).
  void handleScreenSwitch();

  // GET /snapshot.bmp - the current canvas contents (whatever was last
  // rendered, no separate shadow-buffer capture needed like the e-paper
  // firmware's DisplaySnapshot - ClippedCanvas16/GFXcanvas16 already holds
  // the full rendered RGB565 frame persistently in canvas_->getBuffer()).
  void handleSnapshot();
  void sendBMP();
  void generateBMPHeader(uint8_t* header, int width, int height, int dataSize);

  // GET /api/topic-values?topics=a,b,c -> {"a":"1","b":"2",...} - see
  // setGetTopicValueHandler's comment. Comma-separated rather than repeated
  // query params, matching DisplaySnapshot::handleGetTopicValues exactly -
  // WebServer's arg() only returns the last value for a repeated key.
  void handleGetTopicValues();
};
