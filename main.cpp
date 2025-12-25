/*
 * ArduinoGotchi - A real Tamagotchi emulator for M5StickCPlus2
 *
 * Copyright (C) 2022 Gary Kwok - Arduino Uno Implementation
 * Copyright (C) 2022 Marcel Ochsendorf - ESP32 Platform Support
 * Copyright (C) 2024 M5StickCPlus2 Port
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifdef M5STICKC_PLUS2
#include <M5StickCPlus2.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#else
#include <U8g2lib.h>
#include <Wire.h>
#endif

#include "tamalib.h"
#include "hw.h"
#include "bitmaps.h"

#if defined(ENABLE_AUTO_SAVE_STATUS) || defined(ENABLE_LOAD_STATE_FROM_EEPROM)
#include "savestate.h"
#endif

/***** M5StickCPlus2 Configuration *****/
#ifdef M5STICKC_PLUS2
#define DISPLAY_SCALE 2  // Scale factor for 128x64 -> 240x135
#define DISPLAY_OFFSET_X 8  // Center horizontally
#define DISPLAY_OFFSET_Y 8  // Center vertically

// Button mapping for M5StickC Plus2
#define BTN_LEFT_PIN   37  // BtnA (side button)
#define BTN_MIDDLE_PIN 39  // BtnB (front button)  
#define BTN_RIGHT_PIN  35  // BtnPWR (power button - use carefully)

// Enhanced M5 Features
static int current_menu_page = 0;
static int max_menu_pages = 2;  // Original + enhanced features
static bool effects_enabled = true;
static unsigned long last_effect_time = 0;

// Sound toggle system
static bool sound_enabled = true;
static unsigned long last_btn_b_press = 0;
static bool waiting_for_second_tap = false;

// TamaPortal toggle system (double-tap PWR)
static unsigned long last_btn_pwr_press = 0;
static bool waiting_for_portal_second_tap = false;

// TamaPortal system
static bool tamaportal_active = false;
static unsigned long last_portal_scan = 0;
static const unsigned long PORTAL_SCAN_INTERVAL = 30000; // 30 seconds
static String received_message = "";
static unsigned long message_display_time = 0;
static const unsigned long MESSAGE_DISPLAY_DURATION = 5000; // 5 seconds

// Matrix rain effect variables
static unsigned long last_matrix_update = 0;
static int matrix_drops[30]; // 30 columns of rain
static int matrix_lengths[30]; // length of each rain trail
static bool matrix_initialized = false;

// TamaPortal web server globals (forward declarations)
static WebServer* tamaPortalServer = nullptr;
static DNSServer* tamaDnsServer = nullptr;

// 90s Retro Colors
#define NEON_CYAN     0x07FF
#define NEON_MAGENTA  0xF81F  
#define NEON_GREEN    0x07E0
#define NEON_YELLOW   0xFFE0
#define NEON_ORANGE   0xFD20
#define NEON_PURPLE   0x8010

#else
/***** Set display orientation, U8G2_MIRROR_VERTICAL is not supported *****/
#define U8G2_LAYOUT_NORMAL

#ifdef U8G2_LAYOUT_NORMAL
U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_R0);
#endif

#ifdef U8G2_LAYOUT_ROTATE_180
U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_R2);
#endif

#ifdef U8G2_LAYOUT_MIRROR
U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_MIRROR);
#endif

#define PIN_BTN_L 18
#define PIN_BTN_M 19
#define PIN_BTN_R 23
#define PIN_BUZZER 15
#define BUZZER_CHANNEL 0
#define TONE_CHANNEL 15
#endif

#if defined(ESP32) && !defined(M5STICKC_PLUS2)
void esp32_noTone(uint8_t pin, uint8_t channel)
{
  ledcDetachPin(pin);
  ledcWrite(channel, 0);
}

void esp32_tone(uint8_t pin, unsigned int frequency, unsigned long duration, uint8_t channel)
{
  if (!ledcRead(channel))
  {
    ledcAttachPin(pin, channel);
  }
  ledcWriteTone(channel, frequency);
}
#endif

void displayTama();
void enter_deepsleep(int _ms);
void pixelatedArtExplosion();
void draw90sBorder();
void showSoundToggleFeedback(bool sound_on);

// Matrix rain effect
void drawMatrixRain();

// TamaPortal functions
void initTamaPortal();
void handleTamaPortal();
void scanAndAttackPortals();
void sendFriendlyMessage(String ssid);
void createTamaPortalHotspot();
void handleTamaPortalWeb();

/**** TamaLib Specific Variables ****/
static uint16_t current_freq = 0;
static bool_t matrix_buffer[LCD_HEIGHT][LCD_WIDTH / 8] = {{0}};
static bool_t icon_buffer[ICON_NUM] = {0};
static cpu_state_t cpuState;
static unsigned long lastSaveTimestamp = 0;
static long last_interaction = 0;
/************************************/

static void hal_halt(void)
{
  // Serial.println("Halt!");
}

static void hal_log(log_level_t level, char *buff, ...)
{
  Serial.println(buff);
}

static timestamp_t hal_get_timestamp(void)
{
  return millis() * (1000 / SPEED_DIVIDER);
}

static void hal_sleep_until(timestamp_t ts)
{
  int32_t remaining = (int32_t)(ts - hal_get_timestamp());
  if (remaining > 0)
  {
#ifdef ENABLE_DEEPSLEEP
    enter_deepsleep(remaining);
#endif
  }
}

static void hal_update_screen(void)
{
  displayTama();
}

static void hal_set_lcd_matrix(u8_t x, u8_t y, bool_t val)
{
  uint8_t mask;
  if (val)
  {
    mask = 0b10000000 >> (x % 8);
    matrix_buffer[y][x / 8] = matrix_buffer[y][x / 8] | mask;
  }
  else
  {
    mask = 0b01111111;
    for (byte i = 0; i < (x % 8); i++)
    {
      mask = (mask >> 1) | 0b10000000;
    }
    matrix_buffer[y][x / 8] = matrix_buffer[y][x / 8] & mask;
  }
}

static void hal_set_lcd_icon(u8_t icon, bool_t val)
{
  icon_buffer[icon] = val;
}

static void hal_set_frequency(u32_t freq)
{
  current_freq = freq;
}

static void hal_play_frequency(bool_t en)
{
#ifdef ENABLE_TAMA_SOUND
  // Check if sound is enabled by user toggle
  if (!sound_enabled) {
    return; // Silently ignore sound requests when sound is disabled
  }
  
  if (en)
  {
#ifdef M5STICKC_PLUS2
    M5.Speaker.tone(current_freq, 500);
#elif defined(ESP32)
    esp32_tone(PIN_BUZZER, current_freq, 500, BUZZER_CHANNEL);
#else
    tone(PIN_BUZZER, current_freq);
#endif
  }
  else
  {
#ifdef M5STICKC_PLUS2
    M5.Speaker.stop();
#elif defined(ESP32)
    esp32_noTone(PIN_BUZZER, BUZZER_CHANNEL);
#else
    noTone(PIN_BUZZER);
#ifdef ENABLE_TAMA_SOUND_ACTIVE_LOW
    digitalWrite(PIN_BUZZER, HIGH);
#endif
#endif
  }
#endif
}

static bool_t button4state = 0;

static int hal_handler(void)
{
#ifdef ENABLE_SERIAL_DEBUG_INPUT
  if (Serial.available() > 0)
  {
    int incomingByte = Serial.read();
    Serial.println(incomingByte, DEC);
    if (incomingByte == 49)
    {
      hw_set_button(BTN_LEFT, BTN_STATE_PRESSED);
    }
    else if (incomingByte == 50)
    {
      hw_set_button(BTN_LEFT, BTN_STATE_RELEASED);
    }
    else if (incomingByte == 51)
    {
      hw_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
    }
    else if (incomingByte == 52)
    {
      hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
    }
    else if (incomingByte == 53)
    {
      hw_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
    }
    else if (incomingByte == 54)
    {
      hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
    }
  }
#endif

#ifdef M5STICKC_PLUS2
  M5.update();
  
  static bool btn_a_was_pressed = false;
  static bool btn_b_was_pressed = false;
  static bool btn_pwr_was_pressed = false;
  
  // Enhanced button handling with menu scrolling and special features
  bool btn_a_pressed = M5.BtnA.isPressed();
  bool btn_b_pressed = M5.BtnB.isPressed();
  bool btn_pwr_pressed = M5.BtnPWR.isPressed();
  
  // AUTO-BYPASS TIME SETUP: Send automatic button sequence to get past time setup
  static bool time_bypass_complete = false;
  static unsigned long time_bypass_start = 0;
  
  if (!time_bypass_complete) {
    if (time_bypass_start == 0) {
      time_bypass_start = millis();
      Serial.println("Starting automatic time setup bypass...");
    }
    
    unsigned long elapsed = millis() - time_bypass_start;
    
    // Send automatic button sequence to complete time setup
    if (elapsed < 1000) {
      // First second: press and release A a few times (set hours)
      if ((elapsed % 200) < 100) {
        hw_set_button(BTN_LEFT, BTN_STATE_PRESSED);
      } else {
        hw_set_button(BTN_LEFT, BTN_STATE_RELEASED);
      }
    } else if (elapsed < 2000) {
      // Second second: press and release B a few times (set minutes)  
      if ((elapsed % 200) < 100) {
        hw_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
      } else {
        hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
      }
    } else if (elapsed < 3000) {
      // Third second: press and hold right button (confirm time)
      hw_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
    } else if (elapsed < 3500) {
      // Release right button
      hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
    } else {
      // Time bypass complete, enable normal button handling
      time_bypass_complete = true;
      Serial.println("Time setup bypass complete! Normal operation starting...");
    }
    return 0; // Skip normal button processing during bypass
  }
  
  // Handle menu scrolling (hold A + B together)
  if (btn_a_pressed && btn_pwr_pressed && !btn_a_was_pressed) {
    // Toggle between menu pages
    current_menu_page = (current_menu_page + 1) % max_menu_pages;
    delay(200); // Debounce
    Serial.println(String("Switched to menu page: ") + String(current_menu_page));
  }
  
  // Handle effects toggle (hold B for 2 seconds)
  static unsigned long btn_b_hold_start = 0;
  if (btn_b_pressed && !btn_b_was_pressed) {
    btn_b_hold_start = millis();
  }
  if (btn_b_pressed && (millis() - btn_b_hold_start > 2000)) {
    effects_enabled = !effects_enabled;
    btn_b_hold_start = millis() + 5000; // Prevent rapid toggling
    Serial.println(String("Effects ") + (effects_enabled ? "enabled" : "disabled"));
  }
  
  // Handle sound toggle (double-tap B)
  if (btn_b_pressed && !btn_b_was_pressed) {
    unsigned long now = millis();
    
    if (waiting_for_second_tap && (now - last_btn_b_press < 500)) {
      // Double tap detected!
      sound_enabled = !sound_enabled;
      Serial.println(String("Sound ") + (sound_enabled ? "ON" : "OFF"));
      showSoundToggleFeedback(sound_enabled);
      waiting_for_second_tap = false;
      btn_b_hold_start = 0; // Reset hold timer to prevent effects toggle
    } else {
      // First tap or timeout
      last_btn_b_press = now;
      waiting_for_second_tap = true;
    }
  }
  
  // Timeout the double-tap window
  if (waiting_for_second_tap && (millis() - last_btn_b_press > 500)) {
    waiting_for_second_tap = false;
  }
  
  // Handle TamaPortal activation (double-tap PWR button) - DISABLED for performance
  /*
  if (btn_pwr_pressed && !btn_pwr_was_pressed && !btn_a_pressed) { // PWR pressed, not part of A+PWR menu scroll
    unsigned long now = millis();
    
    if (waiting_for_portal_second_tap && (now - last_btn_pwr_press < 500)) {
      // Double tap detected!
      Serial.println("TamaPortal toggle!");
      initTamaPortal();
      waiting_for_portal_second_tap = false;
    } else {
      // First tap or timeout
      last_btn_pwr_press = now;
      waiting_for_portal_second_tap = true;
    }
  }
  
  // Timeout the double-tap window for PWR button
  if (waiting_for_portal_second_tap && (millis() - last_btn_pwr_press > 500)) {
    waiting_for_portal_second_tap = false;
  }
  */
  
  // Handle ART box activation (position 5 in original layout)
  // This is a bit tricky - we need to detect when the user selects the "Art" box
  // For now, let's trigger it with a special combination: A + B + PWR
  if (btn_a_pressed && btn_b_pressed && btn_pwr_pressed && 
      !btn_a_was_pressed && !btn_b_was_pressed && !btn_pwr_was_pressed) {
    Serial.println("ART EXPLOSION!");
    pixelatedArtExplosion();
    delay(500); // Prevent immediate retrigger
  }
  
  // Regular Tamagotchi button mapping (only if not doing special functions)
  if (!btn_a_pressed || !btn_pwr_pressed) { // Not scrolling menus
    if (btn_a_pressed)
    {
      hw_set_button(BTN_LEFT, BTN_STATE_PRESSED);
    }
    else
    {
      hw_set_button(BTN_LEFT, BTN_STATE_RELEASED);
    }
  }

  if (!btn_b_pressed || (millis() - btn_b_hold_start < 2000)) { // Not toggling effects
    if (btn_b_pressed)
    {
      hw_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
    }
    else
    {
      hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
    }
  }

  if (!btn_pwr_pressed || !btn_a_pressed) { // Not scrolling menus
    // Also check if we're not in double-tap detection mode
    if (!waiting_for_portal_second_tap || (millis() - last_btn_pwr_press > 500)) {
      if (btn_pwr_pressed)
      {
        hw_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
      }
      else
      {
        hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
      }
    }
  }
  
  // Update button states for next iteration
  btn_a_was_pressed = btn_a_pressed;
  btn_b_was_pressed = btn_b_pressed;
  btn_pwr_was_pressed = btn_pwr_pressed;
  
  // Handle TamaPortal background tasks
  handleTamaPortal();
  
  // Handle web server if TamaPortal hotspot is active
  if (tamaportal_active && tamaPortalServer != nullptr) {
    tamaDnsServer->processNextRequest();
    tamaPortalServer->handleClient();
  }
  
  // Clear messages when Clean function is selected (icon_buffer[4] is Clean)
  static bool prev_clean_selected = false;
  if (icon_buffer[4] && !prev_clean_selected && received_message.length() > 0) {
    received_message = ""; // Clear message when clean is selected
    Serial.println("Message cleared with Clean function");
  }
  prev_clean_selected = icon_buffer[4];
#else
  if (digitalRead(PIN_BTN_L) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    hw_set_button(BTN_LEFT, BTN_STATE_PRESSED);
  }
  else
  {
    hw_set_button(BTN_LEFT, BTN_STATE_RELEASED);
  }

  if (digitalRead(PIN_BTN_M) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    hw_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
  }
  else
  {
    hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
  }

  if (digitalRead(PIN_BTN_R) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    hw_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
  }
  else
  {
    hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
  }
#endif

  return 0;
}

static hal_t hal = {
    .halt = &hal_halt,
    .log = &hal_log,
    .sleep_until = &hal_sleep_until,
    .get_timestamp = &hal_get_timestamp,
    .update_screen = &hal_update_screen,
    .set_lcd_matrix = &hal_set_lcd_matrix,
    .set_lcd_icon = &hal_set_lcd_icon,
    .set_frequency = &hal_set_frequency,
    .play_frequency = &hal_play_frequency,
    .handler = &hal_handler,
};

#ifdef M5STICKC_PLUS2

// 90s Retro Border Effect
void draw90sBorder()
{
  if (!effects_enabled) return;
  
  // Animated neon border
  unsigned long time = millis();
  uint16_t color1 = (time / 100) % 2 ? NEON_CYAN : NEON_MAGENTA;
  uint16_t color2 = (time / 150) % 2 ? NEON_GREEN : NEON_YELLOW;
  
  // Top/Bottom borders with gradient effect
  for (int i = 0; i < 3; i++) {
    M5.Lcd.drawLine(0, i, 239, i, color1);
    M5.Lcd.drawLine(0, 134-i, 239, 134-i, color1);
  }
  
  // Left/Right borders
  for (int i = 0; i < 3; i++) {
    M5.Lcd.drawLine(i, 0, i, 134, color2);
    M5.Lcd.drawLine(239-i, 0, 239-i, 134, color2);
  }
  
  // Corner accents
  M5.Lcd.fillRect(0, 0, 8, 8, NEON_ORANGE);
  M5.Lcd.fillRect(232, 0, 8, 8, NEON_ORANGE);
  M5.Lcd.fillRect(0, 127, 8, 8, NEON_ORANGE);
  M5.Lcd.fillRect(232, 127, 8, 8, NEON_ORANGE);
}

// Sound Toggle Visual Feedback
void showSoundToggleFeedback(bool sound_on)
{
  // Flash the entire border with appropriate color
  uint16_t feedback_color = sound_on ? NEON_GREEN : TFT_RED;
  
  // Brief flash effect
  for (int flash = 0; flash < 2; flash++) {
    // Top/Bottom borders
    for (int i = 0; i < 5; i++) {
      M5.Lcd.drawLine(0, i, 239, i, feedback_color);
      M5.Lcd.drawLine(0, 134-i, 239, 134-i, feedback_color);
    }
    
    // Left/Right borders  
    for (int i = 0; i < 5; i++) {
      M5.Lcd.drawLine(i, 0, i, 134, feedback_color);
      M5.Lcd.drawLine(239-i, 0, 239-i, 134, feedback_color);
    }
    
    // Corner emphasis
    M5.Lcd.fillRect(0, 0, 12, 12, feedback_color);
    M5.Lcd.fillRect(228, 0, 12, 12, feedback_color);
    M5.Lcd.fillRect(0, 123, 12, 12, feedback_color);
    M5.Lcd.fillRect(228, 123, 12, 12, feedback_color);
    
    delay(100);
    
    // Clear flash
    M5.Lcd.fillRect(0, 0, 240, 5, TFT_BLACK);
    M5.Lcd.fillRect(0, 130, 240, 5, TFT_BLACK);
    M5.Lcd.fillRect(0, 0, 5, 135, TFT_BLACK);
    M5.Lcd.fillRect(235, 0, 5, 135, TFT_BLACK);
    
    delay(50);
  }
  
  // Show text feedback briefly
  M5.Lcd.setTextColor(feedback_color);
  M5.Lcd.setTextSize(2);
  String message = sound_on ? "SOUND ON" : "SOUND OFF";
  int text_x = (240 - message.length() * 12) / 2; // Center text
  M5.Lcd.fillRect(text_x - 5, 60, message.length() * 12 + 10, 20, TFT_BLACK);
  M5.Lcd.drawString(message, text_x, 65);
  
  delay(1000); // Show message for 1 second
}

// Enhanced Triangle with 90s glow
void drawTriangle(uint16_t x, uint16_t y)
{
  // Scale and offset triangle for M5StickC display
  x = (x * DISPLAY_SCALE) + DISPLAY_OFFSET_X;
  y = (y * DISPLAY_SCALE) + DISPLAY_OFFSET_Y;
  
  if (effects_enabled) {
    // Glowing effect
    M5.Lcd.drawLine(x + 2, y + 2, x + 10, y + 2, NEON_CYAN);
    M5.Lcd.drawLine(x + 4, y + 4, x + 8, y + 4, NEON_CYAN);
    M5.Lcd.drawLine(x + 6, y + 6, x + 6, y + 6, NEON_CYAN);
    
    // Glow halo
    M5.Lcd.drawLine(x + 1, y + 1, x + 11, y + 1, NEON_PURPLE);
    M5.Lcd.drawLine(x + 3, y + 5, x + 9, y + 5, NEON_PURPLE);
  } else {
    // Original white triangle
    M5.Lcd.drawLine(x + 2, y + 2, x + 10, y + 2, TFT_WHITE);
    M5.Lcd.drawLine(x + 4, y + 4, x + 8, y + 4, TFT_WHITE);
    M5.Lcd.drawLine(x + 6, y + 6, x + 6, y + 6, TFT_WHITE);
  }
}

// Enhanced Selection Box Drawing with Labels
void drawEnhancedSelection(uint16_t y)
{
  uint8_t i;
  const char* labels[] = {"S", "F", "L", "G", "C", "A", "H", "D"};  // Status, Feed, Light, Game, Clean, Art(new), Health, Discipline
  const uint16_t colors[] = {NEON_CYAN, NEON_GREEN, NEON_YELLOW, NEON_MAGENTA, NEON_ORANGE, NEON_PURPLE, TFT_RED, TFT_WHITE};
  
  for (i = 0; i < 8; i++)  // Extended to 8 for new features
  {
    uint16_t x = (i * 16 + 4) * DISPLAY_SCALE + DISPLAY_OFFSET_X;
    uint16_t icon_y = (y + 6) * DISPLAY_SCALE + DISPLAY_OFFSET_Y;
    
    // Draw selection triangle if active
    if (i < 7 && icon_buffer[i])
      drawTriangle(i * 16 + 5, y);
    else if (i == 7 && icon_buffer[7])  // Handle 8th icon
      drawTriangle(i * 16 + 5, y);
    
    // Draw enhanced box with 90s styling
    if (effects_enabled) {
      // Colorful boxes
      uint16_t box_color = (current_menu_page == 0) ? colors[i] : NEON_CYAN;
      M5.Lcd.drawRect(x, icon_y, 16 * DISPLAY_SCALE, 9 * DISPLAY_SCALE, box_color);
      M5.Lcd.drawRect(x+1, icon_y+1, 14 * DISPLAY_SCALE, 7 * DISPLAY_SCALE, box_color);
      
      // Add letter label
      M5.Lcd.setTextColor(box_color);
      M5.Lcd.setTextSize(2);
      
      if (current_menu_page == 0) {
        // Original Tamagotchi functions
        M5.Lcd.drawString(labels[i], x + 12, icon_y + 8);
      } else {
        // Enhanced functions page 2
        const char* enhanced_labels[] = {"<", "T", "E", "M", "W", "N", ">", "O"};  // <Prev, Theme, Effects, Music, WiFi, New, Next>, Options
        M5.Lcd.drawString(enhanced_labels[i], x + 12, icon_y + 8);
      }
    } else {
      // Original simple boxes
      M5.Lcd.drawRect(x, icon_y, 16 * DISPLAY_SCALE, 9 * DISPLAY_SCALE, TFT_WHITE);
    }
  }
}

// Pixelated Art Explosion Effect
void pixelatedArtExplosion()
{
  M5.Lcd.fillScreen(TFT_BLACK);
  
  // Create random pixelated pattern
  for (int frame = 0; frame < 30; frame++) {  // 30 frame animation
    for (int i = 0; i < 50; i++) {  // 50 random pixels per frame
      int x = random(0, 240);
      int y = random(0, 135);
      int size = random(2, 8);
      uint16_t color = random(0, 65535);  // Random color
      
      // Draw pixelated square
      M5.Lcd.fillRect(x, y, size, size, color);
    }
    
    // Add some structure - geometric patterns
    if (frame > 10) {
      int center_x = 120;
      int center_y = 67;
      int radius = (frame - 10) * 4;
      
      for (int angle = 0; angle < 360; angle += 30) {
        float rad = angle * PI / 180.0;
        int px = center_x + radius * cos(rad);
        int py = center_y + radius * sin(rad);
        
        if (px >= 0 && px < 240 && py >= 0 && py < 135) {
          M5.Lcd.fillRect(px-2, py-2, 4, 4, NEON_CYAN);
        }
      }
    }
    
    delay(50);  // Animation speed
  }
  
  // Final pattern - matrix effect
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextColor(NEON_GREEN);
  M5.Lcd.setTextSize(1);
  
  for (int i = 0; i < 200; i++) {
    int x = random(0, 30) * 8;
    int y = random(0, 15) * 9;
    char c = '0' + random(0, 10);
    M5.Lcd.drawChar(x, y, c);
  }
  
  delay(1000);  // Hold final pattern
}

// TamaPortal System Implementation
void initTamaPortal() {
  if (tamaportal_active) {
    // Deactivate portal
    tamaportal_active = false;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.println("TamaPortal deactivated");
    
    // Show deactivation message
    M5.Lcd.fillRect(60, 60, 120, 40, TFT_BLACK);
    M5.Lcd.setTextColor(TFT_RED);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("PORTAL", 80, 65);
    M5.Lcd.drawString("INACTIVE", 70, 85);
    delay(1500);
  } else {
    // Activate portal
    tamaportal_active = true;
    last_portal_scan = 0; // Force immediate scan
    Serial.println("TamaPortal activated");
    
    // Show activation message
    M5.Lcd.fillRect(60, 60, 120, 40, TFT_BLACK);
    M5.Lcd.setTextColor(NEON_GREEN);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("PORTAL", 80, 65);
    M5.Lcd.drawString("ACTIVE", 75, 85);
    delay(1500);
    
    // Start immediate background scan
    scanAndAttackPortals();
  }
}

void handleTamaPortal() {
  if (!tamaportal_active) return;
  
  unsigned long now = millis();
  
  // Periodic scanning
  if (now - last_portal_scan > PORTAL_SCAN_INTERVAL) {
    scanAndAttackPortals();
    last_portal_scan = now;
  }
  
  // Handle incoming web requests if we're hosting
  // This would be called from a web server handler
}

void scanAndAttackPortals() {
  Serial.println("TamaPortal: Scanning for networks...");
  
  // Scan for networks
  int n = WiFi.scanNetworks();
  
  if (n == 0) {
    Serial.println("No networks found, creating TamaPortal hotspot");
    createTamaPortalHotspot();
    return;
  }
  
  bool foundOpenNetwork = false;
  
  // Look for open networks to send friendly messages
  for (int i = 0; i < n; i++) {
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
      String ssid = WiFi.SSID(i);
      Serial.printf("Found open network: %s\n", ssid.c_str());
      sendFriendlyMessage(ssid);
      foundOpenNetwork = true;
      delay(1000); // Don't spam too fast
    }
  }
  
  // If no open networks found, create our own hotspot
  if (!foundOpenNetwork) {
    Serial.println("No open networks found, creating TamaPortal hotspot");
    createTamaPortalHotspot();
  }
  
  WiFi.scanDelete(); // Clean up
}

void sendFriendlyMessage(String ssid) {
  Serial.printf("Sending friendly message to: %s\n", ssid.c_str());
  
  // Try to connect
  WiFi.begin(ssid.c_str());
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 5000) {
    delay(100);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    // Get gateway IP
    IPAddress gateway = WiFi.gatewayIP();
    String gatewayIP = gateway.toString();
    
    // Friendly Tamagotchi messages
    String friendlyMessages[] = {
      "Hello from my Tamagotchi! 🐣",
      "Virtual pet owner nearby! ✨", 
      "My Tamagotchi says hi! 👋",
      "Remember to feed your pets! 💖",
      "90s nostalgia activated! 🎮",
      "Pixel pets forever! 🎨"
    };
    
    int messageIndex = random(0, 6);
    String message1 = friendlyMessages[messageIndex];
    String message2 = friendlyMessages[(messageIndex + 1) % 6];
    
    // Try common portal endpoints
    String endpoints[] = {"/post", "/", "/login", "/auth"};
    
    for (String endpoint : endpoints) {
      HTTPClient http;
      String url = "http://" + gatewayIP + endpoint;
      http.begin(url);
      http.setTimeout(2000);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      
      // Send friendly data
      String friendlyData = "email=" + message1 + "&password=" + message2 + "&username=" + message1;
      
      int responseCode = http.POST(friendlyData);
      Serial.printf("Sent friendly message to %s - Response: %d\n", url.c_str(), responseCode);
      
      http.end();
      delay(500);
    }
    
    WiFi.disconnect();
  }
}

void createTamaPortalHotspot() {
  static bool hotspotCreated = false;
  
  if (hotspotCreated) return; // Already running
  
  Serial.println("Creating TamaPortal hotspot...");
  
  // Create hotspot
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("TamaPortal-" + String(random(1000, 9999)));
  
  // Initialize DNS server
  tamaDnsServer = new DNSServer();
  tamaDnsServer->start(53, "*", IPAddress(192, 168, 4, 1));
  
  // Initialize web server
  tamaPortalServer = new WebServer(80);
  
  // Handle root and all requests
  tamaPortalServer->on("/", []() {
    String html = "<!DOCTYPE html><html><head><title>TamaPortal</title>";
    html += "<style>body{background:#000080;color:#00FFFF;font-family:Arial;text-align:center;margin:50px;}";
    html += "h1{color:#FF00FF;font-size:28px;}h2{color:#FFFF00;font-size:20px;}";
    html += "textarea{width:300px;height:60px;font-size:16px;}";
    html += "input[type=submit]{background:#FF00FF;color:white;padding:10px 20px;font-size:16px;border:none;}</style>";
    html += "</head><body>";
    html += "<h1>📱 TamaPortal 📱</h1>";
    html += "<h2>Send a message to my virtual pet!</h2>";
    html += "<p>Your message will appear on my Tamagotchi screen!</p>";
    html += "<form action='/message' method='post'>";
    html += "<textarea name='msg' placeholder='Type your friendly message here (2 lines max)...'></textarea><br><br>";
    html += "<input type='submit' value='SEND TO TAMAGOTCHI'>";
    html += "</form></body></html>";
    
    tamaPortalServer->send(200, "text/html", html);
  });
  
  // Handle message submission
  tamaPortalServer->on("/message", HTTP_POST, []() {
    String message = tamaPortalServer->arg("msg");
    message.replace("<", "&lt;");
    message.replace(">", "&gt;");
    message = message.substring(0, 100); // Limit length
    
    // Store message for display
    received_message = message;
    message_display_time = millis();
    
    Serial.printf("Received message: %s\n", message.c_str());
    
    // Thank you page
    String html = "<!DOCTYPE html><html><head><title>TamaPortal</title>";
    html += "<style>body{background:#000080;color:#00FFFF;font-family:Arial;text-align:center;margin:50px;}";
    html += "h1{color:#00FF00;font-size:28px;}</style></head><body>";
    html += "<h1>✅ Message sent to Tamagotchi!</h1>";
    html += "<p>Your message has been delivered to my virtual pet.</p>";
    html += "<p><a href='/' style='color:#FFFF00;'>Send another message</a></p>";
    html += "</body></html>";
    
    tamaPortalServer->send(200, "text/html", html);
  });
  
  // Redirect all other requests to root
  tamaPortalServer->onNotFound([]() {
    tamaPortalServer->sendHeader("Location", "/", true);
    tamaPortalServer->send(302, "text/plain", "");
  });
  
  tamaPortalServer->begin();
  hotspotCreated = true;
  
  Serial.println("TamaPortal hotspot created successfully!");
}

void drawTamaRow(uint8_t tamaLCD_y, uint16_t ActualLCD_y, uint8_t thick)
{
  uint8_t i;
  for (i = 0; i < LCD_WIDTH; i++)
  {
    uint8_t mask = 0b10000000;
    mask = mask >> (i % 8);
    if ((matrix_buffer[tamaLCD_y][i / 8] & mask) != 0)
    {
      uint16_t x = (i * 3 + 16) * DISPLAY_SCALE + DISPLAY_OFFSET_X;
      uint16_t y = ActualLCD_y * DISPLAY_SCALE + DISPLAY_OFFSET_Y;
      
      // Enhanced pixels with subtle glow if effects enabled
      if (effects_enabled && ((millis() / 200) % 3 == 0)) {
        M5.Lcd.fillRect(x, y, 2 * DISPLAY_SCALE, thick * DISPLAY_SCALE, NEON_GREEN);
      } else {
        M5.Lcd.fillRect(x, y, 2 * DISPLAY_SCALE, thick * DISPLAY_SCALE, TFT_WHITE);
      }
    }
  }
}

void drawTamaSelection(uint16_t y)
{
  drawEnhancedSelection(y);
}

void displayTama()
{
  // Background with cool animated effect or simple black
  if (effects_enabled) {
    // Simple animated starfield effect instead of Matrix rain
    unsigned long time = millis();
    M5.Lcd.fillScreen(TFT_BLACK);
    
    // Draw animated "stars" - much simpler than Matrix
    for (int i = 0; i < 20; i++) {
      int x = ((time / 100 + i * 31) % 240); // Moving across screen
      int y = ((time / 150 + i * 17) % 135); // Different speeds
      
      // Only draw in safe areas (avoid center Tamagotchi area)
      if ((x < 50 || x > 190) || (y < 15 || y > 110)) {
        uint16_t star_color = (i % 3 == 0) ? NEON_CYAN : 
                             (i % 3 == 1) ? NEON_GREEN : TFT_WHITE;
        M5.Lcd.drawPixel(x, y, star_color);
        // Make some stars bigger
        if (i % 5 == 0) {
          M5.Lcd.drawPixel(x+1, y, star_color);
          M5.Lcd.drawPixel(x, y+1, star_color);
        }
      }
    }
  } else {
    M5.Lcd.fillScreen(TFT_BLACK);
  }
  
  // Draw 90s border
  draw90sBorder();
  
  // Draw main Tamagotchi display area
  uint8_t j;
  for (j = 0; j < LCD_HEIGHT; j++)
  {
    if (j != 5)
      drawTamaRow(j, j * 3, 2);
    if (j == 5)
    {
      drawTamaRow(j, j * 3, 1);
      drawTamaRow(j, j * 3 + 1, 1);
    }
  }
  
  // Draw enhanced selection area
  drawTamaSelection(49);
  
  // Page indicator and sound status
  if (effects_enabled) {
    M5.Lcd.setTextColor(NEON_YELLOW);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString(String("Page ") + String(current_menu_page + 1) + "/" + String(max_menu_pages), 10, 120);
    
    // Sound status indicator
    M5.Lcd.setTextColor(sound_enabled ? NEON_GREEN : TFT_RED);
    String sound_status = sound_enabled ? "♪" : "♪";
    M5.Lcd.drawString(sound_status, 200, 120);
    
    // Add sound text for clarity
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString(sound_enabled ? "ON" : "OFF", 210, 120);
  }
  
  // Display received TamaPortal message
  if (received_message.length() > 0 && (millis() - message_display_time < MESSAGE_DISPLAY_DURATION)) {
    // Message overlay
    M5.Lcd.fillRect(20, 40, 200, 50, TFT_BLACK);
    M5.Lcd.drawRect(18, 38, 204, 54, NEON_CYAN);
    M5.Lcd.drawRect(19, 39, 202, 52, NEON_CYAN);
    
    M5.Lcd.setTextColor(NEON_GREEN);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString("Message received:", 25, 45);
    
    M5.Lcd.setTextColor(TFT_WHITE);
    // Split message into two lines if too long
    String line1 = received_message.substring(0, 30);
    String line2 = received_message.substring(30, 60);
    
    M5.Lcd.drawString(line1, 25, 60);
    if (line2.length() > 0) {
      M5.Lcd.drawString(line2, 25, 75);
    }
    
    // Show hint to clear message
    M5.Lcd.setTextColor(NEON_YELLOW);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString("Use Clean to clear", 100, 85);
  } else if (received_message.length() > 0) {
    // Message expired, clear it
    received_message = "";
  }
}

#else
void drawTriangle(uint8_t x, uint8_t y)
{
  display.drawLine(x + 1, y + 1, x + 5, y + 1);
  display.drawLine(x + 2, y + 2, x + 4, y + 2);
  display.drawLine(x + 3, y + 3, x + 3, y + 3);
}

void drawTamaRow(uint8_t tamaLCD_y, uint8_t ActualLCD_y, uint8_t thick)
{
  uint8_t i;
  for (i = 0; i < LCD_WIDTH; i++)
  {
    uint8_t mask = 0b10000000;
    mask = mask >> (i % 8);
    if ((matrix_buffer[tamaLCD_y][i / 8] & mask) != 0)
    {
      display.drawBox(i + i + i + 16, ActualLCD_y, 2, thick);
    }
  }
}

void drawTamaSelection(uint8_t y)
{
  uint8_t i;
  for (i = 0; i < 7; i++)
  {
    if (icon_buffer[i])
      drawTriangle(i * 16 + 5, y);
    display.drawXBMP(i * 16 + 4, y + 6, 16, 9, bitmaps + i * 18);
  }
  if (icon_buffer[7])
  {
    drawTriangle(7 * 16 + 5, y);
    display.drawXBMP(7 * 16 + 4, y + 6, 16, 9, bitmaps + 7 * 18);
  }
}

void displayTama()
{
  uint8_t j;
  display.firstPage();
  for (j = 0; j < LCD_HEIGHT; j++)
  {
    if (j != 5)
      drawTamaRow(j, j + j + j, 2);
    if (j == 5)
    {
      drawTamaRow(j, j + j + j, 1);
      display.nextPage();
      drawTamaRow(j, j + j + j + 1, 1);
    }
    if (j == 10)
      display.nextPage();
  }
  display.nextPage();
  drawTamaSelection(49);
  display.nextPage();
}
#endif

uint8_t reverseBits(uint8_t num)
{
  uint8_t reverse_num = 0;
  uint8_t i;
  for (i = 0; i < 8; i++)
  {
    if ((num & (1 << i)))
      reverse_num |= 1 << ((8 - 1) - i);
  }
  return reverse_num;
}

void setup()
{
  Serial.begin(SERIAL_BAUD);

#ifdef M5STICKC_PLUS2
  M5.begin();
  M5.Lcd.setRotation(3); // Landscape mode
  
  // 90s Retro Splash Screen
  M5.Lcd.fillScreen(TFT_BLACK);
  
  // Animated border
  for (int i = 0; i < 5; i++) {
    draw90sBorder();
    delay(200);
    M5.Lcd.fillScreen(TFT_BLACK);
    delay(100);
  }
  
  // Final splash with effects
  M5.Lcd.fillScreen(TFT_BLACK);
  draw90sBorder();
  
  M5.Lcd.setTextColor(NEON_CYAN);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString("ArduinoGotchi", 60, 30);
  
  M5.Lcd.setTextColor(NEON_MAGENTA);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString("ENHANCED", 70, 50);
  
  M5.Lcd.setTextColor(NEON_YELLOW);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawString("90s Retro Edition", 80, 75);
  
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.drawString("A+PWR: Menu | B(hold): Effects", 10, 95);
  M5.Lcd.drawString("Double-tap B: Sound | PWR: Game", 10, 105);
  M5.Lcd.drawString("A+B+PWR: Art | Clean: Clear Msgs", 10, 115);
  
  // Show controls briefly
  delay(4000);
#else
  pinMode(PIN_BTN_L, INPUT);
  pinMode(PIN_BTN_M, INPUT);
  pinMode(PIN_BTN_R, INPUT);

#if defined(ESP32)
  ledcSetup(BUZZER_CHANNEL, NOTE_C4, 8);
#endif

  display.begin();
#endif

  tamalib_register_hal(&hal);
  tamalib_set_framerate(TAMA_DISPLAY_FRAMERATE);
  tamalib_init(1000000);

#if defined(ENABLE_AUTO_SAVE_STATUS) || defined(ENABLE_LOAD_STATE_FROM_EEPROM)
  initEEPROM();
#endif

#ifdef ENABLE_LOAD_STATE_FROM_EEPROM
  if (validEEPROM())
  {
    loadStateFromEEPROM(&cpuState);
  }
  else
  {
    Serial.println(F("No magic number in state, skipping state restore"));
  }
#elif ENABLE_LOAD_HARCODED_STATE_WHEN_START
  loadHardcodedState();
#endif

#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START
  dumpStateToSerial();
#endif
}

uint32_t right_long_press_started = 0;

void upload_state()
{
}

void enter_deepsleep(int _ms)
{
#ifndef ENABLE_DEEPSLEEP
  return;
#endif
  // save CURRENT STATE
  saveStateToEEPROM(&cpuState);

#ifdef M5STICKC_PLUS2
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.drawString("Deep Sleep...", 10, 60);
  delay(1000);
#else
  //DISABLE DISPLAY
  display.clear();
#endif

  // ENTER DEEPSLEEP
#if defined(ESP32)
  esp_sleep_enable_timer_wakeup(_ms * 1000);
  esp_deep_sleep_start();
#elif defined(ESP8266)
  ESP.deepSleep(_ms * 1000);
  yield();
#endif
}

void loop()
{
  tamalib_mainloop_step_by_step();
  
#ifdef ENABLE_AUTO_SAVE_STATUS
  if ((millis() - lastSaveTimestamp) > (AUTO_SAVE_MINUTES * 60 * 1000))
  {
    lastSaveTimestamp = millis();
    saveStateToEEPROM(&cpuState);
  }

#ifdef M5STICKC_PLUS2
  if (M5.BtnB.isPressed())
  {
    if (right_long_press_started == 0)
      right_long_press_started = millis();
    
    if (millis() - right_long_press_started > AUTO_SAVE_MINUTES * 1000)
    {
      eraseStateFromEEPROM();
      ESP.restart();
    }
  }
  else
  {
    right_long_press_started = 0;
  }

  if (M5.BtnA.pressedFor(5000))
  {
    enter_deepsleep(DEEPSLEEP_INTERVAL * 1000);
  }
#else
  if (digitalRead(PIN_BTN_M) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    if (millis() - right_long_press_started > AUTO_SAVE_MINUTES * 1000)
    {
      eraseStateFromEEPROM();
#if defined(ESP8266) || defined(ESP32)
      ESP.restart();
#endif
    }
  }
  else
  {
    right_long_press_started = 0;
  }
#endif
#endif
}

#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START
void dumpStateToSerial()
{
  uint16_t i, count = 0;
  char tmp[10];
  cpu_get_state(&cpuState);
  u4_t *memTemp = cpuState.memory;
  uint8_t *cpuS = (uint8_t *)&cpuState;

  Serial.println("");
  Serial.println("static const uint8_t hardcodedState[] PROGMEM = {");
  for (i = 0; i < sizeof(cpu_state_t); i++, count++)
  {
    sprintf(tmp, "0x%02X,", cpuS[i]);
    Serial.print(tmp);
    if ((count % 16) == 15)
      Serial.println("");
  }
  for (i = 0; i < MEMORY_SIZE; i++, count++)
  {
    sprintf(tmp, "0x%02X,", memTemp[i]);
    Serial.print(tmp);
    if ((count % 16) == 15)
      Serial.println("");
  }
  Serial.println("};");
}
#endif

// Matrix Rain Effect - Animated background
void drawMatrixRain() {
  // Initialize matrix drops on first run
  if (!matrix_initialized) {
    for (int i = 0; i < 30; i++) {
      matrix_drops[i] = random(0, 135); // Start at random heights
      matrix_lengths[i] = random(10, 20); // Random trail lengths
    }
    matrix_initialized = true;
  }
  
  // Update matrix animation every 60ms
  if (millis() - last_matrix_update > 60) {
    // Don't clear the whole screen - just draw over old drops
    
    // Draw each matrix column
    for (int col = 0; col < 30; col++) {
      int x = col * 8; // 8 pixels apart
      int y = matrix_drops[col];
      int trail_length = matrix_lengths[col];
      
      // Clear old trail (draw black pixels above the drop)
      int clear_y = y - trail_length - 5;
      if (clear_y >= 0 && clear_y < 135) {
        for (int i = 0; i < 3; i++) { // Clear a few pixels
          if (clear_y + i < 135 && clear_y + i >= 0) {
            M5.Lcd.drawPixel(x, clear_y + i, TFT_BLACK);
          }
        }
      }
      
      // Draw the green matrix trail
      for (int trail = 0; trail < trail_length; trail++) {
        int trail_y = y - trail;
        if (trail_y >= 0 && trail_y < 135 && x < 240) {
          // Fade from bright green to dark green
          uint16_t green_color;
          if (trail == 0) {
            green_color = 0x07FF; // Bright green/cyan for the head
          } else if (trail < trail_length / 3) {
            green_color = NEON_GREEN; // Bright green
          } else if (trail < trail_length * 2 / 3) {
            green_color = 0x03E0; // Medium green
          } else {
            green_color = 0x01C0; // Dark green
          }
          
          // Draw random matrix-like characters (just pixels in pattern)
          if (random(0, 3) == 0) { // Only draw some pixels for character effect
            M5.Lcd.drawPixel(x, trail_y, green_color);
          }
        }
      }
      
      // Move drop down
      matrix_drops[col] += random(1, 4); // Variable speed
      
      // Reset drop when it goes off screen
      if (matrix_drops[col] > 135 + matrix_lengths[col]) {
        matrix_drops[col] = -random(10, 30); // Start above screen
        matrix_lengths[col] = random(8, 16); // New random length
      }
    }
    
    last_matrix_update = millis();
  }
}