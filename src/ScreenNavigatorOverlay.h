#pragma once
#include <Arduino.h>
#include "ClippedCanvas16.h"
#include "interfaces/IProjectLoader.h"
#include "project/ColorScreenRenderer.h"

// Animated screen-switch navigator (2026-08-11, M5 Dial-specific - the
// designer has zero opinion on this, see docs/device-contract.md's
// needsPageIconsInSize section): triggered by dispatchButtonAction()'s
// next-screen/previous-screen branches only (not goto-screen, not a plain
// SoftwareButton tap to a specific screen - those are direct jumps, no
// orientation aid needed). Every screen's own icon (Screen::pageIconPath,
// an 8-bit grayscale PGM mask - empty if that screen never got one) is drawn on a
// round "tablet" arranged along a fixed arc at the top of the round
// display; the current screen's tablet is highlighted. Tablets fly in from
// off-canvas on the first next/previous input, stay put while further
// next/previous input keeps arriving (each one just jumps which tablet is
// highlighted, restarting the hold timer - no replayed fly-in), then fly
// back out after a short idle gap.
//
// Every visual/timing constant here (arc radius, tablet size, colors,
// durations) is a first-pass default, not a spec - explicitly meant to be
// refined once actually seen running on the device (see the M5 Dial
// grilling session in device-contract.md's history for why this wasn't
// nailed down in the abstract first).
class ScreenNavigatorOverlay {
public:
  ScreenNavigatorOverlay(ClippedCanvas16* canvas, IProjectLoader& projectLoader);

  // Call right after the real screen switch (currentScreenIndex already
  // updated, destination screen already rendered+blitted once by the
  // caller, same as any other screen switch) - starts the fly-in animation,
  // or if the overlay is already visible/animating, just jumps the active
  // marker to newScreenIndex and restarts the hold timer without replaying
  // the fly-in (see this class's own header comment for why).
  void trigger(int newScreenIndex);

  // Call every loop() iteration regardless of whether the overlay is
  // active - a no-op returning false when idle. While animating/holding,
  // redraws currentScreenIndex's real screen content (screenRenderer must
  // already have a project loaded) plus the tablet strip on top, and
  // returns true to tell the caller to blit. currentScreenIndex is passed
  // in rather than read from a stored field so this always draws whatever
  // screen is actually current, even if something else changed it
  // mid-animation.
  bool update(ColorScreenRenderer* screenRenderer, int currentScreenIndex);

  bool isActive() const { return phase_ != Phase::Idle; }

private:
  enum class Phase { Idle, Entering, Holding, Exiting };

  ClippedCanvas16* canvas_;
  IProjectLoader& projectLoader_;
  Phase phase_ = Phase::Idle;
  unsigned long phaseStartMs_ = 0;
  int activeIndex_ = 0;

  // progress: 0 = tablets fully retracted (off-canvas, invisible), 1 = in
  // their resting arc position. Draws every screen's tablet regardless of
  // pageIconPath - an icon-less screen still gets a plain tablet (position
  // in the strip matters for orientation even without an icon to show).
  void drawTablets(float progress) const;
};
