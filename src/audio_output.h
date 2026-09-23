#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audio_output {

constexpr int kPinBclk = 3;
constexpr int kPinLrc = 45;
constexpr int kPinDin = 46;
// MAX98357A only accepts a fixed set of LRCLK rates.  48 kHz is supported and
// divides evenly into the NES 60 Hz frame rate (800 samples per frame).
constexpr uint32_t kSampleRate = 48000;

struct Stats {
  uint32_t queuedBlocks;
  uint32_t droppedBlocks;
  uint32_t writtenBlocks;
  uint32_t writeErrors;
  uint32_t pendingBlocks;
  uint16_t outputPeak;
  uint32_t starvationGaps;
  uint32_t maximumWriteGapUs;
};

bool begin();
void end();
bool writeMono(const int16_t *samples, size_t sampleCount,
               uint8_t volumePercent);
Stats stats();

}  // namespace audio_output
