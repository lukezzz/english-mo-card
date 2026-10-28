#ifndef _EPD_GDEY042Z98_H_
#define _EPD_GDEY042Z98_H_

#include <Arduino.h>
#include "epd_profiles.h"

bool GDEY042Z98_refreshLogicalFrame(const uint8_t *logicalFrame, EPDColorMode mode);

#endif
