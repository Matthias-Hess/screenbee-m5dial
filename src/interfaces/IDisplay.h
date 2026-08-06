#pragma once
#include <Arduino.h>
#include <initializer_list>

/**
 * Interface for display operations
 * Allows display implementations to be swapped without affecting business logic
 */
class IDisplay {
public:
  virtual ~IDisplay() = default;
  
  /**
   * Display multiple lines of text
   */
  virtual void showLines(std::initializer_list<String> lines) = 0;
  
  /**
   * Clear the display
   */
  virtual void clear() = 0;
};


