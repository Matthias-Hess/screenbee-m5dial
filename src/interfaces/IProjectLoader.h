#pragma once
#include <Arduino.h>
#include "../project/ProjectTypes.h"

/**
 * Interface for loading project configuration
 */
class IProjectLoader {
public:
  virtual ~IProjectLoader() = default;
  
  /**
   * Load project from file system
   * @param projectPath Path to project.json (default: /PROJECT/project.json)
   * @return true if loaded successfully
   */
  virtual bool loadProject(const String& projectPath = "/PROJECT/project.json") = 0;
  
  /**
   * Get the loaded project configuration
   */
  virtual const ProjectConfig& getProject() const = 0;
  
  /**
   * Check if a project is loaded
   */
  virtual bool isLoaded() const = 0;
  
  /**
   * Get current value for a topic (by topic name)
   */
  virtual String getTopicValue(const String& topic) const = 0;
  
  /**
   * Set current value for a topic (for testing without MQTT)
   */
  virtual void setTopicValue(const String& topic, const String& value) = 0;

  /**
   * Resolve the effective action for a button (e.g. "button-0") on a given
   * screen: that screen's own override if it has one, else the project-
   * wide default for that button id, else a default-constructed
   * ButtonAction (empty `type` - caller treats this as "nothing configured
   * here, do whatever the generic fallback behavior is").
   * @param screenIndex Index of the currently-displayed screen (-1 or
   *   out-of-range is treated as "no screen override available")
   * @param buttonId Button id string, e.g. "button-0"
   */
  virtual ButtonAction getButtonAction(int screenIndex, const String& buttonId) const = 0;
};





