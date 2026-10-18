#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <driver/i2s_std.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "audio_demo.h"

namespace {

constexpr int amplifierPin = 30;
constexpr size_t sampleRate = 16000;
constexpr size_t recordingSamples = sampleRate * 4;
constexpr size_t chunkSamples = 1024;
constexpr size_t chimeSamples = sampleRate * 3 / 4;
constexpr size_t warmupSamples = 2000;

enum class Command { None, Chime, Record, Analyze, Stop };
I2SClass microphone;
I2SClass speaker;
std::atomic<Command> command{Command::None};
std::atomic<AudioStage> stage{AudioStage::Unavailable};
std::atomic<unsigned int> progress{0};
std::atomic<unsigned int> level{0};
std::atomic<unsigned int> volume{35};
std::atomic<const char *> message{"Audio unavailable"};
int16_t *recording = nullptr;
int16_t chunk[chunkSamples];
size_t position = 0;
size_t discarded = 0;
bool initialized = false;
bool draining = false;
size_t drainRemaining = 0;
float recordingGain = 1.0f;
int16_t spectrumInput[spectrumSamples];
SpectrumFrame spectrumFrame;
portMUX_TYPE spectrumLock = portMUX_INITIALIZER_UNLOCKED;

void publishSpectrum(const SpectrumFrame &frame) {
  portENTER_CRITICAL(&spectrumLock);
  spectrumFrame = frame;
  portEXIT_CRITICAL(&spectrumLock);
}

void finish(const char *result) {
  digitalWrite(amplifierPin, HIGH);
  if (recording != nullptr) {
    memset(recording, 0, recordingSamples * sizeof(int16_t));
  }
  memset(chunk, 0, sizeof(chunk));
  memset(spectrumInput, 0, sizeof(spectrumInput));
  publishSpectrum(SpectrumFrame{});
  level.store(0);
  progress.store(0);
  draining = false;
  message.store(result);
  stage.store(initialized ? AudioStage::Ready : AudioStage::Unavailable);
}

void fail(const char *reason) {
  finish(reason);
  message.store(reason);
  stage.store(AudioStage::Unavailable);
  Serial.println(reason);
}

void updateLevel(const int16_t *samples, size_t count) {
  int peak = 0;
  for (size_t index = 0; index < count; ++index) {
    int magnitude = abs(static_cast<int>(samples[index]));
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  level.store(static_cast<unsigned int>(peak * 100 / 32768));
}

bool writeChunk(size_t count) {
  updateLevel(chunk, count);
  size_t bytes = count * sizeof(int16_t);
  if (speaker.write(reinterpret_cast<uint8_t *>(chunk), bytes) != bytes) {
    fail("Speaker write failed");
    return false;
  }
  return true;
}

void startOutput(AudioStage nextStage) {
  position = 0;
  draining = false;
  digitalWrite(amplifierPin, LOW);
  message.store(nextStage == AudioStage::Chime ? "Welcome" : "Playing your recording");
  stage.store(nextStage);
}

} 

bool audio_demo_begin() {
  pinMode(amplifierPin, OUTPUT);
  digitalWrite(amplifierPin, HIGH);
  recording = static_cast<int16_t *>(heap_caps_calloc(
    recordingSamples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (recording == nullptr) {
    fail("Not enough PSRAM for recording");
    return false;
  }
  microphone.setPinsPdmRx(24, 26);
  microphone.setTimeout(250);
  if (!microphone.begin(I2S_MODE_PDM_RX, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    free(recording);
    recording = nullptr;
    fail("Microphone initialization failed");
    return false;
  }
  speaker.setPins(22, 21, 23);
  if (!speaker.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_MONO, I2S_STD_SLOT_BOTH)) {
    microphone.end();
    free(recording);
    recording = nullptr;
    fail("Speaker initialization failed");
    return false;
  }
  initialized = true;
  finish("Ready to record");
  return true;
}

bool audio_demo_start(bool welcomeChime) {
  if (stage.load() != AudioStage::Ready) {
    return false;
  }
  Command expected = Command::None;
  return command.compare_exchange_strong(expected,
    welcomeChime ? Command::Chime : Command::Record);
}

bool audio_demo_start_analysis() {
  if (stage.load() != AudioStage::Ready) {
    return false;
  }
  Command expected = Command::None;
  return command.compare_exchange_strong(expected, Command::Analyze);
}

SpectrumFrame audio_demo_spectrum() {
  portENTER_CRITICAL(&spectrumLock);
  SpectrumFrame snapshot = spectrumFrame;
  portEXIT_CRITICAL(&spectrumLock);
  return snapshot;
}

void audio_demo_stop() {
  command.store(Command::Stop);
}

void audio_demo_set_volume(unsigned int percent) {
  volume.store(percent > 100 ? 100 : percent);
}

AudioStatus audio_demo_status() {
  return {stage.load(), progress.load(), level.load(), message.load()};
}

void audio_demo_process() {
  Command next = command.exchange(Command::None);
  if (next == Command::Stop) {
    if (initialized && stage.load() != AudioStage::Unavailable) {
      finish("Stopped");
    }
    return;
  }
  if (next == Command::Chime) {
    startOutput(AudioStage::Chime);
  } else if (next == Command::Record || next == Command::Analyze) {
    digitalWrite(amplifierPin, HIGH);
    position = 0;
    discarded = 0;
    progress.store(0);
    publishSpectrum(SpectrumFrame{});
    message.store(next == Command::Analyze ? "Analyzing live microphone" : "Listening - speak now");
    if (i2s_channel_disable(microphone.rxChan()) != ESP_OK ||
        i2s_channel_enable(microphone.rxChan()) != ESP_OK) {
      fail("Microphone restart failed");
      return;
    }
    stage.store(next == Command::Analyze ? AudioStage::Analyzing : AudioStage::Recording);
  }

  AudioStage current = stage.load();
  if (current == AudioStage::Ready || current == AudioStage::Unavailable) {
    return;
  }
  if (draining) {
    size_t count = drainRemaining < chunkSamples ? drainRemaining : chunkSamples;
    memset(chunk, 0, count * sizeof(int16_t));
    if (!writeChunk(count)) {
      return;
    }
    drainRemaining -= count;
    if (drainRemaining == 0) {
      finish(current == AudioStage::Chime ? "Ready to record" : "Playback complete");
    }
    return;
  }

  if (current == AudioStage::Analyzing) {
    size_t count = discarded < warmupSamples ? warmupSamples - discarded :
                   spectrumSamples - position;
    if (count > chunkSamples) {
      count = chunkSamples;
    }
    size_t bytes = count * sizeof(int16_t);
    if (microphone.readBytes(reinterpret_cast<char *>(chunk), bytes) != bytes) {
      fail("Microphone read failed");
      return;
    }
    if (discarded < warmupSamples) {
      discarded += count;
      return;
    }
    memcpy(spectrumInput + position, chunk, bytes);
    position += count;
    updateLevel(chunk, count);
    if (position == spectrumSamples) {
      SpectrumFrame frame;
      spectrum_analyze(spectrumInput, frame);
      publishSpectrum(frame);
      position = 0;
    }
    return;
  }

  if (current == AudioStage::Recording) {
    size_t count = discarded < warmupSamples ? warmupSamples - discarded :
                   recordingSamples - position;
    if (count > chunkSamples) {
      count = chunkSamples;
    }
    size_t bytes = count * sizeof(int16_t);
    if (microphone.readBytes(reinterpret_cast<char *>(chunk), bytes) != bytes) {
      fail("Microphone read failed");
      return;
    }
    if (discarded < warmupSamples) {
      discarded += count;
      return;
    }
    memcpy(recording + position, chunk, bytes);
    updateLevel(chunk, count);
    position += count;
    progress.store(position * 100 / recordingSamples);
    if (position == recordingSamples) {
      int peak = 0;
      for (size_t index = 0; index < recordingSamples; ++index) {
        int magnitude = abs(static_cast<int>(recording[index]));
        if (magnitude > peak) {
          peak = magnitude;
        }
      }
      recordingGain = peak > 0 ? fminf(4.0f, 20000.0f / peak) : 1.0f;
      startOutput(AudioStage::Playing);
    }
    return;
  }

  size_t total = current == AudioStage::Chime ? chimeSamples : recordingSamples;
  size_t count = total - position;
  if (count > chunkSamples) {
    count = chunkSamples;
  }
  float gain = volume.load() / 100.0f;
  const float notes[] = {523.25f, 659.25f, 783.99f};
  for (size_t index = 0; index < count; ++index) {
    size_t offset = position + index;
    if (current == AudioStage::Chime) {
      size_t noteSamples = chimeSamples / 3;
      size_t local = offset % noteSamples;
      float envelope = sinf(3.14159265f * local / noteSamples);
      chunk[index] = static_cast<int16_t>(6000.0f * gain * envelope *
        sinf(6.2831853f * notes[offset / noteSamples] * local / sampleRate));
    } else {
      chunk[index] = static_cast<int16_t>(recording[offset] * recordingGain * gain);
    }
  }
  if (!writeChunk(count)) {
    return;
  }
  position += count;
  progress.store(position * 100 / total);
  if (position == total) {
    draining = true;
    drainRemaining = sampleRate / 2;
  }
}