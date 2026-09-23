#include "rom_catalog.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <string.h>

namespace rom_catalog {
namespace {

constexpr uint16_t kMaximumGames = 60;
constexpr uint32_t kPackVersion = 1;
constexpr char kPackMagic[8] = {'N', 'E', 'S', 'P', 'A', 'C', 'K', '1'};

#pragma pack(push, 1)
struct PackHeader {
  char magic[8];
  uint32_t version;
  uint32_t gameCount;
  uint32_t entrySize;
  uint32_t totalSize;
  uint8_t reserved[8];
};

struct PackEntry {
  char name[40];
  uint32_t offset;
  uint32_t size;
  uint32_t crc32;
  uint16_t mapper;
  uint8_t reserved[10];
};
#pragma pack(pop)

static_assert(sizeof(PackHeader) == 32, "ROM pack header layout changed");
static_assert(sizeof(PackEntry) == 64, "ROM pack entry layout changed");

const esp_partition_t *gPartition = nullptr;
Game gGames[kMaximumGames] = {};
uint16_t gGameCount = 0;
uint8_t *gMappedData = nullptr;
size_t gMappedSize = 0;
uint16_t gMappedIndex = UINT16_MAX;

bool rangeIsValid(uint32_t offset, uint32_t size, uint32_t totalSize) {
  return offset >= sizeof(PackHeader) && size >= 16 && offset <= totalSize &&
         size <= totalSize - offset;
}

}  // namespace

bool begin() {
  gPartition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "roms");
  if (gPartition == nullptr) {
    Serial.println("ROMS: partition 'roms' not found");
    return false;
  }

  PackHeader header = {};
  if (esp_partition_read(gPartition, 0, &header, sizeof(header)) != ESP_OK ||
      memcmp(header.magic, kPackMagic, sizeof(kPackMagic)) != 0 ||
      header.version != kPackVersion || header.gameCount < 20 ||
      header.gameCount > kMaximumGames ||
      header.entrySize != sizeof(PackEntry) ||
      header.totalSize > gPartition->size) {
    Serial.println("ROMS: invalid or missing NESPACK1 image");
    return false;
  }

  for (uint32_t index = 0; index < header.gameCount; ++index) {
    PackEntry entry = {};
    const size_t position = sizeof(PackHeader) + index * sizeof(PackEntry);
    if (esp_partition_read(gPartition, position, &entry, sizeof(entry)) !=
            ESP_OK ||
        !rangeIsValid(entry.offset, entry.size, header.totalSize) ||
        memchr(entry.name, '\0', sizeof(entry.name)) == nullptr) {
      Serial.printf("ROMS: invalid catalog entry %lu\n",
                    static_cast<unsigned long>(index));
      gGameCount = 0;
      return false;
    }
    memcpy(gGames[index].name, entry.name, sizeof(entry.name));
    gGames[index].offset = entry.offset;
    gGames[index].size = entry.size;
    gGames[index].crc32 = entry.crc32;
    gGames[index].mapper = entry.mapper;
  }
  gGameCount = static_cast<uint16_t>(header.gameCount);
  Serial.printf("ROMS: %u games, image=%lu bytes, partition=%lu bytes\n",
                gGameCount, static_cast<unsigned long>(header.totalSize),
                static_cast<unsigned long>(gPartition->size));
  return true;
}

uint16_t count() { return gGameCount; }

const Game *game(uint16_t index) {
  return index < gGameCount ? &gGames[index] : nullptr;
}

bool mapGame(uint16_t index) {
  if (index >= gGameCount || gPartition == nullptr) return false;
  unmapGame();
  const Game &selected = gGames[index];
  // Nofrendo's CPU and several old mapper implementations occasionally write
  // through a bank pointer that normally points at cartridge ROM.  XIP Flash
  // is read-only on ESP32-S3, so keep the selected game in writable PSRAM.
  gMappedData = static_cast<uint8_t *>(heap_caps_malloc(
      selected.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (gMappedData == nullptr) {
    Serial.printf("ROMS: PSRAM allocation failed for %s (%lu bytes)\n",
                  selected.name, static_cast<unsigned long>(selected.size));
    return false;
  }
  const esp_err_t result = esp_partition_read(
      gPartition, selected.offset, gMappedData, selected.size);
  if (result != ESP_OK) {
    Serial.printf("ROMS: partition read failed for %s: %s\n", selected.name,
                  esp_err_to_name(result));
    heap_caps_free(gMappedData);
    gMappedData = nullptr;
    return false;
  }
  gMappedSize = selected.size;
  gMappedIndex = index;
  Serial.printf("ROMS: loaded #%u %s to PSRAM, %lu bytes, mapper %u\n",
                index + 1, selected.name,
                static_cast<unsigned long>(selected.size), selected.mapper);
  return true;
}

void unmapGame() {
  if (gMappedData != nullptr) heap_caps_free(gMappedData);
  gMappedData = nullptr;
  gMappedSize = 0;
  gMappedIndex = UINT16_MAX;
}

const uint8_t *mappedData() { return gMappedData; }
size_t mappedSize() { return gMappedSize; }

const char *mappedName() {
  return gMappedIndex < gGameCount ? gGames[gMappedIndex].name : "rom.nes";
}

}  // namespace rom_catalog
