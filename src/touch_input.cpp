#include "touch_input.h"

#include <Arduino.h>
#include <Wire.h>

#include "display_panel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace touch_input {
namespace {

constexpr int kPinSda = 47;
constexpr int kPinScl = 48;
constexpr int kPinInterrupt = 2;
constexpr uint32_t kI2cFrequencyHz = 400000U;
constexpr uint32_t kPollIntervalMs = 5U;
constexpr uint8_t kGt911Addresses[] = {0x5D, 0x14};
constexpr uint16_t kProductIdRegister = 0x8140;
constexpr uint16_t kStatusRegister = 0x814E;
constexpr uint16_t kFirstPointRegister = 0x8150;
constexpr uint8_t kDataReadyMask = 0x80;
constexpr uint8_t kTouchCountMask = 0x0F;
constexpr uint8_t kReadRetryCount = 3;
constexpr uint32_t kReadRetryDelayMs = 2U;
constexpr uint32_t kRecoveryFailureThreshold = 5U;
constexpr BaseType_t kTouchCore = 0;
// The LCD renderer also runs on Core 0. Poll touch only when that core is idle
// so GT911 traffic cannot preempt a game-frame conversion.
constexpr UBaseType_t kTouchPriority = 0;

QueueHandle_t gQueue = nullptr;
TaskHandle_t gTask = nullptr;
SemaphoreHandle_t gI2cMutex = nullptr;
uint8_t gAddress = 0;
bool gI2cInitialized = false;
bool gReady = false;
volatile uint32_t gPublishedEvents = 0;
volatile uint32_t gReadFailures = 0;
volatile uint32_t gRecoveries = 0;

bool probeLocked(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission(true) == 0;
}

bool readOnceLocked(uint16_t reg, uint8_t *data, size_t length) {
  if (gAddress == 0 || data == nullptr || length == 0) {
    return false;
  }
  Wire.beginTransmission(gAddress);
  Wire.write(static_cast<uint8_t>(reg >> 8U));
  Wire.write(static_cast<uint8_t>(reg));
  // GT911 keeps the selected register across STOP. Using two complete I2C
  // transactions avoids the ESP32 Arduino driver's intermittent
  // i2cWriteReadNonStop ESP_ERR_INVALID_STATE path.
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  const size_t received = Wire.requestFrom(gAddress, length, true);
  if (received != length) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    data[index] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool readLocked(uint16_t reg, uint8_t *data, size_t length) {
  for (uint8_t retry = 0; retry <= kReadRetryCount; ++retry) {
    if (readOnceLocked(reg, data, length)) {
      return true;
    }
    if (retry < kReadRetryCount) {
      vTaskDelay(pdMS_TO_TICKS(kReadRetryDelayMs));
    }
  }
  return false;
}

bool readRegister(uint16_t reg, uint8_t *data, size_t length) {
  if (gI2cMutex == nullptr ||
      xSemaphoreTake(gI2cMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  const bool succeeded = readLocked(reg, data, length);
  xSemaphoreGive(gI2cMutex);
  return succeeded;
}

bool writeByteLocked(uint16_t reg, uint8_t value) {
  if (gAddress == 0) {
    return false;
  }
  Wire.beginTransmission(gAddress);
  Wire.write(static_cast<uint8_t>(reg >> 8U));
  Wire.write(static_cast<uint8_t>(reg));
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool writeByte(uint16_t reg, uint8_t value) {
  if (gI2cMutex == nullptr ||
      xSemaphoreTake(gI2cMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  const bool succeeded = writeByteLocked(reg, value);
  xSemaphoreGive(gI2cMutex);
  return succeeded;
}

bool initializeI2cLocked(bool forceRestart) {
  if (forceRestart && gI2cInitialized) {
    Wire.end();
    gI2cInitialized = false;
    vTaskDelay(pdMS_TO_TICKS(kReadRetryDelayMs));
  }
  if (gI2cInitialized) {
    return true;
  }
  if (!Wire.begin(kPinSda, kPinScl, kI2cFrequencyHz)) {
    return false;
  }
  Wire.setTimeOut(50);
  gI2cInitialized = true;
  return true;
}

bool initializeGt911Locked(bool verbose) {
  gAddress = 0;
  for (uint8_t address : kGt911Addresses) {
    const bool found = probeLocked(address);
    if (verbose) {
      Serial.printf("GT911: address 0x%02X %s\n", address,
                    found ? "ACK" : "no response");
    }
    if (found && gAddress == 0) {
      gAddress = address;
    }
  }
  if (gAddress == 0) {
    return false;
  }

  uint8_t productIdBytes[4] = {};
  const bool productIdRead =
      readLocked(kProductIdRegister, productIdBytes, sizeof(productIdBytes));
  if (verbose) {
    char productId[5] = {};
    for (size_t index = 0; index < sizeof(productIdBytes); ++index) {
      productId[index] =
          productIdBytes[index] >= 0x20 && productIdBytes[index] <= 0x7E
              ? static_cast<char>(productIdBytes[index])
              : '.';
    }
    if (productIdRead) {
      Serial.printf(
          "GT911: address=0x%02X product='%s' raw=%02X %02X %02X %02X\n",
          gAddress, productId, productIdBytes[0], productIdBytes[1],
          productIdBytes[2], productIdBytes[3]);
    } else {
      Serial.printf("GT911: product ID read failed at address 0x%02X\n",
                    gAddress);
    }
  }
  writeByteLocked(kStatusRegister, 0);
  return true;
}

bool recover() {
  if (gI2cMutex == nullptr ||
      xSemaphoreTake(gI2cMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  const bool recovered =
      initializeI2cLocked(true) && initializeGt911Locked(false);
  xSemaphoreGive(gI2cMutex);
  if (recovered) {
    gRecoveries = gRecoveries + 1U;
  }
  return recovered;
}

void transformCoordinates(uint16_t rawX, uint16_t rawY, uint16_t &x,
                          uint16_t &y) {
  // This exact panel/GT911 pair was previously verified without swap or flip.
  x = static_cast<uint16_t>(
      constrain(static_cast<int32_t>(rawX), 0,
                static_cast<int32_t>(display_panel::kWidth - 1U)));
  y = static_cast<uint16_t>(
      constrain(static_cast<int32_t>(rawY), 0,
                static_cast<int32_t>(display_panel::kHeight - 1U)));
}

void publish(const State &state) {
  if (gQueue != nullptr) {
    xQueueOverwrite(gQueue, &state);
    gPublishedEvents = gPublishedEvents + 1U;
  }
}

void task(void *) {
  State state = {};
  TickType_t lastWakeTime = xTaskGetTickCount();
  uint32_t consecutiveReadFailures = 0;
  for (;;) {
    uint8_t status = 0;
    if (!readRegister(kStatusRegister, &status, 1)) {
      gReadFailures = gReadFailures + 1U;
      ++consecutiveReadFailures;
      if (consecutiveReadFailures >= kRecoveryFailureThreshold) {
        Serial.println("GT911: recovering after 5 consecutive read failures");
        Serial.println(recover() ? "GT911: recovery succeeded"
                                 : "GT911: recovery failed");
        consecutiveReadFailures = 0;
      }
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(kPollIntervalMs));
      continue;
    }
    consecutiveReadFailures = 0;

    if ((status & kDataReadyMask) != 0U) {
      const uint8_t touchCount = status & kTouchCountMask;
      if (touchCount > 0 && touchCount <= 5) {
        uint8_t point[8] = {};
        if (readRegister(kFirstPointRegister, point, sizeof(point))) {
          const uint16_t rawX =
              static_cast<uint16_t>(point[0] | (point[1] << 8U));
          const uint16_t rawY =
              static_cast<uint16_t>(point[2] | (point[3] << 8U));
          uint16_t x = 0;
          uint16_t y = 0;
          transformCoordinates(rawX, rawY, x, y);
          const bool changed = !state.pressed || state.x != x || state.y != y;
          state.valid = true;
          state.pressed = true;
          state.x = x;
          state.y = y;
          if (changed) {
            ++state.sequence;
            state.sampledAtUs = micros();
            publish(state);
          }
        }
      } else if (touchCount == 0 && state.pressed) {
        state.pressed = false;
        ++state.sequence;
        state.sampledAtUs = micros();
        publish(state);
      }
      writeByte(kStatusRegister, 0);
    }
    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(kPollIntervalMs));
  }
}

}  // namespace

bool begin() {
  pinMode(kPinInterrupt, INPUT_PULLUP);
  gI2cMutex = xSemaphoreCreateMutex();
  gQueue = xQueueCreate(1, sizeof(State));
  if (gI2cMutex == nullptr || gQueue == nullptr) {
    Serial.println("GT911: failed to create synchronization objects");
    return false;
  }
  if (xSemaphoreTake(gI2cMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  const bool initialized =
      initializeI2cLocked(false) && initializeGt911Locked(true);
  xSemaphoreGive(gI2cMutex);
  if (!initialized) {
    Serial.printf("GT911: not found on SDA GPIO%d / SCL GPIO%d\n", kPinSda,
                  kPinScl);
    return false;
  }
  const BaseType_t created = xTaskCreatePinnedToCore(
      task, "gt911_poll", 4096, nullptr, kTouchPriority, &gTask, kTouchCore);
  if (created != pdPASS) {
    Serial.println("GT911: failed to create polling task");
    return false;
  }
  gReady = true;
  Serial.printf(
      "GT911: ready, SDA=%d SCL=%d INT=%d, %lu kHz, %lu ms polling, "
      "direct 800x480 coordinates\n",
      kPinSda, kPinScl, kPinInterrupt,
      static_cast<unsigned long>(kI2cFrequencyHz / 1000U),
      static_cast<unsigned long>(kPollIntervalMs));
  return true;
}

bool takeLatest(State &state) {
  return gQueue != nullptr && xQueueReceive(gQueue, &state, 0) == pdTRUE;
}

bool isReady() { return gReady; }

Stats stats() {
  return {gPublishedEvents, gReadFailures, gRecoveries};
}

}  // namespace touch_input
