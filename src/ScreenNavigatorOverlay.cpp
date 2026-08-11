#include "ScreenNavigatorOverlay.h"
#include "project/ColorAssetLoader.h"

namespace {
const unsigned long ENTER_MS = 250;
const unsigned long HOLD_MS = 500;
const unsigned long EXIT_MS = 200;

// 240x240 display, center at (120,120). Tablets sit near the top edge but
// fully on-canvas; the fly-in start radius (OUTER_R) is chosen so every
// tablet position within the arc this class actually uses (roughly the top
// third of the circle) lands with a negative y - off the top edge, safely
// clipped by ClippedCanvas16 - without needing any real alpha fade.
const int CENTER_X = 120;
const int CENTER_Y = 120;
// Bumped 32->40 (2026-08-11): 32px looked small and blocky on real
// hardware - a plain resolution increase (bigger source raster = bigger
// AND finer-looking even as a hard-edged 1-bit mask), not a switch away
// from the mask approach itself. Tablet radii/arc radius scaled up to
// match (see the spacing comment below for the math this keeps clearing).
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

// Per-tablet angular spacing, capped so a large screen count doesn't spread
// the whole way around the display - see this class's own header comment,
// crowding at high counts is an accepted first-pass tradeoff, not
// unnoticed.
const float MAX_TOTAL_SPAN_DEG = 130.0f;
// Worst case center-to-center need = TABLET_RADIUS + ACTIVE_TABLET_RADIUS +
// a real gap (an inactive tablet sitting right next to the larger active
// one) = 22+25+4 = 51px. At the new TARGET_ARC_R (88), 34deg gives ~52px of
// spacing - clears that with a small margin, same reasoning as the 28deg
// value this replaces (bumped 2026-08-11 alongside the icon size increase,
// since bigger tablets need proportionally more room to keep clearing it).
const float PREFERRED_STEP_DEG = 34.0f;
}  // namespace

ScreenNavigatorOverlay::ScreenNavigatorOverlay(ClippedCanvas16* canvas, IProjectLoader& projectLoader)
  : canvas_(canvas), projectLoader_(projectLoader) {}

void ScreenNavigatorOverlay::trigger(int newScreenIndex) {
  activeIndex_ = newScreenIndex;
  if (phase_ == Phase::Idle) {
    phase_ = Phase::Entering;
  } else {
    // Already visible, mid fly-in, or mid fly-out - just move the marker
    // and restart the hold timer. Replaying the fly-in on every single
    // detent while spinning the dial quickly would look like a stutter,
    // not an animation - see this class's own header comment.
    phase_ = Phase::Holding;
  }
  phaseStartMs_ = millis();
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

void ScreenNavigatorOverlay::drawTablets(float progress) const {
  if (!projectLoader_.isLoaded()) return;
  const ProjectConfig& project = projectLoader_.getProject();
  int count = (int)project.screens.size();
  if (count == 0) return;

  float totalSpanDeg = count > 1 ? min(MAX_TOTAL_SPAN_DEG, PREFERRED_STEP_DEG * (count - 1)) : 0.0f;
  float stepDeg = count > 1 ? totalSpanDeg / (float)(count - 1) : 0.0f;
  float startDeg = -totalSpanDeg / 2.0f;

  int r = OUTER_ARC_R + (int)((TARGET_ARC_R - OUTER_ARC_R) * progress);

  for (int i = 0; i < count; i++) {
    // 0 degrees = straight up - screen coordinates put 0/90/180/270 at
    // right/down/left/up respectively, hence the -90 shift.
    float angleDeg = startDeg + i * stepDeg;
    float angleRad = (angleDeg - 90.0f) * PI / 180.0f;
    int cx = CENTER_X + (int)(r * cosf(angleRad));
    int cy = CENTER_Y + (int)(r * sinf(angleRad));

    bool isActive = (i == activeIndex_);
    int tabletRadius = isActive ? ACTIVE_TABLET_RADIUS : TABLET_RADIUS;
    canvas_->fillCircle(cx, cy, tabletRadius, isActive ? COLOR_ACTIVE : COLOR_INACTIVE);
    if (isActive) {
      canvas_->drawCircle(cx, cy, tabletRadius, COLOR_WHITE);
    }

    const Screen& screen = project.screens[i];
    if (!screen.pageIconPath.isEmpty()) {
      uint16_t tabletColor = isActive ? COLOR_ACTIVE : COLOR_INACTIVE;
      ColorAssetLoader::drawGrayscaleMaskToCanvas(canvas_, screen.pageIconPath, cx - ICON_SIZE / 2, cy - ICON_SIZE / 2, COLOR_WHITE, tabletColor);
    }
  }
}
