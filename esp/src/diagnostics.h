#pragma once
#include <Arduino.h>

void diagnosticsBegin();
void diagnosticLog(const char *format, ...);
void diagnosticsTick();
String diagnosticsJson();
String diagnosticsStatus();
void diagnosticsStage(const char *stage, uint32_t progress = 0);
