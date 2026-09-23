#pragma once

#include <stdint.h>

namespace touch_input {

struct State {
  bool valid;
  bool pressed;
  uint16_t x;
  uint16_t y;
  uint32_t sequence;
  uint32_t sampledAtUs;
};

struct Stats {
  uint32_t publishedEvents;
  uint32_t readFailures;
  uint32_t recoveries;
};

bool begin();
bool takeLatest(State &state);
bool isReady();
Stats stats();

}  // namespace touch_input
