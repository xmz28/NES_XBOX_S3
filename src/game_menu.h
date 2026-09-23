#pragma once

#include <stdint.h>

namespace game_menu {

bool begin();
void setButtons(uint16_t buttons);
void tick();
void processTouch();
void renderIfNeeded(bool force = false);
bool takeLaunchRequest(uint16_t &gameIndex);

}  // namespace game_menu
