#pragma once

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"
// Optional: match EPD_API_TOKEN on the flash card server; leave empty to disable.
#define EPD_API_TOKEN ""

// XIAO ESP32-C3 GPIO numbers. Adjust to match your EPD adapter wiring.
#define EPD_PIN_BUSY 4  // D2
#define EPD_PIN_CS   3  // D1
#define EPD_PIN_DC   5  // D3
#define EPD_PIN_RST  2  // D0
#define EPD_PIN_SCK  8  // D8
#define EPD_PIN_MOSI 10 // D10
#define EPD_PIN_MISO 9  // D9; leave unconnected, EPD is write-only
