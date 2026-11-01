// PCB v1 board helpers: IDF headers its lambdas need.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_PARAM_KEY_REMOTE  // param_key: role: remote

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#endif  // USE_PARAM_KEY_REMOTE
