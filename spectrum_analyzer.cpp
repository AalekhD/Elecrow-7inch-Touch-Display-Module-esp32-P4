#include <arduinoFFT.h>
#include <cmath>
#include <cstring>
#include "spectrum_analyzer.h"

namespace {

float realSamples[spectrumSamples];
float imaginarySamples[spectrumSamples];
ArduinoFFT<float> transform(realSamples, imaginarySamples, spectrumSamples,
                            static_cast<float>(spectrumSampleRate), true);

float toDbfs(float amplitude) {
  return fminf(0.0f, fmaxf(-96.0f, 20.0f * log10f(fmaxf(amplitude, 0.0000001f))));
}

}

void spectrum_analyze(const int16_t *samples, SpectrumFrame &result) {
  result = SpectrumFrame{};
  static float windowSum = 0.0f;
  if (windowSum == 0.0f) {
    for (float &sample : realSamples) {
      sample = 1.0f;
    }
    transform.windowing(FFTWindow::Hann, FFTDirection::Forward, false);
    for (float sample : realSamples) {
      windowSum += sample;
    }
  }
  float mean = 0.0f;
  for (size_t index = 0; index < spectrumSamples; ++index) {
    mean += samples[index] / 32768.0f;
    if (samples[index] >= 32760 || samples[index] <= -32760) {
      result.clipped = true;
    }
  }
  mean /= spectrumSamples;
  float energy = 0.0f;
  for (size_t index = 0; index < spectrumSamples; ++index) {
    realSamples[index] = samples[index] / 32768.0f - mean;
    energy += realSamples[index] * realSamples[index];
  }
  result.signalDbfs = toDbfs(sqrtf(energy / spectrumSamples));
  if (result.signalDbfs < -75.0f) {
    return;
  }
  memset(imaginarySamples, 0, sizeof(imaginarySamples));
  transform.windowing(FFTWindow::Hann, FFTDirection::Forward, false);
  transform.compute(FFTDirection::Forward);
  transform.complexToMagnitude();

  constexpr float binHz = static_cast<float>(spectrumSampleRate) / spectrumSamples;
  constexpr size_t firstBin = 8;
  constexpr size_t lastBin = 896;
  size_t selectedBins[spectrumPeakCount] = {};
  float strongestDbfs = -96.0f;
  for (size_t rank = 0; rank < spectrumPeakCount; ++rank) {
    size_t bestBin = 0;
    float bestMagnitude = 0.0f;
    for (size_t bin = firstBin; bin <= lastBin; ++bin) {
      if (realSamples[bin] <= realSamples[bin - 1] ||
          realSamples[bin] < realSamples[bin + 1]) {
        continue;
      }
      bool separated = true;
      for (size_t previous = 0; previous < rank; ++previous) {
        int distance = static_cast<int>(bin) - static_cast<int>(selectedBins[previous]);
        if (distance >= -3 && distance <= 3) {
          separated = false;
        }
      }
      if (separated && realSamples[bin] > bestMagnitude) {
        bestMagnitude = realSamples[bin];
        bestBin = bin;
      }
    }
    if (bestBin == 0) {
      break;
    }
    float left = logf(fmaxf(realSamples[bestBin - 1], 0.000000000001f));
    float center = logf(fmaxf(realSamples[bestBin], 0.000000000001f));
    float right = logf(fmaxf(realSamples[bestBin + 1], 0.000000000001f));
    float denominator = left - 2.0f * center + right;
    float offset = fabsf(denominator) > 0.000001f ?
                     0.5f * (left - right) / denominator : 0.0f;
    offset = fmaxf(-0.5f, fminf(0.5f, offset));
    float magnitude = expf(center - 0.25f * (left - right) * offset);
    float amplitude = magnitude * 2.0f / windowSum;
    float strength = toDbfs(amplitude);
    if (rank == 0) {
      strongestDbfs = strength;
    }
    if (strength < -70.0f || strength < strongestDbfs - 28.0f) {
      break;
    }
    selectedBins[rank] = bestBin;
    result.peaks[rank].frequencyHz = fmaxf(60.0f, fminf(7000.0f, (bestBin + offset) * binHz));
    result.peaks[rank].dbfs = strength;
    ++result.count;
  }
}