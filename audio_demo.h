#pragma once

#include <stdint.h>
#include "spectrum_analyzer.h"

enum class AudioStage { Unavailable, Ready, Chime, Recording, Playing, Analyzing };

struct AudioStatus {
  AudioStage stage;
  unsigned int progress;
  unsigned int level;
  const char *message;
};

bool audio_demo_begin();
bool audio_demo_start(bool welcomeChime = false);
bool audio_demo_start_analysis();
void audio_demo_stop();
void audio_demo_set_volume(unsigned int percent);
void audio_demo_process();
AudioStatus audio_demo_status();
SpectrumFrame audio_demo_spectrum();