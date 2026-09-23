#pragma once

#include <stdint.h>

namespace touch_input {
struct State;
}

namespace game_ui {

void begin();
void processTouch();
bool handleTouch(const touch_input::State &touch);
void render(uint16_t *frameBuffer, uint16_t width, uint16_t height);
uint8_t brightnessPercent();
uint8_t volumePercent();

}  // namespace game_ui
