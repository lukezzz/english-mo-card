#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "config.h"

#define EPD_W21_RST_0 digitalWrite(EPD_PIN_RST, LOW)
#define EPD_W21_RST_1 digitalWrite(EPD_PIN_RST, HIGH)
#define isEPD_W21_BUSY digitalRead(EPD_PIN_BUSY)

inline void EPD_W21_WriteCMD(uint8_t value) {
    digitalWrite(EPD_PIN_CS, LOW);
    digitalWrite(EPD_PIN_DC, LOW);
    delayMicroseconds(200);
    SPI.transfer(value);
    delayMicroseconds(200);
    digitalWrite(EPD_PIN_CS, HIGH);
}
inline void EPD_W21_WriteDATA(uint8_t value) {
    digitalWrite(EPD_PIN_CS, LOW);
    digitalWrite(EPD_PIN_DC, HIGH);
    delayMicroseconds(2);
    SPI.transfer(value);
    delayMicroseconds(2);
    digitalWrite(EPD_PIN_CS, HIGH);
}
inline void Sys_run() { yield(); }
