#include "display_panel.h"

#include <Arduino.h>
#include <string.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_memory_utils.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace display_panel {
namespace {

constexpr uint32_t kPixelClockHz = 16UL * 1000UL * 1000UL;
constexpr int kBacklightPin = 42;
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr uint8_t kBacklightResolutionBits = 11;
constexpr ledc_timer_bit_t kBacklightResolution = LEDC_TIMER_11_BIT;
constexpr uint32_t kBacklightFrequencyHz = 20000;
constexpr uint32_t kBacklightMaxDuty =
    (1U << kBacklightResolutionBits) - 1U;
constexpr uint16_t kHsyncPulseWidth = 48;
constexpr uint16_t kHsyncBackPorch = 40;
constexpr uint16_t kHsyncFrontPorch = 8;
constexpr uint16_t kVsyncPulseWidth = 2;
constexpr uint16_t kVsyncBackPorch = 40;
constexpr uint16_t kVsyncFrontPorch = 6;
constexpr bool kPixelClockActiveNegative = true;

constexpr int kPinPclk = 1;
constexpr int kPinHsync = 39;
constexpr int kPinVsync = 40;
constexpr int kPinDe = 41;
constexpr int kPinDispEnable = -1;
constexpr int kDataGpios[16] = {4, 5, 6, 7, 8, 9, 10, 11,
                                12, 13, 14, 15, 16, 17, 18, 21};

constexpr uint8_t kFrameBufferCount = 3;
constexpr uint16_t kBounceBufferLines = 30;
constexpr uint16_t kDmaBurstSize = 64;
constexpr uint32_t kSyncTimeoutMs = 100;
constexpr size_t kFrameBufferBytes =
    static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t);

struct DisplaySyncState {
  SemaphoreHandle_t vsyncSemaphore = nullptr;
  SemaphoreHandle_t frameCompleteSemaphore = nullptr;
  volatile uint32_t vsyncCount = 0;
  volatile uint32_t frameCompleteCount = 0;
};

esp_lcd_panel_handle_t gPanel = nullptr;
uint16_t *gFrameBuffers[kFrameBufferCount] = {};
DisplaySyncState gSync;
uint8_t gFrontBufferIndex = 0;
volatile uint32_t gPresentedFrames = 0;
SemaphoreHandle_t gDisplayMutex = nullptr;
bool gFrameSwitchPending = false;

bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t panel,
                       const esp_lcd_rgb_panel_event_data_t *eventData,
                       void *userContext) {
  (void)panel;
  (void)eventData;
  auto *sync = static_cast<DisplaySyncState *>(userContext);
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  sync->vsyncCount = sync->vsyncCount + 1U;
  xSemaphoreGiveFromISR(sync->vsyncSemaphore, &higherPriorityTaskWoken);
  return higherPriorityTaskWoken == pdTRUE;
}

bool IRAM_ATTR onFrameComplete(
    esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_data_t *eventData, void *userContext) {
  (void)panel;
  (void)eventData;
  auto *sync = static_cast<DisplaySyncState *>(userContext);
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  sync->frameCompleteCount = sync->frameCompleteCount + 1U;
  xSemaphoreGiveFromISR(sync->frameCompleteSemaphore, &higherPriorityTaskWoken);
  return higherPriorityTaskWoken == pdTRUE;
}

void drainSemaphore(SemaphoreHandle_t semaphore) {
  while (xSemaphoreTake(semaphore, 0) == pdTRUE) {
  }
}

bool waitForSignal(SemaphoreHandle_t semaphore, const char *name) {
  if (xSemaphoreTake(semaphore, pdMS_TO_TICKS(kSyncTimeoutMs)) == pdTRUE) {
    return true;
  }
  Serial.printf("LCD: waiting for %s timed out\n", name);
  return false;
}

bool presentFrameBuffer(uint8_t bufferIndex, uint32_t *presentTimeUs) {
  const uint32_t presentStartedAt = micros();
  // A third frame buffer lets the producer prepare the next image while the
  // RGB engine finishes switching the previously submitted one.
  if (gFrameSwitchPending &&
      !waitForSignal(gSync.frameCompleteSemaphore,
                     "previous frame complete")) {
    return false;
  }
  gFrameSwitchPending = false;
  drainSemaphore(gSync.frameCompleteSemaphore);
  const esp_err_t error = esp_lcd_panel_draw_bitmap(
      gPanel, 0, 0, kWidth, kHeight, gFrameBuffers[bufferIndex]);
  if (error != ESP_OK) {
    Serial.printf("LCD: frame switch failed: %s\n", esp_err_to_name(error));
    return false;
  }
  gFrameSwitchPending = true;
  if (presentTimeUs != nullptr) {
    *presentTimeUs = micros() - presentStartedAt;
  }
  gFrontBufferIndex = bufferIndex;
  gPresentedFrames = gPresentedFrames + 1U;
  return true;
}

}  // namespace

bool begin() {
  if (!psramFound()) {
    Serial.println("LCD: PSRAM not found");
    return false;
  }

  gSync.vsyncSemaphore = xSemaphoreCreateBinary();
  gSync.frameCompleteSemaphore = xSemaphoreCreateBinary();
  gDisplayMutex = xSemaphoreCreateRecursiveMutex();
  if (gSync.vsyncSemaphore == nullptr ||
      gSync.frameCompleteSemaphore == nullptr || gDisplayMutex == nullptr) {
    Serial.println("LCD: failed to create synchronization semaphores");
    return false;
  }

  esp_lcd_rgb_panel_config_t config = {};
  config.clk_src = LCD_CLK_SRC_DEFAULT;
  config.timings.pclk_hz = kPixelClockHz;
  config.timings.h_res = kWidth;
  config.timings.v_res = kHeight;
  config.timings.hsync_pulse_width = kHsyncPulseWidth;
  config.timings.hsync_back_porch = kHsyncBackPorch;
  config.timings.hsync_front_porch = kHsyncFrontPorch;
  config.timings.vsync_pulse_width = kVsyncPulseWidth;
  config.timings.vsync_back_porch = kVsyncBackPorch;
  config.timings.vsync_front_porch = kVsyncFrontPorch;
  config.timings.flags.hsync_idle_low = false;
  config.timings.flags.vsync_idle_low = false;
  config.timings.flags.de_idle_high = false;
  config.timings.flags.pclk_active_neg = kPixelClockActiveNegative;
  config.timings.flags.pclk_idle_high = false;
  config.data_width = 16;
  config.bits_per_pixel = 16;
  config.num_fbs = kFrameBufferCount;
  config.bounce_buffer_size_px = kWidth * kBounceBufferLines;
  config.dma_burst_size = kDmaBurstSize;
  config.hsync_gpio_num = kPinHsync;
  config.vsync_gpio_num = kPinVsync;
  config.de_gpio_num = kPinDe;
  config.pclk_gpio_num = kPinPclk;
  config.disp_gpio_num = kPinDispEnable;
  for (size_t index = 0; index < 16; ++index) {
    config.data_gpio_nums[index] = kDataGpios[index];
  }
  config.flags.fb_in_psram = true;
  config.flags.double_fb = false;

  esp_err_t error = esp_lcd_new_rgb_panel(&config, &gPanel);
  if (error != ESP_OK) {
    Serial.printf("LCD: esp_lcd_new_rgb_panel failed: %s\n",
                  esp_err_to_name(error));
    return false;
  }

  void *frameBuffer0 = nullptr;
  void *frameBuffer1 = nullptr;
  void *frameBuffer2 = nullptr;
  error = esp_lcd_rgb_panel_get_frame_buffer(
      gPanel, kFrameBufferCount, &frameBuffer0, &frameBuffer1, &frameBuffer2);
  if (error != ESP_OK || frameBuffer0 == nullptr || frameBuffer1 == nullptr ||
      frameBuffer2 == nullptr || frameBuffer0 == frameBuffer1 ||
      frameBuffer0 == frameBuffer2 || frameBuffer1 == frameBuffer2 ||
      !esp_ptr_external_ram(frameBuffer0) ||
      !esp_ptr_external_ram(frameBuffer1) ||
      !esp_ptr_external_ram(frameBuffer2) ||
      (reinterpret_cast<uintptr_t>(frameBuffer0) & 0x0FU) != 0U ||
      (reinterpret_cast<uintptr_t>(frameBuffer1) & 0x0FU) != 0U ||
      (reinterpret_cast<uintptr_t>(frameBuffer2) & 0x0FU) != 0U) {
    Serial.printf("LCD: invalid PSRAM frame buffers: %s\n",
                  esp_err_to_name(error));
    return false;
  }
  gFrameBuffers[0] = static_cast<uint16_t *>(frameBuffer0);
  gFrameBuffers[1] = static_cast<uint16_t *>(frameBuffer1);
  gFrameBuffers[2] = static_cast<uint16_t *>(frameBuffer2);
  memset(gFrameBuffers[0], 0, kFrameBufferBytes);
  memset(gFrameBuffers[1], 0, kFrameBufferBytes);
  memset(gFrameBuffers[2], 0, kFrameBufferBytes);

  esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
  callbacks.on_vsync = onVsync;
  callbacks.on_frame_buf_complete = onFrameComplete;
  error = esp_lcd_rgb_panel_register_event_callbacks(gPanel, &callbacks, &gSync);
  if (error != ESP_OK) {
    Serial.printf("LCD: callback registration failed: %s\n",
                  esp_err_to_name(error));
    return false;
  }

  error = esp_lcd_panel_reset(gPanel);
  if (error == ESP_OK) {
    error = esp_lcd_panel_init(gPanel);
  }
  if (error != ESP_OK) {
    Serial.printf("LCD: panel initialization failed: %s\n",
                  esp_err_to_name(error));
    return false;
  }

  Serial.printf(
      "LCD: 800x480 RGB565, PCLK %lu MHz falling edge, triple FB, bounce %u "
      "lines\nLCD: frame buffers %p / %p / %p (%u bytes each)\n",
      static_cast<unsigned long>(kPixelClockHz / 1000000UL),
      static_cast<unsigned>(kBounceBufferLines), frameBuffer0, frameBuffer1,
      frameBuffer2, static_cast<unsigned>(kFrameBufferBytes));
  return true;
}

void setBacklightPercent(uint8_t percent) {
  const uint8_t clamped = min<uint8_t>(percent, 100U);
  const uint32_t duty =
      (kBacklightMaxDuty * static_cast<uint32_t>(clamped) + 50U) / 100U;
  ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, duty);
  ledc_update_duty(kBacklightSpeedMode, kBacklightChannel);
}

bool beginBacklight(uint8_t initialPercent) {
  ledc_timer_config_t timer = {};
  timer.speed_mode = kBacklightSpeedMode;
  timer.duty_resolution = kBacklightResolution;
  timer.timer_num = kBacklightTimer;
  timer.freq_hz = kBacklightFrequencyHz;
  timer.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&timer) != ESP_OK) return false;

  ledc_channel_config_t channel = {};
  channel.gpio_num = kBacklightPin;
  channel.speed_mode = kBacklightSpeedMode;
  channel.channel = kBacklightChannel;
  channel.intr_type = LEDC_INTR_DISABLE;
  channel.timer_sel = kBacklightTimer;
  channel.duty = kBacklightMaxDuty;
  channel.hpoint = 0;
  if (ledc_channel_config(&channel) != ESP_OK) return false;

  setBacklightPercent(initialPercent);
  Serial.printf(
      "Backlight: BL=GPIO%d PWM=%luHz/%ubit high-active initial=%u%%\n",
      kBacklightPin, static_cast<unsigned long>(kBacklightFrequencyHz),
      static_cast<unsigned>(kBacklightResolutionBits),
      static_cast<unsigned>(min<uint8_t>(initialPercent, 100U)));
  return true;
}

bool renderAndPresentFrame(FrameRenderer renderer, void *userContext,
                           uint32_t *renderTimeUs,
                           uint32_t *presentTimeUs) {
  if (gPanel == nullptr || renderer == nullptr || gDisplayMutex == nullptr) {
    return false;
  }
  if (xSemaphoreTakeRecursive(gDisplayMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  const uint8_t backBufferIndex =
      static_cast<uint8_t>((gFrontBufferIndex + 1U) % kFrameBufferCount);
  const uint32_t renderStartedAt = micros();
  renderer(gFrameBuffers[backBufferIndex], kWidth, kHeight, userContext);
  if (renderTimeUs != nullptr) {
    *renderTimeUs = micros() - renderStartedAt;
  }
  const bool presented = presentFrameBuffer(backBufferIndex, presentTimeUs);
  xSemaphoreGiveRecursive(gDisplayMutex);
  return presented;
}

uint32_t presentedFrameCount() { return gPresentedFrames; }

}  // namespace display_panel
