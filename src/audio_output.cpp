#include "audio_output.h"

#include <Arduino.h>
#include <atomic>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace audio_output {
namespace {

i2s_chan_handle_t gTxChannel = nullptr;
std::atomic<bool> gReady{false};
constexpr size_t kChunkSamples = 128;
constexpr size_t kMaximumBlockSamples = kSampleRate / 60U;
constexpr UBaseType_t kQueueDepth = 6;
constexpr TickType_t kQueueWait = pdMS_TO_TICKS(25);
constexpr uint32_t kDmaDescriptorCount = 12;
constexpr uint32_t kDmaFramesPerDescriptor = 512;
constexpr uint32_t kStarvationGapUs = 100000;
static_assert(kSampleRate % 60U == 0U,
              "audio sample rate must divide evenly into 60 Hz");
int16_t gStereo[kChunkSamples * 2U] = {};
QueueHandle_t gAudioQueue = nullptr;
std::atomic<uint32_t> gQueuedBlocks{0};
std::atomic<uint32_t> gDroppedBlocks{0};
std::atomic<uint32_t> gWrittenBlocks{0};
std::atomic<uint32_t> gWriteErrors{0};
std::atomic<uint16_t> gOutputPeak{0};
std::atomic<uint32_t> gStarvationGaps{0};
std::atomic<uint32_t> gMaximumWriteGapUs{0};
std::atomic<TaskHandle_t> gAudioTask{nullptr};

struct AudioBlock {
  uint16_t sampleCount;
  uint8_t volumePercent;
  int16_t samples[kMaximumBlockSamples];
};

void audioTask(void *) {
  AudioBlock block = {};
  uint32_t previousWriteUs = micros();
  bool haveWritten = false;
  while (true) {
    if (xQueueReceive(gAudioQueue, &block, portMAX_DELAY) != pdTRUE) continue;
    if (block.sampleCount == 0) break;
    size_t offset = 0;
    uint16_t blockPeak = 0;
    while (offset < block.sampleCount) {
      const size_t count = min(kChunkSamples,
                               static_cast<size_t>(block.sampleCount) - offset);
      for (size_t index = 0; index < count; ++index) {
        const int32_t scaled =
            (static_cast<int32_t>(block.samples[offset + index]) *
             block.volumePercent) /
            100;
        // Keep the first recovery build bit-transparent.  The former
        // ESP_I2S path was proven audible on this exact ESP32-S3 board; EQ can
        // be restored only after the raw I2S path is confirmed again.
        const int16_t shaped = static_cast<int16_t>(scaled);
        const uint16_t magnitude = static_cast<uint16_t>(
            shaped == INT16_MIN ? 32768 : abs(static_cast<int>(shaped)));
        blockPeak = max(blockPeak, magnitude);
        gStereo[index * 2U] = shaped;
        gStereo[index * 2U + 1U] = shaped;
      }
      const size_t bytes = count * 2U * sizeof(int16_t);
      const uint32_t nowUs = micros();
      const uint32_t gapUs = nowUs - previousWriteUs;
      if (haveWritten) {
        uint32_t previousMaximum =
            gMaximumWriteGapUs.load(std::memory_order_relaxed);
        while (gapUs > previousMaximum &&
               !gMaximumWriteGapUs.compare_exchange_weak(
                   previousMaximum, gapUs, std::memory_order_relaxed)) {
        }
        // 12 x 512 frames provide about 128 ms at 48 kHz.  Count only gaps
        // that approach exhaustion; maxGap still records smaller stalls.
        if (gapUs > kStarvationGapUs) {
          gStarvationGaps.fetch_add(1, std::memory_order_relaxed);
        }
      }
      size_t bytesWritten = 0;
      const esp_err_t result = i2s_channel_write(
          gTxChannel, gStereo, bytes, &bytesWritten, portMAX_DELAY);
      previousWriteUs = micros();
      haveWritten = true;
      if (result != ESP_OK || bytesWritten != bytes) {
        gWriteErrors.fetch_add(1, std::memory_order_relaxed);
      }
      offset += count;
    }
    gOutputPeak.store(blockPeak, std::memory_order_relaxed);
    gWrittenBlocks.fetch_add(1, std::memory_order_relaxed);
  }
  gAudioTask.store(nullptr, std::memory_order_release);
  vTaskDelete(nullptr);
}

}  // namespace

bool begin() {
  if (gReady.load(std::memory_order_acquire)) return true;
  gQueuedBlocks.store(0, std::memory_order_relaxed);
  gDroppedBlocks.store(0, std::memory_order_relaxed);
  gWrittenBlocks.store(0, std::memory_order_relaxed);
  gWriteErrors.store(0, std::memory_order_relaxed);
  gOutputPeak.store(0, std::memory_order_relaxed);
  gStarvationGaps.store(0, std::memory_order_relaxed);
  gMaximumWriteGapUs.store(0, std::memory_order_relaxed);
  i2s_chan_config_t channelConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channelConfig.dma_desc_num = kDmaDescriptorCount;
  channelConfig.dma_frame_num = kDmaFramesPerDescriptor;
  channelConfig.auto_clear_after_cb = true;

  esp_err_t result = i2s_new_channel(&channelConfig, &gTxChannel, nullptr);
  i2s_std_config_t standardConfig = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = static_cast<gpio_num_t>(kPinBclk),
          .ws = static_cast<gpio_num_t>(kPinLrc),
          .dout = static_cast<gpio_num_t>(kPinDin),
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {.mclk_inv = false,
                           .bclk_inv = false,
                           .ws_inv = false},
      },
  };
  if (result == ESP_OK) {
    result = i2s_channel_init_std_mode(gTxChannel, &standardConfig);
  }

  static int16_t silence[kDmaFramesPerDescriptor * 2U] = {};
  if (result == ESP_OK) {
    size_t loaded = 0;
    do {
      result = i2s_channel_preload_data(gTxChannel, silence, sizeof(silence),
                                        &loaded);
    } while (result == ESP_OK && loaded > 0);
  }
  if (result == ESP_OK) result = i2s_channel_enable(gTxChannel);
  gReady.store(result == ESP_OK, std::memory_order_release);

  if (gReady.load(std::memory_order_acquire)) {
    gAudioQueue = xQueueCreate(kQueueDepth, sizeof(AudioBlock));
    TaskHandle_t createdTask = nullptr;
    if (gAudioQueue == nullptr ||
        xTaskCreatePinnedToCore(audioTask, "nes-audio", 4096, nullptr, 2,
                                &createdTask, 1) != pdPASS) {
      gReady.store(false, std::memory_order_release);
    } else {
      gAudioTask.store(createdTask, std::memory_order_release);
    }
  }
  if (!gReady.load(std::memory_order_acquire)) {
    if (gAudioQueue != nullptr) {
      vQueueDelete(gAudioQueue);
      gAudioQueue = nullptr;
    }
    if (gTxChannel != nullptr) {
      i2s_channel_disable(gTxChannel);
      i2s_del_channel(gTxChannel);
      gTxChannel = nullptr;
    }
  }
  Serial.printf(
      "AUDIO: MAX98357A BCLK=%d LRC=%d DIN=%d, %lu Hz, "
      "DMA=%lux%lu raw stereo, %s%s%s\n",
                kPinBclk, kPinLrc, kPinDin,
                static_cast<unsigned long>(kSampleRate),
                static_cast<unsigned long>(kDmaDescriptorCount),
                static_cast<unsigned long>(kDmaFramesPerDescriptor),
                gReady.load() ? "I2S ready" : "I2S init failed",
                gReady.load() ? "" : ": ",
                gReady.load() ? "" : esp_err_to_name(result));
  return gReady.load(std::memory_order_acquire);
}

void end() {
  if (!gReady.exchange(false, std::memory_order_acq_rel) &&
      gAudioQueue == nullptr) {
    return;
  }
  if (gAudioQueue != nullptr &&
      gAudioTask.load(std::memory_order_acquire) != nullptr) {
    AudioBlock stopBlock = {};
    xQueueSend(gAudioQueue, &stopBlock, portMAX_DELAY);
    const uint32_t deadline = millis() + 500U;
    while (gAudioTask.load(std::memory_order_acquire) != nullptr &&
           static_cast<int32_t>(deadline - millis()) > 0) {
      delay(1);
    }
    TaskHandle_t remainingTask =
        gAudioTask.exchange(nullptr, std::memory_order_acq_rel);
    if (remainingTask != nullptr) {
      vTaskDelete(remainingTask);
    }
  }
  if (gTxChannel != nullptr) {
    i2s_channel_disable(gTxChannel);
    i2s_del_channel(gTxChannel);
    gTxChannel = nullptr;
  }
  if (gAudioQueue != nullptr) {
    vQueueDelete(gAudioQueue);
    gAudioQueue = nullptr;
  }
}

bool writeMono(const int16_t *samples, size_t sampleCount,
               uint8_t volumePercent) {
  if (!gReady.load(std::memory_order_acquire) || samples == nullptr || sampleCount == 0 ||
      sampleCount > kMaximumBlockSamples) {
    gDroppedBlocks.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  AudioBlock block = {};
  block.sampleCount = static_cast<uint16_t>(sampleCount);
  block.volumePercent = volumePercent;
  memcpy(block.samples, samples, block.sampleCount * sizeof(int16_t));
  if (xQueueSend(gAudioQueue, &block, kQueueWait) != pdTRUE) {
    gDroppedBlocks.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  gQueuedBlocks.fetch_add(1, std::memory_order_relaxed);
  return true;
}

Stats stats() {
  return {gQueuedBlocks.load(std::memory_order_relaxed),
          gDroppedBlocks.load(std::memory_order_relaxed),
          gWrittenBlocks.load(std::memory_order_relaxed),
          gWriteErrors.load(std::memory_order_relaxed),
          gAudioQueue == nullptr
              ? 0U
              : static_cast<uint32_t>(uxQueueMessagesWaiting(gAudioQueue)),
          gOutputPeak.load(std::memory_order_relaxed),
          gStarvationGaps.load(std::memory_order_relaxed),
          gMaximumWriteGapUs.load(std::memory_order_relaxed)};
}

}  // namespace audio_output
