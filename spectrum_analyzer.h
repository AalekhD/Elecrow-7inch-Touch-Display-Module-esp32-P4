#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t spectrumSamples = 2048;
constexpr size_t spectrumPeakCount = 5;
constexpr unsigned int spectrumSampleRate = 16000;

struct SpectrumPeak {
  float frequencyHz = 0.0f;
  float dbfs = -96.0f;
};

struct SpectrumFrame {
  SpectrumPeak peaks[spectrumPeakCount];
  size_t count = 0;
  float signalDbfs = -96.0f;
  bool clipped = false;
};

void spectrum_analyze(const int16_t *samples, SpectrumFrame &result);