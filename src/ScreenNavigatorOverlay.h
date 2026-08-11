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
// an 8-bit grayscale PGM mask - empty if that screen never got one) is drawn
// on a round "tablet". Tablets fly in from off-canvas on the first
// next/previous input, stay put while further input keeps arriving, then
// fly back out after a short idle gap.
//
// Layout (2026-08-11, replacing an earlier fixed-arc-only version that ran
// out of room past ~4-5 screens): tablets sit on a "stadium" path, not a
// plain circular arc - a bogen across the top out to +-90 degrees (the
// point where the circle's tangent goes vertical), then straight down in a
// parallel line on each side. There's no cap on how many tablets exist:
// ones that scroll past the canvas edge are simply not drawn (see
// drawTablets()'s own comment) rather than being crammed into a fixed span.
//
// Position is a single continuous float (scrollPosition_), not an integer
// index - trigger() (discrete jumps: touch, front button) sets it to an
// exact integer so tablets land precisely on their resting spot with no
// in-between frame, while triggerContinuous() (the rotary encoder, only
// when btn-0/btn-1 are configured as previous-screen/next-screen - see
// main.cpp's continuousNavActive) feeds it a real-valued position so the
// whole tablet strip visibly slides as the dial turns, matching the raw
// encoder position rather than snapping once per detent. Both paths share
// the same phase machine (fly-in/hold/fly-out) - see beginOrExtend().
//
// Every visual/timing constant here (arc radius, tablet size, colors,
// durations) is a first-pass default, not a spec - explicitly meant to be
// refined once actually seen running on the device (see the M5 Dial
// grilling session in device-contract.md's history for why this wasn't
// nailed down in the abstract first).
class ScreenNavigatorOverlay {
public:
  ScreenNavigatorOverlay(ClippedCanvas16* canvas, IProjectLoader& projectLoader);

  // Discrete jump - visual position snaps exactly to newScreenIndex, no
  // fractional in-between frame. Call right after the real screen switch
  // (currentScreenIndex already updated, destination screen already
  // rendered+blitted once by the caller, same as any other screen switch).
  // Starts the fly-in animation, or if the overlay is already
  // visible/animating, just jumps the active marker and restarts the hold
  // timer without replaying the fly-in (see beginOrExtend()).
  void trigger(int newScreenIndex);

  // Continuous coupling - scrollPosition is a real-valued screen index
  // (e.g. 2.35 = 35% of the way from screen 2 to screen 3); the whole
  // tablet strip is drawn at that exact fractional offset every call, so it
  // visibly tracks the rotary encoder's raw position instead of jumping
  // once per detent. Same phase machine as trigger() - see
  // beginOrExtend(). Caller (main.cpp) is responsible for deciding which
  // integer screen is "current" (nearest whole scrollPosition) and updating
  // currentScreenIndex/dispatching accordingly; this class only draws.
  void triggerContinuous(float scrollPosition);

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
  float scrollPosition_ = 0.0f;

  // Shared by trigger()/triggerContinuous(): Idle->Entering (start the
  // fly-in), otherwise->Holding (already visible/animating - just restart
  // the hold-timer countdown, no replayed fly-in - see this class's own
  // header comment for why that would look like a stutter).
  void beginOrExtend();

  // Given a signed, possibly-fractional step offset from the path's
  // center-top point (offset = k - frac, see drawTablets()) and the arc
  // radius r, returns that tablet's (x, y). See this class's own header
  // comment for the arc-then-straight-down "stadium" shape.
  void pathPosition(float offset, int r, int* outX, int* outY) const;

  // progress: 0 = tablets fully retracted (off-canvas, invisible), 1 = in
  // their resting position. Draws every screen's tablet regardless of
  // pageIconPath - an icon-less screen still gets a plain tablet (position
  // in the strip matters for orientation even without an icon to show).
  void drawTablets(float progress) const;
};
