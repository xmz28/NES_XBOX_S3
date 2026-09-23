#include "game_ui.h"

#include <Arduino.h>
#include <Preferences.h>

#include "display_panel.h"
#include "touch_input.h"

namespace game_ui {
namespace {

constexpr uint16_t kGameLeft = 144;
constexpr uint16_t kGameRight = 656;
constexpr uint16_t kTrackTop = 60;
constexpr uint16_t kTrackBottom = 420;
constexpr uint16_t kTrackHeight = kTrackBottom - kTrackTop;
constexpr uint8_t kDefaultBrightness = 100;
constexpr uint8_t kMinimumBrightness = 5;
constexpr uint8_t kDefaultVolume = 35;

Preferences gPreferences;
volatile uint8_t gBrightness = kDefaultBrightness;
volatile uint8_t gVolume = kDefaultVolume;
bool gTouchChangedSetting = false;

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8U) << 8U) |
                               ((green & 0xFCU) << 3U) | (blue >> 3U));
}

void fillRect(uint16_t *frameBuffer, uint16_t width, uint16_t height,
              uint16_t x, uint16_t y, uint16_t rectWidth,
              uint16_t rectHeight, uint16_t color) {
  if (frameBuffer == nullptr || x >= width || y >= height) return;
  const uint16_t clippedWidth = min<uint16_t>(rectWidth, width - x);
  const uint16_t clippedHeight = min<uint16_t>(rectHeight, height - y);
  for (uint16_t row = 0; row < clippedHeight; ++row) {
    uint16_t *destination = frameBuffer + (y + row) * width + x;
    for (uint16_t column = 0; column < clippedWidth; ++column) {
      destination[column] = color;
    }
  }
}

void drawMeter(uint16_t *frameBuffer, uint16_t width, uint16_t height,
               uint16_t centerX, uint8_t percent, uint16_t color) {
  constexpr uint16_t outerWidth = 42;
  constexpr uint16_t innerWidth = 28;
  constexpr uint16_t border = 3;
  const uint16_t outerX = centerX - outerWidth / 2U;
  const uint16_t innerX = centerX - innerWidth / 2U;
  fillRect(frameBuffer, width, height, outerX, kTrackTop - border,
           outerWidth, kTrackHeight + border * 2U, rgb565(10, 14, 20));
  fillRect(frameBuffer, width, height, outerX, kTrackTop - border,
           outerWidth, border, rgb565(100, 110, 120));
  fillRect(frameBuffer, width, height, outerX, kTrackBottom,
           outerWidth, border, rgb565(100, 110, 120));
  fillRect(frameBuffer, width, height, outerX, kTrackTop,
           border, kTrackHeight, rgb565(100, 110, 120));
  fillRect(frameBuffer, width, height, outerX + outerWidth - border,
           kTrackTop, border, kTrackHeight, rgb565(100, 110, 120));

  const uint16_t filled = static_cast<uint16_t>(
      (static_cast<uint32_t>(kTrackHeight - 8U) * percent) / 100U);
  fillRect(frameBuffer, width, height, innerX, kTrackTop + 4U,
           innerWidth, kTrackHeight - 8U, rgb565(20, 24, 30));
  if (filled > 0U) {
    fillRect(frameBuffer, width, height, innerX,
             kTrackBottom - 4U - filled, innerWidth, filled, color);
  }
}

void drawSun(uint16_t *frameBuffer, uint16_t width, uint16_t height) {
  constexpr uint16_t color = rgb565(255, 205, 45);
  fillRect(frameBuffer, width, height, 62, 22, 20, 20, color);
  fillRect(frameBuffer, width, height, 69, 8, 6, 10, color);
  fillRect(frameBuffer, width, height, 69, 46, 6, 10, color);
  fillRect(frameBuffer, width, height, 48, 29, 10, 6, color);
  fillRect(frameBuffer, width, height, 86, 29, 10, 6, color);
}

void drawSpeaker(uint16_t *frameBuffer, uint16_t width, uint16_t height) {
  constexpr uint16_t color = rgb565(80, 205, 255);
  fillRect(frameBuffer, width, height, 704, 24, 12, 18, color);
  for (uint16_t row = 0; row < 30; ++row) {
    const uint16_t run = static_cast<uint16_t>(row < 15 ? row / 2 :
                                               (29U - row) / 2U);
    fillRect(frameBuffer, width, height, 716, 18 + row, run + 2U, 1, color);
  }
  fillRect(frameBuffer, width, height, 732, 22, 4, 22, color);
  fillRect(frameBuffer, width, height, 740, 17, 4, 32, color);
}

uint8_t percentFromY(uint16_t y) {
  const uint16_t constrainedY = constrain(y, kTrackTop, kTrackBottom);
  return static_cast<uint8_t>(
      ((kTrackBottom - constrainedY) * 100U) / kTrackHeight);
}

}  // namespace

void begin() {
  if (gPreferences.begin("nes-ui", false)) {
    gBrightness = constrain(gPreferences.getUChar("bright", kDefaultBrightness),
                            kMinimumBrightness, 100);
    gVolume = constrain(gPreferences.getUChar("volume", kDefaultVolume), 0, 100);
  }
  Serial.printf("UI: backlight=%u%% volume=%u%%\n",
                gBrightness, gVolume);
}

bool handleTouch(const touch_input::State &touch) {
  bool changed = false;
  if (touch.valid && touch.pressed) {
    const uint8_t percent = percentFromY(touch.y);
    if (touch.x < kGameLeft) {
      const uint8_t brightness = max<uint8_t>(percent, kMinimumBrightness);
      changed = brightness != gBrightness;
      gBrightness = brightness;
      display_panel::setBacklightPercent(gBrightness);
      gTouchChangedSetting = true;
    } else if (touch.x >= kGameRight) {
      changed = percent != gVolume;
      gVolume = percent;
      gTouchChangedSetting = true;
    }
  } else if (touch.valid && !touch.pressed && gTouchChangedSetting) {
    gPreferences.putUChar("bright", gBrightness);
    gPreferences.putUChar("volume", gVolume);
    Serial.printf("UI: saved brightness=%u%% volume=%u%%\n",
                  gBrightness, gVolume);
    gTouchChangedSetting = false;
  }
  return changed;
}

void processTouch() {
  touch_input::State touch = {};
  while (touch_input::takeLatest(touch)) {
    handleTouch(touch);
  }
}

void render(uint16_t *frameBuffer, uint16_t width, uint16_t height) {
  drawMeter(frameBuffer, width, height, 72, gBrightness,
            rgb565(255, 180, 30));
  drawMeter(frameBuffer, width, height, 728, gVolume,
            rgb565(30, 175, 255));
  drawSun(frameBuffer, width, height);
  drawSpeaker(frameBuffer, width, height);
}

uint8_t brightnessPercent() { return gBrightness; }
uint8_t volumePercent() { return gVolume; }

}  // namespace game_ui
