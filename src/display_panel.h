#pragma once

#include <stddef.h>
#include <stdint.h>

namespace display_panel {

constexpr uint16_t kWidth = 800;
constexpr uint16_t kHeight = 480;

using FrameRenderer = void (*)(uint16_t *frameBuffer, uint16_t width,
                               uint16_t height, void *userContext);

bool begin();
bool beginBacklight(uint8_t initialPercent = 100);
void setBacklightPercent(uint8_t percent);
bool renderAndPresentFrame(FrameRenderer renderer, void *userContext,
                           uint32_t *renderTimeUs = nullptr,
                           uint32_t *presentTimeUs = nullptr);
uint32_t presentedFrameCount();

}  // namespace display_panel
