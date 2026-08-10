#include "ProjectLoader.h"

namespace {
// Every asset path stored in project.json (icon `path`, SoftwareButton
// `pathNormal`/`pathActive`, MQTTIconField `valueIconPairs[].path`, a
// screen's flattened-background `path`) is written by the designer's
// export pipeline (lib/project-zip.ts) relative to the *zip root* (e.g.
// "assets/icon-lock.bmp") - the same convention e-paper's own export uses.
// ProjectInstaller::installProjectZipFromFile() always installs every zip
// entry under "/PROJECT/" (e.g. "/PROJECT/assets/icon-lock.bmp"), but
// nothing was ever prefixing these path fields to match before they
// reached ColorAssetLoader::drawBMPToCanvas() (a literal LittleFS.open()
// on whatever string it's given) - every icon/SoftwareButton/MQTTIconField
// silently fell into its own "asset missing" black-square placeholder,
// found 2026-08-10 building the M5 Dial HIL fixture (both a plain `icon`
// and an MQTTIconField rendered as solid black squares instead of their
// real bitmaps - the *load failure* fallback, not a pixel-content bug).
String resolveAssetPath(const String& path) {
  if (path.isEmpty() || path.charAt(0) == '/') return path;
  return "/PROJECT/" + path;
}
}  // namespace

ProjectLoader::ProjectLoader() : loaded_(false) {}

bool ProjectLoader::loadProject(const String& projectPath) {
  if (!LittleFS.exists(projectPath)) {
    return false;
  }
  
  File file = LittleFS.open(projectPath, "r");
  if (!file) {
    return false;
  }
  
  // Parse directly from file stream instead of loading entire file into String
  // This avoids doubling memory usage (String + JsonDocument)
  bool success = parseJSONFromFile(file);
  file.close();
  
  if (success) {
    loaded_ = true;
  }
  
  return success;
}

bool ProjectLoader::parseJSONFromFile(File& file) {
  // Parse directly from file stream to avoid loading entire JSON into memory
  size_t fileSize = file.size();
  size_t bufferSize = fileSize * 3 / 2;  // 1.5x file size
  // Floor was 32768 - an arbitrary "reasonable minimum" that had nothing to
  // do with what ArduinoJson actually needs for a small project, and on
  // this specific hardware, ESP.getMaxAllocHeap() at this point in boot
  // (right after WiFi connects, before this call) is a *deterministic*
  // 31732 bytes - not fragmentation noise, the same exact number across
  // every boot measured (2026-08-10, building the M5 Dial HIL fixture).
  // A 32768 floor is unconditionally 1036 bytes above that ceiling, so it
  // failed EVERY project load on this device, regardless of actual project
  // size - even a 598-byte fixture with a single box object. 4096 is still
  // generous slack over the 1.5x-of-actual-file-size estimate for anything
  // this floor could plausibly matter for (a tiny-but-deeply-nested
  // project), while leaving ~27KB of margin under the real ceiling instead
  // of exceeding it.
  if (bufferSize < 4096) bufferSize = 4096;

  // Check if we have enough memory
  if (ESP.getMaxAllocHeap() < bufferSize) {
    Serial.printf("[ProjectLoader] Not enough contiguous heap to load %s (need %u, have %u)\n",
                  file.name(), (unsigned)bufferSize, (unsigned)ESP.getMaxAllocHeap());
    return false;
  }
  
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  DynamicJsonDocument doc(bufferSize);
  #pragma GCC diagnostic pop
  
  // Parse directly from file stream
  DeserializationError error = deserializeJson(doc, file, 
                                                DeserializationOption::NestingLimit(30));
  
  if (error) {
    return false;
  }
  return parseJSONDocument(doc);
}

bool ProjectLoader::parseJSON(const String& jsonContent) {
  // Allocate sufficient memory for large project files with many objects
  // For 66KB JSON file, we need ~100KB buffer (1.5x rule of thumb)
  size_t bufferSize = jsonContent.length() * 3 / 2;
  // See parseJSONFromFile()'s identical fix (2026-08-10) for why this floor
  // was lowered from 32768 - it broke every project load on this hardware.
  if (bufferSize < 4096) bufferSize = 4096;
  
  // Suppress deprecation warning - DynamicJsonDocument is still the correct choice
  // for large, variable-sized JSON in ArduinoJson 7
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  DynamicJsonDocument doc(bufferSize);
  #pragma GCC diagnostic pop
  
  DeserializationError error = deserializeJson(doc, jsonContent, 
                                                DeserializationOption::NestingLimit(30));
  
  if (error) {
    return false;
  }
  
  return parseJSONDocument(doc);
}

bool ProjectLoader::parseJSONDocument(JsonDocument& doc) {
  // Parse basic project info
  project_.name = doc["name"] | "Unnamed Project";
  project_.screenWidth = doc["screenWidth"] | 300;
  project_.screenHeight = doc["screenHeight"] | 400;
  project_.exportColorDepth = doc["exportColorDepth"] | "1bit";
  
  // Parse topics
  if (doc["topics"].is<JsonArray>()) {
    parseTopics(doc["topics"].as<JsonArray>());
  }
  
  // Parse fonts
  if (doc["fonts"].is<JsonArray>()) {
    parseFonts(doc["fonts"].as<JsonArray>());
  }
  
  // Parse screens
  if (doc["screens"].is<JsonArray>()) {
    parseScreens(doc["screens"].as<JsonArray>());
  }

  // Parse hardware button defaults (project-wide, per physical button id)
  if (doc["hardwareButtons"].is<JsonArray>()) {
    parseHardwareButtons(doc["hardwareButtons"].as<JsonArray>());
  }

  return true;
}

void ProjectLoader::parseTopics(JsonArray topicsArray) {
  for (JsonObject topicJson : topicsArray) {
    MQTTTopic topic;
    topic.id = topicJson["id"] | "";
    topic.topic = topicJson["topic"] | "";
    topic.type = topicJson["type"] | "string";
    
    if (topicJson["examples"].is<JsonArray>()) {
      JsonArray examples = topicJson["examples"];
      for (JsonVariant ex : examples) {
        topic.examples.push_back(ex.as<String>());
      }
      // Use first example as initial value
      if (topic.examples.size() > 0) {
        topic.currentValue = topic.examples[0];
      }
    }

    // Only present when type == "json" - absent for every other topic and
    // for any project.json predating this feature (additive field).
    if (topicJson["subtopics"].is<JsonArray>()) {
      for (JsonObject subJson : topicJson["subtopics"].as<JsonArray>()) {
        JsonSubtopic sub;
        sub.id = subJson["id"] | "";
        sub.path = subJson["path"] | "";
        sub.type = subJson["type"] | "text";
        topic.subtopics.push_back(sub);
      }
    }

    project_.topics.push_back(topic);
  }
}

void ProjectLoader::parseFonts(JsonArray fontsArray) {
  project_.fonts.clear();
  
  for (JsonObject fontJson : fontsArray) {
    Font font;
    font.id = fontJson["id"] | "";
    font.name = fontJson["name"] | "";
    font.displayName = fontJson["displayName"] | "";
    font.internalName = fontJson["internalName"] | "";
    font.size = fontJson["size"] | 12;
    font.ascent = fontJson["ascent"] | 10;
    font.descent = fontJson["descent"] | 2;
    
    project_.fonts.push_back(font);
  }
}

void ProjectLoader::parseScreens(JsonArray screensArray) {
  for (JsonObject screenJson : screensArray) {
    Screen screen;
    screen.id = screenJson["id"] | "";
    screen.name = screenJson["name"] | "";
    screen.path = resolveAssetPath(screenJson["path"] | "");
    screen.backgroundColor = screenJson["backgroundColor"] | "#ffffff";

    if (screenJson["objects"].is<JsonArray>()) {
      JsonArray objectsArray = screenJson["objects"];
      for (JsonObject objJson : objectsArray) {
        ScreenObject obj = parseScreenObject(objJson);
        screen.objects.push_back(obj);
      }
    }

    // buttonActions is a JSON *object* keyed by button id (e.g. "btn-0"),
    // not an array - mirrors the designer's Record<string,
    // HardwareButtonAction> (components/screenman-editor.tsx), which
    // JSON.stringify's the same way.
    if (screenJson["buttonActions"].is<JsonObject>()) {
      screen.buttonActions = parseScreenButtonActions(screenJson["buttonActions"].as<JsonObject>());
    }

    project_.screens.push_back(screen);
  }
}

ScreenObject ProjectLoader::parseScreenObject(JsonObject objJson) {
  ScreenObject obj;
  obj.type = objJson["type"] | "";
  obj.id = objJson["id"] | "";
  obj.x = objJson["x"] | 0;
  obj.y = objJson["y"] | 0;
  obj.width = objJson["width"] | 0;
  obj.height = objJson["height"] | 0;
  obj.zIndex = objJson["zIndex"] | 0;
  obj.path = resolveAssetPath(objJson["path"] | "");
  obj.pathNormal = resolveAssetPath(objJson["pathNormal"] | "");
  obj.pathActive = resolveAssetPath(objJson["pathActive"] | "");

  if (objJson["properties"].is<JsonObject>()) {
    obj.properties = parseObjectProperties(objJson["properties"]);
  }

  // Recursive: only "tab-control" and "panel" objects carry children in
  // practice, but parsing is generic (matches the designer's ScreenmanObject
  // - any object type is technically allowed to have children). Absent for
  // every existing project (additive JSON field), so old projects parse
  // identically to before.
  if (objJson["children"].is<JsonArray>()) {
    for (JsonObject childJson : objJson["children"].as<JsonArray>()) {
      obj.children.push_back(parseScreenObject(childJson));
    }
  }

  return obj;
}

ObjectProperties ProjectLoader::parseObjectProperties(JsonObject propsJson) {
  ObjectProperties props;
  
  // Common properties
  props.topic = propsJson["topic"] | "";
  props.displayAs = propsJson["displayAs"] | "";
  props.backgroundColor = propsJson["backgroundColor"] | "#ffffff";
  // Matches the designer's fallback default exactly (render-text-box.ts,
  // render-level-indicator.ts both default to "#cccccc" when unset) - this
  // used to default to "#000000" here, a real discrepancy that just never
  // got exercised because every HIL test project so far always set
  // borderColor explicitly (2026-07-21 finding, level indicator work).
  props.borderColor = propsJson["borderColor"] | "#cccccc";
  props.textColor = propsJson["textColor"] | "#000000";
  props.color = propsJson["color"] | "#000000";  // Label color property
  props.textAlign = propsJson["textAlign"] | "left";
  props.fontId = propsJson["fontId"] | "";
  props.fontWeight = propsJson["fontWeight"] | "normal";
  
  // Label properties
  props.text = propsJson["text"] | "";
  props.fontSize = propsJson["fontSize"] | 12;
  
  // MqttDataField properties
  props.prefix = propsJson["prefix"] | "";
  props.postfix = propsJson["postfix"] | "";
  props.thousandsSeparator = propsJson["thousandsSeparator"] | "";
  props.numberOfDecimals = propsJson["numberOfDecimals"] | 2;
  
  // MQTTIconField properties
  if (propsJson["valueIconPairs"].is<JsonArray>()) {
    props.valueIconPairs = parseValueIconPairs(propsJson["valueIconPairs"]);
  }
  
  // level-indicator properties
  props.barDirection = propsJson["barDirection"] | "left-to-right";
  props.displayValue = propsJson["displayValue"] | "none";
  props.fillColor = propsJson["fillColor"] | "#4CAF50";
  
  if (propsJson["calibrationPoints"].is<JsonArray>()) {
    props.calibrationPoints = parseCalibrationPoints(propsJson["calibrationPoints"]);
  }

  // line properties
  props.strokeWidth = propsJson["strokeWidth"] | 1;
  props.strokeStyle = propsJson["strokeStyle"] | "solid";
  props.filletRadius = propsJson["filletRadius"] | 0;
  if (propsJson["points"].is<JsonArray>()) {
    props.points = parseLinePoints(propsJson["points"]);
  }
  props.arrowStart = propsJson["arrowStart"] | false;
  props.arrowEnd = propsJson["arrowEnd"] | false;

  // MqttDataLine properties - independent per-end arrow conditions, see
  // ProjectTypes.h's ObjectProperties comment for why these can't reuse
  // panel's single comparisonOperator/Value pair.
  props.arrowStartOperator = propsJson["arrowStartOperator"] | "<";
  props.arrowStartValue = propsJson["arrowStartValue"] | "0";
  props.arrowEndOperator = propsJson["arrowEndOperator"] | ">";
  props.arrowEndValue = propsJson["arrowEndValue"] | "0";

  // box properties - strokeColor defaults to empty (no border), matching
  // the designer's `if (strokeColor && strokeWidth > 0)` check exactly.
  props.strokeColor = propsJson["strokeColor"] | "";
  props.cornerRadius = propsJson["cornerRadius"] | 0;

  // panel properties - matched against the parent tab-control's own
  // `topic` (already parsed above). Default operator "==" covers the
  // common enum-tab case (e.g. mode == "TEMP").
  props.comparisonOperator = propsJson["comparisonOperator"] | "==";
  props.comparisonValue = propsJson["comparisonValue"] | "";

  return props;
}

std::vector<ValueIconPair> ProjectLoader::parseValueIconPairs(JsonArray pairsArray) {
  std::vector<ValueIconPair> pairs;
  
  for (JsonObject pairJson : pairsArray) {
    ValueIconPair pair;
    pair.id = pairJson["id"] | "";
    pair.comparisonOperator = pairJson["comparisonOperator"] | ">";
    // The designer stores this as a JSON *string* ("00", "01", ...), not a
    // JSON number - ArduinoJson's `| defaultValue` only substitutes the
    // default when is<float>() is false, and a string value never
    // satisfies is<float>() regardless of its content, so
    // `pairJson["value"] | 0.0f` silently returned 0.0f for every pair
    // here, every time, no matter what the string actually said. .as<
    // String>().toFloat() stringifies either a JSON string or number
    // correctly first, then parses it the same permissive way
    // getIconPathForValue()'s own valueStr.toFloat() already does.
    pair.value = pairJson["value"].as<String>().toFloat();
    pair.thenShowIcon = pairJson["thenShowIcon"] | "";
    pair.path = resolveAssetPath(pairJson["path"] | "");
    
    pairs.push_back(pair);
  }
  
  return pairs;
}

String ProjectLoader::getTopicValue(const String& topicOrPath) const {
  // topicOrPath may be a plain topic or a "<topic>#<path>" composite
  // referencing one field of a "json"-type topic's payload - splitting it
  // here (rather than requiring every caller to know about the "#"
  // convention) is what lets every existing caller of this function
  // (ScreenRenderer's condition evaluation, onMqttMessage's diff-check,
  // etc.) transparently support JSON subtopics with no changes of their
  // own. "#" can't appear in a real MQTT topic name (reserved wildcard
  // char in the spec), so it's a collision-free separator - the designer's
  // lib/json-path.ts splits the same way.
  String realTopic = topicOrPath;
  String path = "";
  int hashIndex = topicOrPath.indexOf('#');
  if (hashIndex >= 0) {
    realTopic = topicOrPath.substring(0, hashIndex);
    path = topicOrPath.substring(hashIndex + 1);
  }

  for (const auto& t : project_.topics) {
    if (t.topic == realTopic) {
      if (path.isEmpty()) {
        return t.currentValue;
      }
      return extractJsonField(t.currentValue, path);
    }
  }
  return "";
}

void ProjectLoader::setTopicValue(const String& topic, const String& value) {
  for (auto& t : project_.topics) {
    if (t.topic == topic) {
      t.currentValue = value;
      return;
    }
  }
}

namespace {

// One JSONPath shorthand segment - either a member-name access (from
// ".name", a bare leading "name", or a quoted "['name']"/"[\"name\"]") or
// an array index (from "[N]"). Mirrors lib/json-path.ts's PathSegment
// exactly - see that file's header comment for the full syntax this
// supports and why it's a genuine JSONPath subset rather than an invented
// look-alike.
struct JsonPathSegment {
  bool isIndex;
  String name;
  int index;
};

std::vector<JsonPathSegment> tokenizeJsonPath(const String& path) {
  String s = path;
  s.trim();
  if (s.startsWith("$")) {
    s = s.substring(1);
  }

  std::vector<JsonPathSegment> segments;
  int i = 0;
  int len = s.length();

  while (i < len) {
    if (s[i] == '.') {
      i++;
      String name = "";
      while (i < len && s[i] != '.' && s[i] != '[') {
        name += s[i];
        i++;
      }
      if (!name.isEmpty()) {
        segments.push_back({false, name, 0});
      }
    } else if (s[i] == '[') {
      int closeIndex = s.indexOf(']', i);
      if (closeIndex == -1) break;  // malformed - resolve what was parsed so far
      String inner = s.substring(i + 1, closeIndex);
      inner.trim();
      bool quoted = (inner.startsWith("'") && inner.endsWith("'")) || (inner.startsWith("\"") && inner.endsWith("\""));
      if (quoted) {
        segments.push_back({false, inner.substring(1, inner.length() - 1), 0});
      } else {
        segments.push_back({true, "", inner.toInt()});
      }
      i = closeIndex + 1;
    } else {
      // Bare leading name with no "." or "$" prefix - the ergonomic
      // shorthand this app's paths have always used ("temp" instead of
      // requiring "$.temp" or ".temp").
      String name = "";
      while (i < len && s[i] != '.' && s[i] != '[') {
        name += s[i];
        i++;
      }
      if (!name.isEmpty()) {
        segments.push_back({false, name, 0});
      }
    }
  }

  return segments;
}

}  // namespace

String ProjectLoader::extractJsonField(const String& jsonPayload, const String& path) const {
  if (path.isEmpty()) return jsonPayload;

  // Small, transient document - this runs per MQTT message on a payload
  // capped at MQTT_MAX_PACKET_SIZE (see MqttClient::connect()), not the
  // large one-time project.json parse that needs a pre-sized
  // DynamicJsonDocument.
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonPayload);
  if (error) return "";

  JsonVariant current = doc.as<JsonVariant>();

  for (const auto& segment : tokenizeJsonPath(path)) {
    if (current.isNull()) return "";
    if (segment.isIndex) {
      if (!current.is<JsonArray>()) return "";
      current = current[segment.index];
    } else {
      if (!current.is<JsonObject>()) return "";
      current = current[segment.name];
    }
  }

  if (current.isNull()) return "";

  // A path that stops partway through a nested structure (object/array)
  // gets JSON-stringified rather than yielding ArduinoJson's own
  // unhelpful default conversion - matches lib/json-path.ts's
  // extractJsonField on the designer side exactly, so a value that looks
  // right in the designer's preview is what this actually returns too.
  if (current.is<JsonObjectConst>() || current.is<JsonArrayConst>()) {
    String out;
    serializeJson(current, out);
    return out;
  }
  return current.as<String>();
}

std::vector<CalibrationPoint> ProjectLoader::parseCalibrationPoints(JsonArray pointsArray) {
  std::vector<CalibrationPoint> points;

  for (JsonObject pointJson : pointsArray) {
    CalibrationPoint point;
    point.value = pointJson["value"] | 0.0f;
    point.barSizePercent = pointJson["barSizePercent"] | 0.0f;

    points.push_back(point);
  }

  return points;
}

std::vector<Point> ProjectLoader::parseLinePoints(JsonArray pointsArray) {
  std::vector<Point> points;

  for (JsonObject pointJson : pointsArray) {
    Point point;
    point.x = pointJson["x"] | 0;
    point.y = pointJson["y"] | 0;

    points.push_back(point);
  }

  return points;
}

ButtonAction ProjectLoader::parseButtonAction(JsonObject actionJson) {
  ButtonAction action;
  action.type = actionJson["type"] | "";
  action.targetScreenId = actionJson["targetScreenId"] | "";
  action.mqttTopic = actionJson["mqttTopic"] | "";
  action.mqttMessage = actionJson["mqttMessage"] | "";
  return action;
}

std::vector<ScreenButtonAction> ProjectLoader::parseScreenButtonActions(JsonObject buttonActionsJson) {
  std::vector<ScreenButtonAction> result;

  for (JsonPair kv : buttonActionsJson) {
    ScreenButtonAction sba;
    sba.buttonId = kv.key().c_str();
    if (kv.value().is<JsonObject>()) {
      sba.action = parseButtonAction(kv.value().as<JsonObject>());
    }
    result.push_back(sba);
  }

  return result;
}

void ProjectLoader::parseHardwareButtons(JsonArray hardwareButtonsArray) {
  for (JsonObject btnJson : hardwareButtonsArray) {
    HardwareButtonDef def;
    def.id = btnJson["id"] | "";
    if (btnJson["defaultAction"].is<JsonObject>()) {
      def.defaultAction = parseButtonAction(btnJson["defaultAction"].as<JsonObject>());
    }
    project_.hardwareButtons.push_back(def);
  }
}

ButtonAction ProjectLoader::getButtonAction(int screenIndex, const String& buttonId) const {
  if (screenIndex >= 0 && screenIndex < (int)project_.screens.size()) {
    const Screen& screen = project_.screens[screenIndex];
    for (const auto& sba : screen.buttonActions) {
      if (sba.buttonId == buttonId) {
        return sba.action;
      }
    }
  }

  for (const auto& def : project_.hardwareButtons) {
    if (def.id == buttonId) {
      return def.defaultAction;
    }
  }

  return ButtonAction();
}

