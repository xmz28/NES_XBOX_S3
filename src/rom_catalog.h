#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rom_catalog {

struct Game {
  char name[40];
  uint32_t offset;
  uint32_t size;
  uint32_t crc32;
  uint16_t mapper;
};

bool begin();
uint16_t count();
const Game *game(uint16_t index);
bool mapGame(uint16_t index);
void unmapGame();
const uint8_t *mappedData();
size_t mappedSize();
const char *mappedName();

}  // namespace rom_catalog
