#pragma once

#include <stdint.h>

namespace nes_runtime {

constexpr uint16_t kButtonA = 1u << 0;
constexpr uint16_t kButtonB = 1u << 1;
constexpr uint16_t kButtonSelect = 1u << 2;
constexpr uint16_t kButtonStart = 1u << 3;
constexpr uint16_t kButtonUp = 1u << 4;
constexpr uint16_t kButtonDown = 1u << 5;
constexpr uint16_t kButtonLeft = 1u << 6;
constexpr uint16_t kButtonRight = 1u << 7;

bool begin(uint16_t gameIndex);
bool requestReturnToMenu();
bool takeReturnCompleted();
void setButtons(uint16_t buttons);
uint32_t frameCount();

}  // namespace nes_runtime
