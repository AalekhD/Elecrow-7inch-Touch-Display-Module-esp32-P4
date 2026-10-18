#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <cstring>
#include "login_ui.h"
#include "lvgl_v8_port.h"
#include "audio_demo.h"

namespace {

constexpr uint32_t accountVersion = 1;
constexpr uint32_t hashIterations = 100000;
constexpr size_t saltLength = 16;
constexpr size_t hashLength = 32;
constexpr size_t usernameLimit = 24;
constexpr size_t passwordLimit = 64;

struct Account {
  uint32_t version;
  uint32_t iterations;
  char username[usernameLimit + 1];
  uint8_t salt[saltLength];
  uint8_t hash[hashLength];
};

enum class AccountState { Empty, Ready, Error };
enum class Request { None, Create, Login };

Preferences storage;
Account account = {};
AccountState accountState = AccountState::Error;
Request pendingRequest = Request::None;
char pendingUsername[usernameLimit + 1] = {};
char pendingPassword[passwordLimit + 1] = {};
bool busy = false;
bool setupMode = false;
unsigned int failedAttempts = 0;
uint32_t cooldownEnd = 0;
int lastCooldownSeconds = -1;

lv_obj_t *usernameField = nullptr;
lv_obj_t *passwordField = nullptr;
lv_obj_t *confirmField = nullptr;
lv_obj_t *keyboard = nullptr;
lv_obj_t *submitButton = nullptr;
lv_obj_t *visibilityButton = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *audioLabel = nullptr;
lv_obj_t *recordButton = nullptr;
lv_obj_t *stopButton = nullptr;
lv_obj_t *audioProgress = nullptr;
lv_obj_t *volumeLabel = nullptr;
lv_obj_t *meterBars[9] = {};
bool splashVisible = false;
bool dashboardVisible = false;
bool analyzerVisible = false;
lv_obj_t *spectrumBars[spectrumPeakCount] = {};
lv_obj_t *frequencyLabels[spectrumPeakCount] = {};
lv_obj_t *strengthLabels[spectrumPeakCount] = {};
lv_obj_t *signalLabel = nullptr;
uint32_t splashStarted = 0;
uint32_t lastVisualUpdate = 0;

lv_color_t ink() { return lv_color_hex(0x202B30); }
lv_color_t accent() { return lv_color_hex(0x087F75); }

lv_obj_t *text(lv_obj_t *parent, const char *value, int posX, int posY,
               int width, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, value);
  lv_obj_set_pos(label, posX, posY);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

lv_obj_t *button(lv_obj_t *parent, const char *value, int posX, int posY,
                 int width, lv_event_cb_t callback) {
  lv_obj_t *control = lv_btn_create(parent);
  lv_obj_set_pos(control, posX, posY);
  lv_obj_set_size(control, width, 54);
  lv_obj_set_style_radius(control, 6, 0);
  lv_obj_set_style_bg_color(control, accent(), 0);
  lv_obj_set_style_shadow_width(control, 0, 0);
  lv_obj_t *label = lv_label_create(control);
  lv_label_set_text(label, value);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, nullptr);
  return control;
}

lv_obj_t *newScreen(const char *state) {
  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0xF2F5F6), 0);
  lv_obj_set_style_text_font(screen, &lv_font_montserrat_20, 0);
  text(screen, LV_SYMBOL_HOME "  DEVICE ACCESS", 32, 22, 600,
       &lv_font_montserrat_20, ink());
  lv_obj_t *stateLabel = text(screen, state, 720, 24, 272,
                             &lv_font_montserrat_16, accent());
  lv_obj_set_style_text_align(stateLabel, LV_TEXT_ALIGN_RIGHT, 0);
  return screen;
}

void loadScreen(lv_obj_t *screen) {
  lv_obj_t *previous = lv_scr_act();
  lv_scr_load(screen);
  lv_obj_del_async(previous);
}

void setStatus(const char *message, bool error = true) {
  lv_label_set_text(statusLabel, message);
  lv_obj_set_style_text_color(statusLabel,
                            error ? lv_color_hex(0xB52D41) : accent(), 0);
}

bool validUsername(const char *username) {
  size_t length = strlen(username);
  if (length < 3 || length > usernameLimit) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    char character = username[index];
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') ||
          character == '_' || character == '-' || character == '.')) {
      return false;
    }
  }
  return true;
}

int cooldownSeconds() {
  if (cooldownEnd == 0) {
    return 0;
  }
  int32_t remaining = static_cast<int32_t>(cooldownEnd - millis());
  return remaining > 0 ? (remaining + 999) / 1000 : 0;
}

void setBusy(bool value) {
  busy = value;
  lv_obj_t *controls[] = {usernameField, passwordField, confirmField,
                          keyboard, submitButton, visibilityButton};
  for (lv_obj_t *control : controls) {
    if (control == nullptr) {
      continue;
    }
    if (value) {
      lv_obj_add_state(control, LV_STATE_DISABLED);
    } else {
      lv_obj_clear_state(control, LV_STATE_DISABLED);
    }
  }
}

void showForm();
void showWelcome();

void logoutClicked(lv_event_t *) {
  audio_demo_stop();
  showForm();
}

void clearAudioWidgets() {
  audioLabel = recordButton = stopButton = audioProgress = volumeLabel = nullptr;
  for (lv_obj_t *&bar : meterBars) {
    bar = nullptr;
  }
  dashboardVisible = false;
  splashVisible = false;
  analyzerVisible = false;
  signalLabel = nullptr;
  for (size_t index = 0; index < spectrumPeakCount; ++index) {
    spectrumBars[index] = frequencyLabels[index] = strengthLabels[index] = nullptr;
  }
}

void createMeter(lv_obj_t *screen, int posX, int posY, int width, int height) {
  int spacing = width / 9;
  for (size_t index = 0; index < 9; ++index) {
    lv_obj_t *bar = lv_bar_create(screen);
    lv_obj_set_pos(bar, posX + index * spacing, posY);
    lv_obj_set_size(bar, spacing - 12, height);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xDFE7E8), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, index % 3 == 0 ? lv_color_hex(0xDBA329) : accent(),
                              LV_PART_INDICATOR);
    meterBars[index] = bar;
  }
}

void splashFinished(lv_event_t *) {
  audio_demo_stop();
  showForm();
}

void showSplash() {
  clearAudioWidgets();
  splashVisible = true;
  splashStarted = millis();
  lv_obj_t *screen = newScreen("WELCOME");
  lv_obj_t *title = text(screen, "Welcome", 64, 116, 896,
                         &lv_font_montserrat_42, ink());
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *note = text(screen, "A little sound. A new beginning.", 64, 182, 896,
                        &lv_font_montserrat_22, ink());
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  createMeter(screen, 305, 270, 414, 100);
  audioLabel = text(screen, "ESP32-P4  /  7-inch touch display", 64, 416, 896,
                     &lv_font_montserrat_18, accent());
  lv_obj_set_style_text_align(audioLabel, LV_TEXT_ALIGN_CENTER, 0);
  button(screen, "Continue  " LV_SYMBOL_RIGHT, 372, 496, 280, splashFinished);
  loadScreen(screen);
  audio_demo_start(true);
}

void recordClicked(lv_event_t *) {
  audio_demo_start();
}

void stopClicked(lv_event_t *) {
  audio_demo_stop();
}

void volumeChanged(lv_event_t *event) {
  int percent = lv_slider_get_value(lv_event_get_target(event));
  audio_demo_set_volume(percent);
  lv_label_set_text_fmt(volumeLabel, "Volume  %d%%", percent);
}

void feature(lv_obj_t *screen, const char *name, const char *value, int posY) {
  text(screen, name, 40, posY, 126, &lv_font_montserrat_18, accent());
  text(screen, value, 170, posY, 360, &lv_font_montserrat_18, ink());
}

void backToApps(lv_event_t *) {
  audio_demo_stop();
  showWelcome();
}

void appNavigation(lv_obj_t *screen) {
  button(screen, LV_SYMBOL_HOME "  Apps", 590, 82, 190, backToApps);
  button(screen, LV_SYMBOL_LEFT "  Log out", 800, 82, 184, logoutClicked);
}

void showRecordApp(lv_event_t *) {
  clearAudioWidgets();
  dashboardVisible = true;
  lv_obj_t *screen = newScreen("RECORD & PLAY");
  text(screen, "Record & Play", 40, 88, 520, &lv_font_montserrat_32, ink());
  appNavigation(screen);
  recordButton = button(screen, LV_SYMBOL_AUDIO "  Listen & repeat", 40, 184, 350, recordClicked);
  stopButton = button(screen, LV_SYMBOL_STOP "  Stop", 410, 184, 140, stopClicked);
  audioLabel = text(screen, "Ready to record", 40, 270, 920,
                     &lv_font_montserrat_22, ink());
  createMeter(screen, 48, 334, 920, 96);
  audioProgress = lv_bar_create(screen);
  lv_obj_set_pos(audioProgress, 48, 458);
  lv_obj_set_size(audioProgress, 920, 8);
  lv_obj_set_style_bg_color(audioProgress, accent(), LV_PART_INDICATOR);
  volumeLabel = text(screen, "Volume  35%", 40, 520, 480,
                      &lv_font_montserrat_20, ink());
  lv_obj_t *slider = lv_slider_create(screen);
  lv_obj_set_pos(slider, 602, 534);
  lv_obj_set_size(slider, 366, 14);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, 35, LV_ANIM_OFF);
  audio_demo_set_volume(35);
  lv_obj_add_event_cb(slider, volumeChanged, LV_EVENT_VALUE_CHANGED, nullptr);
  loadScreen(screen);
}

void startAnalysisClicked(lv_event_t *) {
  audio_demo_start_analysis();
}

void showAnalyzerApp(lv_event_t *) {
  clearAudioWidgets();
  analyzerVisible = true;
  lv_obj_t *screen = newScreen("LIVE FREQUENCIES");
  text(screen, "Frequency Analyzer", 40, 88, 520, &lv_font_montserrat_28, ink());
  appNavigation(screen);
  recordButton = button(screen, LV_SYMBOL_PLAY "  Start", 40, 150, 220, startAnalysisClicked);
  stopButton = button(screen, LV_SYMBOL_STOP "  Stop", 280, 150, 150, stopClicked);
  audioLabel = text(screen, "Ready", 456, 166, 520, &lv_font_montserrat_18, ink());
  const char *scale[] = {"0", "-20", "-40", "-60", "-80"};
  text(screen, "dBFS", 40, 230, 90, &lv_font_montserrat_16, ink());
  for (size_t index = 0; index < 5; ++index) {
    text(screen, scale[index], 60, 260 + index * 50, 70,
         &lv_font_montserrat_16, lv_color_hex(0x526369));
  }
  const uint32_t colors[] = {0x087F75, 0xCF9330, 0x347EA5, 0xB9586D, 0x5D8848};
  for (size_t index = 0; index < spectrumPeakCount; ++index) {
    int column = 130 + index * 168;
    strengthLabels[index] = text(screen, "-- dBFS", column, 232, 150,
                                 &lv_font_montserrat_18, ink());
    lv_obj_set_style_text_align(strengthLabels[index], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *bar = lv_bar_create(screen);
    lv_obj_set_size(bar, 76, 200);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, column + 37, 268);
    lv_bar_set_range(bar, -80, 0);
    lv_bar_set_value(bar, -80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xDFE7E8), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors[index]), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    spectrumBars[index] = bar;
    frequencyLabels[index] = text(screen, "-- Hz", column, 486, 150,
                                  &lv_font_montserrat_22, ink());
    lv_obj_set_style_text_align(frequencyLabels[index], LV_TEXT_ALIGN_CENTER, 0);
  }
  signalLabel = text(screen, "60 Hz - 7 kHz  |  Signal -- dBFS", 40, 554, 940,
                      &lv_font_montserrat_18, ink());
  loadScreen(screen);
}

void showWelcome() {
  clearAudioWidgets();
  lv_obj_t *screen = newScreen("APPLICATIONS");
  lv_obj_t *greeting = text(screen, "", 40, 84, 930, &lv_font_montserrat_28, ink());
  lv_label_set_text_fmt(greeting, "Welcome, %s", account.username);
  text(screen, "Board overview", 40, 150, 490, &lv_font_montserrat_22, ink());
  feature(screen, "Processor", "ESP32-P4 / dual-core RISC-V", 200);
  char memory[80];
  snprintf(memory, sizeof(memory), "%lu MB PSRAM / %lu MB flash",
           static_cast<unsigned long>(ESP.getPsramSize() / (1024 * 1024)),
           static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)));
  feature(screen, "Memory", memory, 244);
  feature(screen, "Display", "7-inch IPS / 1024 x 600 / DSI", 288);
  feature(screen, "Touch", "GT911 capacitive touch", 332);
  feature(screen, "Audio", "PDM microphone / I2S speaker", 376);
  feature(screen, "Expansion", "microSD, camera, UART, I2C, GPIO", 420);
  text(screen, "ESP32-C6 radio: not configured", 40, 476, 490,
       &lv_font_montserrat_16, lv_color_hex(0x526369));
  button(screen, LV_SYMBOL_LEFT "  Log out", 40, 530, 240, logoutClicked);

    text(screen, "Applications", 590, 150, 390,
       &lv_font_montserrat_22, ink());
    lv_obj_t *recordApp = button(screen, LV_SYMBOL_AUDIO "  Record & Play", 590, 210, 390, showRecordApp);
    lv_obj_set_height(recordApp, 108);
    lv_obj_t *analyzerApp = button(screen, LV_SYMBOL_LIST "  Frequency Analyzer", 590, 350, 390, showAnalyzerApp);
    lv_obj_set_height(analyzerApp, 108);
  loadScreen(screen);
  usernameField = passwordField = confirmField = keyboard = nullptr;
  submitButton = visibilityButton = statusLabel = nullptr;
}

void updateSpectrumVisuals(const AudioStatus &status) {
  bool running = status.stage == AudioStage::Analyzing;
  SpectrumFrame frame = running ? audio_demo_spectrum() : SpectrumFrame{};
  for (size_t index = 0; index < spectrumPeakCount; ++index) {
    bool hasPeak = index < frame.count;
    int value = hasPeak ? static_cast<int>(frame.peaks[index].dbfs) : -80;
    lv_bar_set_value(spectrumBars[index], value < -80 ? -80 : value, LV_ANIM_OFF);
    char label[40];
    if (hasPeak) {
      snprintf(label, sizeof(label), "%.0f Hz", frame.peaks[index].frequencyHz);
      lv_label_set_text(frequencyLabels[index], label);
      snprintf(label, sizeof(label), "%.1f dBFS", frame.peaks[index].dbfs);
      lv_label_set_text(strengthLabels[index], label);
    } else {
      lv_label_set_text(frequencyLabels[index], "-- Hz");
      lv_label_set_text(strengthLabels[index], "-- dBFS");
    }
  }
  char signal[100];
  if (running) {
    snprintf(signal, sizeof(signal), "60 Hz - 7 kHz  |  Signal %.1f dBFS%s",
             frame.signalDbfs, frame.clipped ? "  |  CLIPPING" : "");
    lv_label_set_text_fmt(audioLabel, "Live  /  %u peaks", static_cast<unsigned int>(frame.count));
  } else {
    snprintf(signal, sizeof(signal), "60 Hz - 7 kHz  |  Signal -- dBFS");
    lv_label_set_text(audioLabel, status.stage == AudioStage::Unavailable ? status.message : "Stopped");
  }
  lv_label_set_text(signalLabel, signal);
  lv_obj_set_style_text_color(signalLabel, frame.clipped ? lv_color_hex(0xB52D41) : ink(), 0);
  if (status.stage == AudioStage::Ready) {
    lv_obj_clear_state(recordButton, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(recordButton, LV_STATE_DISABLED);
  }
  if (running) {
    lv_obj_clear_state(stopButton, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(stopButton, LV_STATE_DISABLED);
  }
}

void updateAudioVisuals() {
  if (!splashVisible && !dashboardVisible && !analyzerVisible) {
    return;
  }
  uint32_t now = millis();
  if (splashVisible && now - splashStarted >= 3200) {
    splashFinished(nullptr);
    return;
  }
  if (now - lastVisualUpdate < 80) {
    return;
  }
  lastVisualUpdate = now;
  AudioStatus status = audio_demo_status();
  if (analyzerVisible) {
    updateSpectrumVisuals(status);
    return;
  }
  const int weights[] = {40, 66, 82, 94, 100, 88, 76, 60, 36};
  for (size_t index = 0; index < 9; ++index) {
    int intensity = status.level * 4;
    if (intensity > 100) {
      intensity = 100;
    }
    if (splashVisible) {
      intensity = 20 + (now / 80 + index * 11) % 60;
    }
    lv_bar_set_value(meterBars[index], intensity * weights[index] / 100, LV_ANIM_ON);
  }
  if (!dashboardVisible) {
    return;
  }
  lv_bar_set_value(audioProgress, status.progress, LV_ANIM_OFF);
  char caption[100];
  if (status.stage == AudioStage::Recording) {
    snprintf(caption, sizeof(caption), "Listening... %u / 4 seconds", status.progress * 4 / 100);
  } else {
    snprintf(caption, sizeof(caption), "%s", status.message);
  }
  lv_label_set_text(audioLabel, caption);
  bool active = status.stage == AudioStage::Recording || status.stage == AudioStage::Playing ||
                status.stage == AudioStage::Chime;
  if (active || status.stage == AudioStage::Unavailable) {
    lv_obj_add_state(recordButton, LV_STATE_DISABLED);
  } else {
    lv_obj_clear_state(recordButton, LV_STATE_DISABLED);
  }
  if (active) {
    lv_obj_clear_state(stopButton, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(stopButton, LV_STATE_DISABLED);
  }
}

void submitClicked(lv_event_t *) {
  if (busy || pendingRequest != Request::None || cooldownSeconds() > 0) {
    return;
  }
  const char *username = lv_textarea_get_text(usernameField);
  const char *password = lv_textarea_get_text(passwordField);
  if (!validUsername(username)) {
    setStatus("Username: 3-24 letters, numbers, dots, dashes or underscores.");
    return;
  }
  size_t passwordLength = strlen(password);
  if (passwordLength < 8 || passwordLength > passwordLimit) {
    setStatus("Password must contain 8-64 characters.");
    return;
  }
  if (setupMode && strcmp(password, lv_textarea_get_text(confirmField)) != 0) {
    setStatus("Passwords do not match.");
    return;
  }
  strlcpy(pendingUsername, username, sizeof(pendingUsername));
  strlcpy(pendingPassword, password, sizeof(pendingPassword));
  lv_textarea_set_password_mode(passwordField, true);
  lv_textarea_set_text(passwordField, "");
  if (confirmField != nullptr) {
    lv_textarea_set_password_mode(confirmField, true);
    lv_textarea_set_text(confirmField, "");
  }
  lv_obj_clear_state(visibilityButton, LV_STATE_CHECKED);
  setBusy(true);
  setStatus(setupMode ? "Saving account..." : "Signing in...", false);
  pendingRequest = setupMode ? Request::Create : Request::Login;
}

void fieldFocused(lv_event_t *event) {
  if (busy) {
    return;
  }
  lv_keyboard_set_textarea(keyboard, lv_event_get_target(event));
  lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void keyboardReady(lv_event_t *) {
  lv_obj_t *field = lv_keyboard_get_textarea(keyboard);
  lv_obj_t *next = field == usernameField ? passwordField :
                   (field == passwordField ? confirmField : nullptr);
  if (next != nullptr) {
    lv_keyboard_set_textarea(keyboard, next);
    lv_obj_add_state(next, LV_STATE_FOCUSED);
  } else {
    submitClicked(nullptr);
  }
}

void keyboardCancelled(lv_event_t *) {
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void visibilityClicked(lv_event_t *) {
  bool masked = !lv_obj_has_state(visibilityButton, LV_STATE_CHECKED);
  lv_textarea_set_password_mode(passwordField, masked);
  if (confirmField != nullptr) {
    lv_textarea_set_password_mode(confirmField, masked);
  }
}

lv_obj_t *field(lv_obj_t *screen, const char *name, const char *placeholder,
                int posY, bool password) {
  text(screen, name, 440, posY, 460, &lv_font_montserrat_18, ink());
  lv_obj_t *input = lv_textarea_create(screen);
  lv_obj_set_pos(input, 440, posY + 27);
  lv_obj_set_size(input, 480, 50);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, password ? passwordLimit : usernameLimit);
  lv_textarea_set_placeholder_text(input, placeholder);
  lv_textarea_set_password_mode(input, password);
  lv_textarea_set_password_show_time(input, 0);
  lv_textarea_set_accepted_chars(input,
    password ? "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~" :
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-");
  lv_obj_set_style_radius(input, 6, 0);
  lv_obj_set_style_border_color(input, accent(), LV_STATE_FOCUSED);
  lv_obj_add_event_cb(input, fieldFocused, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(input, fieldFocused, LV_EVENT_CLICKED, nullptr);
  return input;
}

void showForm() {
  clearAudioWidgets();
  setupMode = accountState == AccountState::Empty;
  lv_obj_t *screen = newScreen(setupMode ? "FIRST START" : "SIGNED OUT");
  text(screen, setupMode ? "Create your\nowner account" : "Sign in", 48, 106,
       350, &lv_font_montserrat_32, ink());
  if (!setupMode) {
    text(screen, "Owner account", 48, 176, 350,
         &lv_font_montserrat_20, accent());
  }
  usernameField = field(screen, "Username", "3-24 characters", 84, false);
  passwordField = field(screen, "Password", "8-64 characters", 170, true);
  confirmField = setupMode ? field(screen, "Confirm password", "Repeat password", 256, true) : nullptr;
  visibilityButton = button(screen, LV_SYMBOL_EYE_OPEN, 932, 197, 54, visibilityClicked);
  lv_obj_add_flag(visibilityButton, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_set_style_bg_color(visibilityButton, lv_color_hex(0x465B60), LV_STATE_CHECKED);
  submitButton = button(screen, setupMode ? "Create account  " LV_SYMBOL_RIGHT :
                                           "Sign in  " LV_SYMBOL_RIGHT,
                        48, 262, 330, submitClicked);
  statusLabel = text(screen, "", 48, 330, 340,
                     &lv_font_montserrat_16, ink());
  keyboard = lv_keyboard_create(screen);
  lv_obj_set_size(keyboard, 1024, 210);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_20, 0);
  lv_keyboard_set_textarea(keyboard, usernameField);
  lv_obj_add_event_cb(keyboard, keyboardReady, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(keyboard, keyboardCancelled, LV_EVENT_CANCEL, nullptr);
  loadScreen(screen);
}

bool derivePassword(const char *password, const Account &record, uint8_t *output) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
    reinterpret_cast<const unsigned char *>(password), strlen(password),
    record.salt, sizeof(record.salt), record.iterations,
    hashLength, output) == 0;
}

bool sameHash(const uint8_t *left, const uint8_t *right) {
  volatile uint8_t difference = 0;
  for (size_t index = 0; index < hashLength; ++index) {
    difference = difference | (left[index] ^ right[index]);
  }
  return difference == 0;
}

} // namespace

bool login_ui_init() {
  if (!storage.begin("device-login", false)) {
    return false;
  }
  if (!storage.isKey("owner")) {
    accountState = AccountState::Empty;
  } else if (storage.getBytesLength("owner") == sizeof(account) &&
             storage.getBytes("owner", &account, sizeof(account)) == sizeof(account) &&
             account.version == accountVersion && account.iterations == hashIterations &&
             memchr(account.username, '\0', sizeof(account.username)) != nullptr &&
             validUsername(account.username)) {
    accountState = AccountState::Ready;
  } else {
    Serial.println("Saved account data is invalid; refusing to overwrite it.");
    return false;
  }
  if (!lvgl_port_lock(-1)) {
    return false;
  }
  showSplash();
  lvgl_port_unlock();
  return true;
}

void login_ui_process() {
  if (!lvgl_port_lock(-1)) {
    return;
  }
  updateAudioVisuals();
  if (cooldownEnd != 0 && statusLabel != nullptr && !busy) {
    int seconds = cooldownSeconds();
    if (seconds != lastCooldownSeconds) {
      lastCooldownSeconds = seconds;
      if (seconds > 0) {
        char message[80];
        snprintf(message, sizeof(message), "Too many attempts. Try again in %d seconds.", seconds);
        setStatus(message);
      } else {
        cooldownEnd = 0;
        failedAttempts = 0;
        lv_obj_clear_state(submitButton, LV_STATE_DISABLED);
        setStatus("You can sign in again.", false);
      }
    }
  }
  Request request = pendingRequest;
  if (request == Request::None) {
    lvgl_port_unlock();
    return;
  }
  pendingRequest = Request::None;
  char username[usernameLimit + 1];
  char password[passwordLimit + 1];
  strlcpy(username, pendingUsername, sizeof(username));
  strlcpy(password, pendingPassword, sizeof(password));
  mbedtls_platform_zeroize(pendingPassword, sizeof(pendingPassword));
  lvgl_port_unlock();

  Account candidate = account;
  if (request == Request::Create) {
    candidate = {};
    candidate.version = accountVersion;
    candidate.iterations = hashIterations;
    strlcpy(candidate.username, username, sizeof(candidate.username));
    esp_fill_random(candidate.salt, sizeof(candidate.salt));
  }
  uint8_t derived[hashLength] = {};
  bool derivedOk = derivePassword(password, candidate, derived);
  mbedtls_platform_zeroize(password, sizeof(password));
  bool success = false;
  bool storageError = false;
  if (derivedOk && request == Request::Create) {
    memcpy(candidate.hash, derived, sizeof(derived));
    success = storage.putBytes("owner", &candidate, sizeof(candidate)) == sizeof(candidate);
    storageError = !success;
    if (success) {
      account = candidate;
      accountState = AccountState::Ready;
    }
  } else if (derivedOk) {
    bool hashMatches = sameHash(derived, account.hash);
    success = hashMatches && strcmp(username, account.username) == 0;
  }
  mbedtls_platform_zeroize(derived, sizeof(derived));
  mbedtls_platform_zeroize(&candidate, sizeof(candidate));

  if (!lvgl_port_lock(-1)) {
    return;
  }
  setBusy(false);
  if (success) {
    failedAttempts = 0;
    cooldownEnd = 0;
    showWelcome();
    Serial.println("Local login successful.");
  } else if (!derivedOk || storageError) {
    setStatus(storageError ? "Account could not be saved. Please try again." :
                             "Password processing failed. Please try again.");
  } else {
    ++failedAttempts;
    setStatus("Incorrect username or password.");
    if (failedAttempts >= 5) {
      cooldownEnd = millis() + 30000;
      lastCooldownSeconds = -1;
      lv_obj_add_state(submitButton, LV_STATE_DISABLED);
    }
  }
  lvgl_port_unlock();
}