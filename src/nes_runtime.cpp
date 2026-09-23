#include "nes_runtime.h"

#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include <string.h>

#include "audio_output.h"
#include "display_panel.h"
#include "game_ui.h"
#include "rom_catalog.h"

extern "C" {
#include <bitmap.h>
#include <event.h>
#include <log.h>
#include <nofconfig.h>
#include <nofrendo.h>
#include <noftypes.h>
#include <nes/nesinput.h>
#include <osd.h>
#include <vid_drv.h>
}

namespace {

constexpr int kNesWidth = 256;
constexpr int kNesHeight = 240;
constexpr uint16_t kGameLeft = 144;
constexpr uint32_t kDiagnosticIntervalFrames = 120;
constexpr size_t kAudioSamplesPerFrame = audio_output::kSampleRate / 60U;

volatile uint16_t gButtons = 0;
uint32_t gFrames = 0;
uint32_t gLastDiagnosticUs = 0;
TaskHandle_t gNesTask = nullptr;
std::atomic<TaskHandle_t> gRenderTask{nullptr};
std::atomic<bool> gRunning{false};
std::atomic<bool> gReturnRequested{false};
std::atomic<bool> gReturnCompleted{false};
std::atomic<bool> gRenderBusy{false};
std::atomic<bool> gRenderStop{false};
std::atomic<uint32_t> gDroppedRenderFrames{0};
std::atomic<uint32_t> gLastSnapshotUs{0};
bitmap_t *gDriverBitmap = nullptr;
uint8_t gDriverPixels[kNesWidth * kNesHeight] = {};
uint8_t *const gRenderSnapshot = gDriverPixels;
rgb_t gSourcePalette[256] = {};
uint32_t gRgb565PairPalette[256] = {};
alignas(16) uint32_t gRenderPairPalette[256] = {};
alignas(16) uint32_t gExpandedLine[kNesWidth] = {};
uint8_t gPaletteBrightness = 0;
uint8_t gRenderedUiBrightness = 0;
uint8_t gRenderedUiVolume = 0;
uint8_t gUiRefreshFrames = 0;
void (*gAudioCallback)(void *buffer, int length) = nullptr;
int16_t gAudioSamples[kAudioSamplesPerFrame + 2U] = {};
esp_timer_handle_t gFrameTimer = nullptr;
void (*gFrameTimerCallback)() = nullptr;

uint16_t toRgb565(const rgb_t &color, uint8_t brightness) {
  const uint16_t red = (static_cast<uint16_t>(color.r) * brightness) / 100U;
  const uint16_t green =
      (static_cast<uint16_t>(color.g) * brightness) / 100U;
  const uint16_t blue = (static_cast<uint16_t>(color.b) * brightness) / 100U;
  return static_cast<uint16_t>(((red & 0xF8U) << 8U) |
                               ((green & 0xFCU) << 3U) | (blue >> 3U));
}

void rebuildPaletteIfNeeded() {
  constexpr uint8_t brightness = 100;
  if (brightness == gPaletteBrightness) return;
  for (size_t index = 0; index < 256; ++index) {
    const uint16_t color = toRgb565(gSourcePalette[index], brightness);
    gRgb565PairPalette[index] = static_cast<uint32_t>(color) |
                                (static_cast<uint32_t>(color) << 16U);
  }
  gPaletteBrightness = brightness;
}

struct RenderContext {
  const uint8_t *pixels;
};

void renderNesFrame(uint16_t *frameBuffer, uint16_t width, uint16_t height,
                    void *userContext) {
  auto *context = static_cast<RenderContext *>(userContext);
  if (frameBuffer == nullptr || context == nullptr ||
      context->pixels == nullptr || width != display_panel::kWidth ||
      height != display_panel::kHeight) {
    return;
  }

  for (uint16_t sourceY = 0; sourceY < kNesHeight; ++sourceY) {
    const uint8_t *source = context->pixels + sourceY * kNesWidth;
    uint16_t *top = frameBuffer + (sourceY * 2U) * width + kGameLeft;
    for (uint16_t sourceX = 0; sourceX < kNesWidth; sourceX += 8U) {
      gExpandedLine[sourceX] = gRenderPairPalette[source[sourceX]];
      gExpandedLine[sourceX + 1U] = gRenderPairPalette[source[sourceX + 1U]];
      gExpandedLine[sourceX + 2U] = gRenderPairPalette[source[sourceX + 2U]];
      gExpandedLine[sourceX + 3U] = gRenderPairPalette[source[sourceX + 3U]];
      gExpandedLine[sourceX + 4U] = gRenderPairPalette[source[sourceX + 4U]];
      gExpandedLine[sourceX + 5U] = gRenderPairPalette[source[sourceX + 5U]];
      gExpandedLine[sourceX + 6U] = gRenderPairPalette[source[sourceX + 6U]];
      gExpandedLine[sourceX + 7U] = gRenderPairPalette[source[sourceX + 7U]];
    }
    memcpy(top, gExpandedLine, sizeof(gExpandedLine));
    memcpy(top + width, gExpandedLine, sizeof(gExpandedLine));
  }

  const uint8_t brightness = game_ui::brightnessPercent();
  const uint8_t volume = game_ui::volumePercent();
  if (brightness != gRenderedUiBrightness || volume != gRenderedUiVolume) {
    gRenderedUiBrightness = brightness;
    gRenderedUiVolume = volume;
    gUiRefreshFrames = 3;
  }
  if (gUiRefreshFrames > 0U) {
    game_ui::render(frameBuffer, width, height);
    --gUiRefreshFrames;
  }
}

void logFrameDiagnostics(uint32_t frame, uint32_t renderTimeUs,
                         uint32_t presentTimeUs) {
  if ((frame % kDiagnosticIntervalFrames) != 0U) return;
  const uint32_t nowUs = micros();
  const float measuredFps =
      gLastDiagnosticUs == 0U
          ? 0.0F
          : (kDiagnosticIntervalFrames * 1000000.0F) /
                static_cast<float>(nowUs - gLastDiagnosticUs);
  gLastDiagnosticUs = nowUs;
  const audio_output::Stats audioStats = audio_output::stats();
  Serial.printf(
      "NES: frames=%lu fps=%.1f snapshot=%luus render=%luus "
      "presentSubmit=%luus renderDrop=%lu heap=%u psram=%u bright=%u%% "
      "volume=%u%% buttons=0x%02X audioQ=%lu/%lu pending=%lu drop=%lu "
      "err=%lu peak=%u gap=%lu maxGap=%luus\n",
      static_cast<unsigned long>(frame), measuredFps,
      static_cast<unsigned long>(gLastSnapshotUs.load()),
      static_cast<unsigned long>(renderTimeUs),
      static_cast<unsigned long>(presentTimeUs),
      static_cast<unsigned long>(gDroppedRenderFrames.load()),
      ESP.getFreeHeap(), ESP.getFreePsram(),
      game_ui::brightnessPercent(), game_ui::volumePercent(),
      static_cast<unsigned>(gButtons),
      static_cast<unsigned long>(audioStats.queuedBlocks),
      static_cast<unsigned long>(audioStats.writtenBlocks),
      static_cast<unsigned long>(audioStats.pendingBlocks),
      static_cast<unsigned long>(audioStats.droppedBlocks),
      static_cast<unsigned long>(audioStats.writeErrors),
      static_cast<unsigned>(audioStats.outputPeak),
      static_cast<unsigned long>(audioStats.starvationGaps),
      static_cast<unsigned long>(audioStats.maximumWriteGapUs));
}

void renderTask(void *) {
  RenderContext context{gRenderSnapshot};
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (gRenderStop.load(std::memory_order_acquire)) break;
    uint32_t renderTimeUs = 0;
    uint32_t presentTimeUs = 0;
    if (display_panel::renderAndPresentFrame(
            renderNesFrame, &context, &renderTimeUs, &presentTimeUs)) {
      const uint32_t frame = ++gFrames;
      logFrameDiagnostics(frame, renderTimeUs, presentTimeUs);
    }
    gRenderBusy.store(false, std::memory_order_release);
  }
  gRenderBusy.store(false, std::memory_order_release);
  gRenderTask.store(nullptr, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool startRenderTask() {
  gRenderStop.store(false, std::memory_order_release);
  gRenderBusy.store(false, std::memory_order_release);
  TaskHandle_t task = nullptr;
  if (xTaskCreatePinnedToCore(renderTask, "nes-render", 4096, nullptr, 1,
                              &task, 0) != pdPASS) {
    return false;
  }
  gRenderTask.store(task, std::memory_order_release);
  return true;
}

void stopRenderTask() {
  TaskHandle_t task = gRenderTask.load(std::memory_order_acquire);
  if (task == nullptr) return;
  gRenderStop.store(true, std::memory_order_release);
  xTaskNotifyGive(task);
  const uint32_t deadline = millis() + 500U;
  while (gRenderTask.load(std::memory_order_acquire) != nullptr &&
         static_cast<int32_t>(deadline - millis()) > 0) {
    delay(1);
  }
  task = gRenderTask.exchange(nullptr, std::memory_order_acq_rel);
  if (task != nullptr) vTaskDelete(task);
  gRenderBusy.store(false, std::memory_order_release);
}

void frameTimerThunk(void *) {
  if (gFrameTimerCallback != nullptr) gFrameTimerCallback();
}

void nesTask(void *) {
  Serial.println("NES: Nofrendo task starting on core 1");
  const int result = nofrendo_main(0, nullptr);
  Serial.printf("NES: emulator stopped result=%d\n", result);
  rom_catalog::unmapGame();
  gButtons = 0;
  gReturnRequested.store(false, std::memory_order_release);
  gRunning.store(false, std::memory_order_release);
  gReturnCompleted.store(true, std::memory_order_release);
  gNesTask = nullptr;
  vTaskDelete(nullptr);
}

int logToSerial(const char *message) {
  return message == nullptr ? 0 : Serial.print(message);
}

int videoInit(int, int) { return 0; }
void videoShutdown() {}
int videoSetMode(int, int) { return 0; }

void videoSetPalette(rgb_t *palette) {
  if (palette == nullptr) return;
  memcpy(gSourcePalette, palette, sizeof(gSourcePalette));
  gPaletteBrightness = 0;
  rebuildPaletteIfNeeded();
}

void videoClear(uint8_t color) {
  memset(gDriverPixels, color, sizeof(gDriverPixels));
}

bitmap_t *videoLockWrite() { return gDriverBitmap; }
void videoFreeWrite(int, rect_t *) {}

void videoBlit(bitmap_t *bitmap, int, rect_t *) {
  if (bitmap == nullptr ||
      gRenderBusy.exchange(true, std::memory_order_acq_rel)) {
    gDroppedRenderFrames.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t startedAt = micros();
  for (uint16_t y = 0; y < kNesHeight; ++y) {
    memcpy(gRenderSnapshot + y * kNesWidth, bitmap->line[y], kNesWidth);
  }
  // The NES may replace the palette on Core 1 while Core 0 is scaling this
  // frame.  Snapshot it together with the indexed pixels so one LCD frame
  // cannot use two different palettes across a horizontal boundary.
  memcpy(gRenderPairPalette, gRgb565PairPalette,
         sizeof(gRenderPairPalette));
  gLastSnapshotUs.store(micros() - startedAt, std::memory_order_relaxed);
  TaskHandle_t task = gRenderTask.load(std::memory_order_acquire);
  if (task != nullptr) {
    xTaskNotifyGive(task);
  } else {
    gRenderBusy.store(false, std::memory_order_release);
  }
}

viddriver_t gVideoDriver = {
    "ESP32-S3 RGB 512x480", videoInit,       videoShutdown,
    videoSetMode,           videoSetPalette, videoClear,
    videoLockWrite,         videoFreeWrite,  videoBlit,
    false};

}  // namespace

extern "C" {

char configfilename[] = "nofrendo.cfg";

char *osd_getromdata() {
  return reinterpret_cast<char *>(
      const_cast<uint8_t *>(rom_catalog::mappedData()));
}

size_t osd_getromsize() {
  return rom_catalog::mappedSize();
}

int osd_main(int, char **) {
  config.filename = configfilename;
  Serial.printf("NES: %s, ROM=%u bytes\n", rom_catalog::mappedName(),
                static_cast<unsigned>(osd_getromsize()));
  return main_loop(rom_catalog::mappedName(), system_nes);
}

void osd_fullname(char *fullname, const char *shortname) {
  if (fullname == nullptr) return;
  strncpy(fullname, shortname == nullptr ? "rom.nes" : shortname,
          PATH_MAX);
  fullname[PATH_MAX - 1] = '\0';
}

char *osd_newextension(char *string, char *) { return string; }
int osd_makesnapname(char *, int) { return -1; }

int osd_installtimer(int frequency, void *callback, int, void *, int) {
  if (frequency <= 0 || callback == nullptr) return -1;
  gFrameTimerCallback = reinterpret_cast<void (*)()>(callback);
  if (gFrameTimer == nullptr) {
    const esp_timer_create_args_t timerConfig = {
        .callback = frameTimerThunk,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nes-frame",
        .skip_unhandled_events = true};
    if (esp_timer_create(&timerConfig, &gFrameTimer) != ESP_OK) return -1;
  }
  return esp_timer_start_periodic(gFrameTimer, 1000000ULL / frequency) ==
                 ESP_OK
             ? 0
             : -1;
}

void osd_setsound(void (*playfunc)(void *buffer, int length)) {
  gAudioCallback = playfunc;
}

void osd_pushaudio(void) {
  if (gReturnRequested.load(std::memory_order_acquire)) {
    main_requestquit();
    return;
  }
  if (gAudioCallback == nullptr) return;
  gAudioCallback(gAudioSamples, static_cast<int>(kAudioSamplesPerFrame));
  audio_output::writeMono(gAudioSamples, kAudioSamplesPerFrame,
                          game_ui::volumePercent());
}

void osd_getsoundinfo(sndinfo_t *info) {
  info->sample_rate = audio_output::kSampleRate;
  info->bps = 16;
}

void osd_getvideoinfo(vidinfo_t *info) {
  info->default_width = kNesWidth;
  info->default_height = kNesHeight;
  info->driver = &gVideoDriver;
}

void osd_togglefullscreen(int) {}

void osd_getinput(void) {
  event_set_joypad1(static_cast<uint8_t>(gButtons));
}

void osd_getmouse(int *x, int *y, int *button) {
  if (x != nullptr) *x = 0;
  if (y != nullptr) *y = 0;
  if (button != nullptr) *button = 0;
}

int osd_init() {
  log_chain_logfunc(logToSerial);
  gDriverBitmap =
      bmp_createhw(gDriverPixels, kNesWidth, kNesHeight, kNesWidth);
  if (gDriverBitmap == nullptr) return -1;
  if (!startRenderTask() || !audio_output::begin()) {
    stopRenderTask();
    bmp_destroy(&gDriverBitmap);
    return -1;
  }
  return 0;
}

void osd_shutdown() {
  gAudioCallback = nullptr;
  if (gFrameTimer != nullptr) esp_timer_stop(gFrameTimer);
  stopRenderTask();
  audio_output::end();
  if (gDriverBitmap != nullptr) bmp_destroy(&gDriverBitmap);
}

}  // extern "C"

namespace nes_runtime {

bool begin(uint16_t gameIndex) {
  if (gRunning.exchange(true, std::memory_order_acq_rel)) return true;
  gReturnRequested.store(false, std::memory_order_release);
  gReturnCompleted.store(false, std::memory_order_release);
  gFrames = 0;
  gLastDiagnosticUs = 0;
  gDroppedRenderFrames.store(0, std::memory_order_relaxed);
  gLastSnapshotUs.store(0, std::memory_order_relaxed);
  gRenderedUiBrightness = game_ui::brightnessPercent();
  gRenderedUiVolume = game_ui::volumePercent();
  gUiRefreshFrames = 0;
  gButtons = 0;
  if (!rom_catalog::mapGame(gameIndex)) {
    gRunning.store(false, std::memory_order_release);
    return false;
  }
  const BaseType_t created = xTaskCreatePinnedToCore(
      nesTask, "nofrendo", 12288, nullptr, 1, &gNesTask, 1);
  if (created != pdPASS) {
    rom_catalog::unmapGame();
    gRunning.store(false, std::memory_order_release);
  }
  return created == pdPASS;
}

bool requestReturnToMenu() {
  if (!gRunning.load(std::memory_order_acquire)) return false;
  gButtons = 0;
  return !gReturnRequested.exchange(true, std::memory_order_acq_rel);
}

bool takeReturnCompleted() {
  return gReturnCompleted.exchange(false, std::memory_order_acq_rel);
}

void setButtons(uint16_t buttons) { gButtons = buttons & 0xFFU; }
uint32_t frameCount() { return gFrames; }

}  // namespace nes_runtime
