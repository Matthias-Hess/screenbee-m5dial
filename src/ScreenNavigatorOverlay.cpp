#include "ScreenNavigatorOverlay.h"
#include "project/ColorAssetLoader.h"

namespace {
const unsigned long ENTER_MS = 250;
// 1.5s of no further encoder increment/decrement (main.cpp's loop() calls
// trigger()/triggerContinuous() on every raw movement now, not just a
// completed detent - see its own comment) - bumped from an initial 500ms
// 2026-08-11 per direct feedback that the tablets vanished too eagerly once
// "any movement resets the timer" made them appear much earlier in a turn
// too.
const unsigned long HOLD_MS = 1500;
const unsigned long EXIT_MS = 200;

// 240x240 display, center at (120,120).
const int CENTER_X = 120;
const int CENTER_Y = 120;
const int CANVAS_SIZE = 240;
// Bumped 32->40 (2026-08-11): 32px looked small and blocky on real
// hardware - a plain resolution increase (bigger source raster = bigger
// AND finer-looking even as a hard-edged mask), not a switch away from the
// mask approach itself.
const int TABLET_RADIUS = 22;
const int ACTIVE_TABLET_RADIUS = 25;
const int TARGET_ARC_R = 88;
const int OUTER_ARC_R = 170;
const int ICON_SIZE = 40;  // must match the DDF's needsPageIconsInSize

// RGB565: #ff6600 (the M5 Dial adornment's own orange accent, see
// adornment.svg) for the active tablet, #333333 for inactive ones, white
// for the active ring and the icon stencil color.
const uint16_t COLOR_ACTIVE = 0xFB20;
const uint16_t COLOR_INACTIVE = 0x3186;
const uint16_t COLOR_WHITE = 0xFFFF;

// Per-tablet angular spacing along the arc portion of the path - fixed
// regardless of screen count (2026-08-11, replacing an earlier
// count-dependent span that compressed spacing as screens were added).
// Worst case center-to-center need = TABLET_RADIUS + ACTIVE_TABLET_RADIUS +
// a real gap (an inactive tablet sitting right next to the larger active
// one) = 22+25+4 = 51px. At TARGET_ARC_R (88), 34deg gives ~52px of
// spacing - clears that with a small margin.
const float STEP_DEG = 34.0f;
// The point along the arc where its tangent goes vertical (the rightmost/
// leftmost point of the circle, directly out from center) - beyond this,
// the path continues straight down instead of curving back in. See this
// class's header comment for why (an upside-down "U" with parallel legs,
// not a closing circle).
const float ARC_TO_STRAIGHT_DEG = 90.0f;
// How many tablets on each side of center to even consider drawing. Chosen
// generously (STEP_DEG=34 puts the arc/straight transition at ~2.6 steps,
// and by step 5 a resting-radius tablet has already scrolled off the
// canvas's bottom edge - see drawTablets()'s own off-canvas skip) - not a
// visible "window" size, just an upper bound so a very large project
// doesn't walk hundreds of screens every frame.
const int MAX_STEPS = 6;
}  // namespace

ScreenNavigatorOverlay::ScreenNavigatorOverlay(ClippedCanvas16* canvas, IProjectLoader& projectLoader)
  : canvas_(canvas), projectLoader_(projectLoader) {}

void ScreenNavigatorOverlay::beginOrExtend() {
  if (phase_ == Phase::Idle) {
    phase_ = Phase::Entering;
  } else {
    // Already visible, mid fly-in, or mid fly-out - just restart the hold
    // timer. Replaying the fly-in on every single detent while spinning the
    // dial quickly would look like a stutter, not an animation - see this
    // class's own header comment.
    phase_ = Phase::Holding;
  }
  phaseStartMs_ = millis();
}

void ScreenNavigatorOverlay::trigger(int newScreenIndex) {
  scrollPosition_ = (float)newScreenIndex;
  beginOrExtend();
}

void ScreenNavigatorOverlay::triggerContinuous(float scrollPosition) {
  scrollPosition_ = scrollPosition;
  beginOrExtend();
}

bool ScreenNavigatorOverlay::update(ColorScreenRenderer* screenRenderer, int currentScreenIndex) {
  if (phase_ == Phase::Idle) return false;

  unsigned long elapsed = millis() - phaseStartMs_;
  float progress = 1.0f;

  if (phase_ == Phase::Entering) {
    progress = min(1.0f, (float)elapsed / (float)ENTER_MS);
    if (progress >= 1.0f) {
      phase_ = Phase::Holding;
      phaseStartMs_ = millis();
    }
  } else if (phase_ == Phase::Holding) {
    if (elapsed >= HOLD_MS) {
      phase_ = Phase::Exiting;
      phaseStartMs_ = millis();
      elapsed = 0;
    }
    progress = 1.0f;
  }
  if (phase_ == Phase::Exiting) {
    float exitProgress = min(1.0f, (float)elapsed / (float)EXIT_MS);
    progress = 1.0f - exitProgress;
    // Still draws this one last frame at progress==0 (tablets fully
    // retracted, so visually identical to no overlay at all) before
    // flipping to Idle - the following call returns false immediately,
    // no redundant redraw once nothing's changing anymore.
    if (exitProgress >= 1.0f) phase_ = Phase::Idle;
  }

  if (!screenRenderer) {
    phase_ = Phase::Idle;
    return false;
  }

  screenRenderer->renderScreen(currentScreenIndex);
  drawTablets(progress);
  return true;
}

void ScreenNavigatorOverlay::pathPosition(float offset, int r, int* outX, int* outY) const {
  float sign = offset < 0 ? -1.0f : 1.0f;
  float angleDeg = fabsf(offset) * STEP_DEG;

  int x, y;
  if (angleDeg <= ARC_TO_STRAIGHT_DEG) {
    // 0 degrees = straight up - screen coordinates put 0/90/180/270 at
    // right/down/left/up respectively, hence the -90 shift.
    float angleRad = (sign * angleDeg - 90.0f) * PI / 180.0f;
    x = CENTER_X + (int)(r * cosf(angleRad));
    y = CENTER_Y + (int)(r * sinf(angleRad));
  } else {
    // Past the tangent point: straight line down from there, at the same
    // per-degree spacing the arc used (arc length = r * angle-in-radians),
    // so a tablet's distance from its neighbors doesn't visibly change
    // where the path stops curving.
    float tangentRad = (sign * ARC_TO_STRAIGHT_DEG - 90.0f) * PI / 180.0f;
    int tx = CENTER_X + (int)(r * cosf(tangentRad));
    int ty = CENTER_Y + (int)(r * sinf(tangentRad));
    float extraDeg = angleDeg - ARC_TO_STRAIGHT_DEG;
    float extraDist = r * extraDeg * PI / 180.0f;
    x = tx;
    y = ty + (int)extraDist;
  }
  *outX = x;
  *outY = y;
}

void ScreenNavigatorOverlay::drawTablets(float progress) const {
  if (!projectLoader_.isLoaded()) return;
  const ProjectConfig& project = projectLoader_.getProject();
  int count = (int)project.screens.size();
  if (count == 0) return;

  int r = OUTER_ARC_R + (int)((TARGET_ARC_R - OUTER_ARC_R) * progress);

  // centerIdx: the screen index nearest the path's center-top point right
  // now; frac: how far scrollPosition_ actually is from that whole index
  // (-0.5..0.5). Every tablet's offset from center is k - frac, so at
  // frac==0 tablets sit exactly on their resting marks (matches the old
  // discrete behavior byte for byte), and as frac moves the whole strip
  // visibly slides - see this class's own header comment.
  int centerIdx = (int)roundf(scrollPosition_);
  float frac = scrollPosition_ - (float)centerIdx;

  // Small, fixed range around center rather than looping every screen -
  // see MAX_STEPS's own comment. No wrap-around (2026-08-11): a k that
  // lands outside the actual screen list is simply not drawn - the strip
  // ends at the first/last screen instead of cycling back, matching
  // main.cpp's clamped (not modulo) scrollPosition.
  for (int k = -MAX_STEPS; k <= MAX_STEPS; k++) {
    int idx = centerIdx + k;
    if (idx < 0 || idx >= count) continue;
    float offset = (float)k - frac;

    int cx, cy;
    pathPosition(offset, r, &cx, &cy);
    // Cheap off-canvas skip - avoids the (relatively expensive, LittleFS
    // file read) icon draw for tablets that have scrolled well past the
    // visible edge. Margin covers the tablet radius and icon size so a
    // partially-visible tablet right at the edge still gets drawn.
    if (cx < -ICON_SIZE || cx > CANVAS_SIZE + ICON_SIZE || cy < -ICON_SIZE || cy > CANVAS_SIZE + ICON_SIZE) {
      continue;
    }

    bool isActive = (k == 0);
    int tabletRadius = isActive ? ACTIVE_TABLET_RADIUS : TABLET_RADIUS;
    canvas_->fillCircle(cx, cy, tabletRadius, isActive ? COLOR_ACTIVE : COLOR_INACTIVE);
    if (isActive) {
      canvas_->drawCircle(cx, cy, tabletRadius, COLOR_WHITE);
    }

    const Screen& screen = project.screens[idx];
    if (!screen.pageIconPath.isEmpty()) {
      uint16_t tabletColor = isActive ? COLOR_ACTIVE : COLOR_INACTIVE;
      ColorAssetLoader::drawGrayscaleMaskToCanvas(canvas_, screen.pageIconPath, cx - ICON_SIZE / 2, cy - ICON_SIZE / 2, COLOR_WHITE, tabletColor);
    }
  }
}
