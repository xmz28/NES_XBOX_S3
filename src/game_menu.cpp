#include "game_menu.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "display_panel.h"
#include "game_ui.h"
#include "nes_runtime.h"
#include "rom_catalog.h"
#include "touch_input.h"

namespace game_menu {
namespace {

constexpr uint16_t kRowsPerPage = 10;
constexpr uint16_t kListTop = 82;
constexpr uint16_t kRowHeight = 36;
constexpr uint16_t kBackground = 0x0861;
constexpr uint16_t kPanel = 0x10C3;
constexpr uint16_t kSelected = 0xFD20;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kMuted = 0x9CF3;
constexpr uint16_t kBlack = 0x0000;
constexpr uint32_t kRepeatDelayMs = 400;
constexpr uint32_t kRepeatIntervalMs = 90;
constexpr int kSwipeThreshold = 60;
constexpr int kTapMovementLimit = 25;

uint16_t gSelected = 0;
uint16_t gPreviousButtons = 0;
uint16_t gLaunchIndex = UINT16_MAX;
bool gDirty = true;
int8_t gRepeatDirection = 0;
uint32_t gNextRepeatAtMs = 0;

struct TouchGesture {
  bool active = false;
  bool inList = false;
  uint16_t startY = 0;
  uint16_t lastY = 0;
  uint16_t maximumMovement = 0;
};

TouchGesture gTouchGesture;

struct Glyph {
  char character;
  uint8_t rows[7];
};

constexpr Glyph kGlyphs[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {31, 4, 4, 4, 4, 4, 31}},      {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'/', {1, 2, 2, 4, 8, 8, 16}},
    {'>', {16, 8, 4, 2, 4, 8, 16}},      {'.', {0, 0, 0, 0, 0, 12, 12}},
    {'#', {10, 31, 10, 10, 31, 10, 0}},
};

void fillRect(uint16_t *buffer, uint16_t width, uint16_t height, int x, int y,
              int rectWidth, int rectHeight, uint16_t color) {
  if (buffer == nullptr || x < 0 || y < 0 || x + rectWidth > width ||
      y + rectHeight > height) return;
  for (int row = 0; row < rectHeight; ++row) {
    uint16_t *destination = buffer + (y + row) * width + x;
    for (int column = 0; column < rectWidth; ++column) destination[column] = color;
  }
}

const uint8_t *glyphRows(char character) {
  for (const Glyph &glyph : kGlyphs) {
    if (glyph.character == character) return glyph.rows;
  }
  return nullptr;
}

void drawText(uint16_t *buffer, uint16_t width, uint16_t height, int x, int y,
              const char *text, uint8_t scale, uint16_t color) {
  if (text == nullptr) return;
  for (; *text != '\0'; ++text, x += 6 * scale) {
    if (*text == ' ') continue;
    const uint8_t *rows = glyphRows(*text);
    if (rows == nullptr) continue;
    for (int row = 0; row < 7; ++row) {
      for (int column = 0; column < 5; ++column) {
        if ((rows[row] & (1U << (4 - column))) != 0U) {
          fillRect(buffer, width, height, x + column * scale, y + row * scale,
                   scale, scale, color);
        }
      }
    }
  }
}

void renderMenu(uint16_t *buffer, uint16_t width, uint16_t height, void *) {
  fillRect(buffer, width, height, 0, 0, width, height, kBackground);
  drawText(buffer, width, height, 171, 18, "NES GAME LIBRARY", 3, kWhite);
  drawText(buffer, width, height, 123, 54,
           "DPAD SELECT  A START  VIEW+MENU RETURN", 1, kMuted);

  const uint16_t pageStart = (gSelected / kRowsPerPage) * kRowsPerPage;
  for (uint16_t row = 0; row < kRowsPerPage; ++row) {
    const uint16_t index = pageStart + row;
    if (index >= rom_catalog::count()) break;
    const rom_catalog::Game *item = rom_catalog::game(index);
    const int y = kListTop + row * kRowHeight;
    const bool selected = index == gSelected;
    fillRect(buffer, width, height, 150, y, 500, kRowHeight - 3,
             selected ? kSelected : kPanel);
    char label[64] = {};
    snprintf(label, sizeof(label), "%02u  %s", index + 1, item->name);
    drawText(buffer, width, height, 164, y + 9, label, 2,
             selected ? kBlack : kWhite);
    char details[24] = {};
    snprintf(details, sizeof(details), "%luK M%u",
             static_cast<unsigned long>((item->size + 1023U) / 1024U),
             item->mapper);
    drawText(buffer, width, height, 574, y + 12, details, 1,
             selected ? kBlack : kMuted);
  }

  char page[32] = {};
  snprintf(page, sizeof(page), "PAGE %u/%u", gSelected / kRowsPerPage + 1,
           (rom_catalog::count() + kRowsPerPage - 1) / kRowsPerPage);
  drawText(buffer, width, height, 574, 458, page, 1, kMuted);
  game_ui::render(buffer, width, height);
}

void moveSelection(int delta) {
  const int total = rom_catalog::count();
  if (total == 0) return;
  int next = static_cast<int>(gSelected) + delta;
  while (next < 0) next += total;
  while (next >= total) next -= total;
  if (next != gSelected) {
    gSelected = static_cast<uint16_t>(next);
    gDirty = true;
  }
}

}  // namespace

bool begin() {
  gSelected = 0;
  gPreviousButtons = 0;
  gLaunchIndex = UINT16_MAX;
  gDirty = true;
  gRepeatDirection = 0;
  gNextRepeatAtMs = 0;
  gTouchGesture = {};
  return rom_catalog::begin();
}

void setButtons(uint16_t buttons) {
  const uint16_t pressed = buttons & ~gPreviousButtons;
  gPreviousButtons = buttons;
  if ((pressed & nes_runtime::kButtonUp) != 0U) moveSelection(-1);
  if ((pressed & nes_runtime::kButtonDown) != 0U) moveSelection(1);
  if ((pressed & nes_runtime::kButtonLeft) != 0U) moveSelection(-kRowsPerPage);
  if ((pressed & nes_runtime::kButtonRight) != 0U) moveSelection(kRowsPerPage);
  if ((pressed & (nes_runtime::kButtonA | nes_runtime::kButtonStart)) != 0U) {
    gLaunchIndex = gSelected;
  }

  const bool up = (buttons & nes_runtime::kButtonUp) != 0U;
  const bool down = (buttons & nes_runtime::kButtonDown) != 0U;
  const int8_t direction = up == down ? 0 : (up ? -1 : 1);
  if (direction != gRepeatDirection) {
    gRepeatDirection = direction;
    gNextRepeatAtMs = direction == 0 ? 0 : millis() + kRepeatDelayMs;
  }
}

void tick() {
  if (gRepeatDirection == 0 || gNextRepeatAtMs == 0) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - gNextRepeatAtMs) < 0) return;
  moveSelection(gRepeatDirection);
  gNextRepeatAtMs = now + kRepeatIntervalMs;
}

void processTouch() {
  touch_input::State touch = {};
  while (touch_input::takeLatest(touch)) {
    if (!touch.valid) continue;
    if (touch.pressed) {
      if (!gTouchGesture.active) {
        gTouchGesture.active = true;
        gTouchGesture.inList = touch.x >= 150 && touch.x < 650;
        gTouchGesture.startY = touch.y;
        gTouchGesture.lastY = touch.y;
        gTouchGesture.maximumMovement = 0;
      }
      if (gTouchGesture.inList) {
        gTouchGesture.lastY = touch.y;
        const int movement = abs(static_cast<int>(touch.y) -
                                 static_cast<int>(gTouchGesture.startY));
        gTouchGesture.maximumMovement = max<uint16_t>(
            gTouchGesture.maximumMovement, static_cast<uint16_t>(movement));
      } else if (game_ui::handleTouch(touch)) {
        gDirty = true;
      }
      continue;
    }

    if (!gTouchGesture.active) continue;
    if (!gTouchGesture.inList) {
      game_ui::handleTouch(touch);
    } else {
      const int deltaY = static_cast<int>(gTouchGesture.lastY) -
                         static_cast<int>(gTouchGesture.startY);
      if (deltaY <= -kSwipeThreshold) {
        moveSelection(kRowsPerPage);
        Serial.println("MENU: swipe up; next page");
      } else if (deltaY >= kSwipeThreshold) {
        moveSelection(-kRowsPerPage);
        Serial.println("MENU: swipe down; previous page");
      } else if (gTouchGesture.maximumMovement <= kTapMovementLimit &&
                 gTouchGesture.startY >= kListTop &&
                 gTouchGesture.startY <
                     kListTop + kRowsPerPage * kRowHeight) {
        const uint16_t row = (gTouchGesture.startY - kListTop) / kRowHeight;
        const uint16_t index =
            (gSelected / kRowsPerPage) * kRowsPerPage + row;
        if (index < rom_catalog::count()) {
          gSelected = index;
          gLaunchIndex = index;
          gDirty = true;
        }
      }
    }
    gTouchGesture = {};
  }
}

void renderIfNeeded(bool force) {
  if (!force && !gDirty) return;
  if (display_panel::renderAndPresentFrame(renderMenu, nullptr)) gDirty = false;
}

bool takeLaunchRequest(uint16_t &gameIndex) {
  if (gLaunchIndex == UINT16_MAX) return false;
  gameIndex = gLaunchIndex;
  gLaunchIndex = UINT16_MAX;
  return true;
}

}  // namespace game_menu
