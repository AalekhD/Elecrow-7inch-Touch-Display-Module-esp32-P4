#include <Arduino.h>
#include <esp_ldo_regulator.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "login_ui.h"
#include "audio_demo.h"

static esp_ldo_channel_handle_t displayPower = nullptr;
static esp_ldo_channel_handle_t touchPower = nullptr;
static esp_panel::board::Board board;

static void haltWithError(const char *message) {
  Serial.println(message);
  while (true) {
    delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting display...");

  esp_ldo_channel_config_t powerConfig = {};
  powerConfig.chan_id = 3;
  powerConfig.voltage_mv = 2500;
  if (esp_ldo_acquire_channel(&powerConfig, &displayPower) != ESP_OK) {
    haltWithError("Failed to enable MIPI display power.");
  }
  powerConfig.chan_id = 4;
  powerConfig.voltage_mv = 3300;
  if (esp_ldo_acquire_channel(&powerConfig, &touchPower) != ESP_OK) {
    haltWithError("Failed to enable touch power.");
  }

  if (!board.init()) {
    haltWithError("Display configuration failed.");
  }
  auto lcd = board.getLCD();
  if (lcd == nullptr) {
    haltWithError("LCD driver is unavailable.");
  }
#if LVGL_PORT_AVOID_TEARING_MODE
  lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#endif
  if (!board.begin()) {
    haltWithError("Display initialization failed.");
  }
  auto touch = board.getTouch();
  if (touch == nullptr) {
    haltWithError("Touch driver is unavailable.");
  }
  if (!lvgl_port_init(lcd, touch)) {
    haltWithError("LVGL initialization failed.");
  }
  if (!audio_demo_begin()) {
    Serial.println("Audio unavailable; login will remain usable.");
  }
  if (!login_ui_init()) {
    haltWithError("Login initialization failed. Check saved account storage.");
  }
  Serial.println("Touchscreen login ready.");
}

void loop() {
  audio_demo_process();
  login_ui_process();
  delay(1);
}
