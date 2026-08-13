#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "../interfaces/IProjectLoader.h"
#include "ProjectTypes.h"

/**
 * Loads and parses project.json from LittleFS
 * Implements IProjectLoader interface
 */
class ProjectLoader : public IProjectLoader {
public:
  ProjectLoader();
  
  // IProjectLoader interface
  bool loadProject(const String& projectPath = "/PROJECT/project.json") override;
  const ProjectConfig& getProject() const override { return project_; }
  bool isLoaded() const override { return loaded_; }
  String getTopicValue(const String& topic) const override;
  void setTopicValue(const String& topic, const String& value) override;
  ButtonAction getButtonAction(int screenIndex, const String& buttonId) const override;

private:
  ProjectConfig project_;
  bool loaded_;

  // Parsing helpers
  bool parseJSON(const String& jsonContent);
  bool parseJSONFromFile(File& file);
  bool parseJSONDocument(JsonDocument& doc);
  void parseTopics(JsonArray topicsArray);
  void parseFonts(JsonArray fontsArray);
  void parseScreens(JsonArray screensArray);
  ScreenObject parseScreenObject(JsonObject objJson);
  ObjectProperties parseObjectProperties(JsonObject propsJson);
  std::vector<ValueIconPair> parseValueIconPairs(JsonArray pairsArray);
  std::vector<SwitchState> parseSwitchStates(JsonArray statesArray);
  std::vector<CalibrationPoint> parseCalibrationPoints(JsonArray pointsArray);
  std::vector<Point> parseLinePoints(JsonArray pointsArray);
  ButtonAction parseButtonAction(JsonObject actionJson);
  std::vector<ScreenButtonAction> parseScreenButtonActions(JsonObject buttonActionsJson);
  void parseHardwareButtons(JsonArray hardwareButtonsArray);

  // Extracts the field at `path` (dot-notation, optional [N] array
  // indices - see ProjectTypes.h's JsonSubtopic) out of a JSON payload
  // string. Mirrors the designer's lib/json-path.ts getJsonPathValue()
  // exactly: a missing field, an out-of-range index, or indexing into a
  // non-object/non-array all just yield "" rather than asserting - a
  // stale or malformed path is a configuration problem to surface as "no
  // value", not a crash.
  String extractJsonField(const String& jsonPayload, const String& path) const;
};





