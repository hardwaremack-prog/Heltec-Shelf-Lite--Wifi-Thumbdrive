// Heltec Shelf Lite - board pin map
// ONE wiring diagram works on the Heltec WiFi LoRa 32 V3, V4 (ESP32-S3R2) and V4-R8.
// The V4 firmware is built with -DSHELF_BOARD_V4; everything else defaults to V3.
//
// Pins picked to stay clear of: the LoRa radio (8-14), the OLED (17, 18, 21),
// USB (19, 20), the V4's high-power LoRa amplifier controls (2, 5, 7),
// the V4-R8's octal PSRAM (33-37), battery sensing (1, 37) and boot pins (0, 3, 45, 46).
#pragma once

#define FW_VERSION "1.0.0"

#if defined(SHELF_BOARD_V4)
  #define BOARD_NAME "Heltec V4"
  #define BOARD_ID   "v4"
#else
  #define BOARD_NAME "Heltec V3"
  #define BOARD_ID   "v3"
#endif

// --- Built in to the board (same on V3 and V4) ---
#define OLED_SDA   17
#define OLED_SCL   18
#define OLED_RST   21
#define OLED_ADDR  0x3C
#define PIN_BUTTON 0      // PRG button
#define PIN_VEXT   36     // LOW = OLED/Ve power on (V3 and V4-R2)
#if defined(SHELF_BOARD_V4)
  #define PIN_VEXT_ALT 40 // Vext moved here on the V4-R8 variant
#endif

// --- microSD module (its own SPI bus) ---
#define SD_CS    4
#define SD_MISO  6
#define SD_SCK   47
#define SD_MOSI  48

// --- MAX98357A I2S amp (optional speaker) ---
#define I2S_BCLK 39
#define I2S_LRC  41
#define I2S_DOUT 38
