#pragma once
#include "interfaces/IDisplay.h"

// Simple IDisplay implementation for setup-mode status text (AP
// instructions, connection status) via M5Dial.Display's own built-in text
// drawing - deliberately NOT going through ColorScreenRenderer/u8g2's
// pixel-perfect BDF pipeline, which exists to match the designer's own
// rendering of a real authored project exactly. This is throwaway setup
// UI with no designer-side equivalent to match, so M5GFX's own font
// rendering is simpler and sufficient.
class M5DisplayAdapter : public IDisplay {
public:
  void showLines(std::initializer_list<String> lines) override;
  void clear() override;
};
