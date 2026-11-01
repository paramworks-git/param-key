// Battery percentage from the cell voltage, the way a phone shows it without a
// fuel gauge: a LiPo discharge curve instead of a straight line, smoothed across
// readings, and never rising while on battery. Board-independent.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_PARAM_KEY_REMOTE  // param_key: role: remote

#include <cmath>
#include <cstdint>

#include "esp_attr.h"

namespace pk {

// Typical single-cell LiPo at light load (<0.1 C): volts -> percent. The middle
// is flat (3.75-3.9 V covers ~50%), so a straight line misreads most of it.
inline float lipo_percent(float v) {
  static const float CURVE[][2] = {
      {3.27f, 0},  {3.61f, 5},  {3.69f, 10}, {3.71f, 15}, {3.73f, 20}, {3.75f, 25}, {3.77f, 30},
      {3.79f, 35}, {3.80f, 40}, {3.82f, 45}, {3.84f, 50}, {3.85f, 55}, {3.87f, 60}, {3.91f, 65},
      {3.95f, 70}, {3.98f, 75}, {4.02f, 80}, {4.08f, 85}, {4.11f, 90}, {4.15f, 95}, {4.20f, 100}};
  constexpr int N = sizeof(CURVE) / sizeof(CURVE[0]);
  if (v <= CURVE[0][0])
    return 0;
  for (int i = 1; i < N; i++) {
    if (v <= CURVE[i][0]) {
      float t = (v - CURVE[i - 1][0]) / (CURVE[i][0] - CURVE[i - 1][0]);
      return CURVE[i - 1][1] + t * (CURVE[i][1] - CURVE[i - 1][1]);
    }
  }
  return 100;
}

// Kept in RTC memory so the smoothing and the "never rises" rule carry across
// deep sleep; a cold boot starts fresh.
struct BatteryGauge {
  static constexpr uint32_t MAGIC = 0x0B477E21;
  static constexpr float ALPHA = 0.3f;  // weight of each new reading
  uint32_t magic;
  float smooth;  // percent, exponentially smoothed
  float shown;   // percent reported
  bool was_charging;

  // Returns the percent to report for a voltage reading.
  float update(float volts, bool charging) {
    if (std::isnan(volts))
      return NAN;
    float pct = lipo_percent(volts);
    // Fresh start, or just unplugged: take the reading as it is. While
    // charging it follows the (charger-inflated) voltage up.
    if (this->magic != MAGIC || this->was_charging != charging) {
      this->magic = MAGIC;
      this->smooth = pct;
      this->shown = pct;
    } else {
      this->smooth += ALPHA * (pct - this->smooth);
      this->shown = charging ? this->smooth : std::fmin(this->shown, this->smooth);
    }
    this->was_charging = charging;
    return std::round(this->shown);
  }
};

static RTC_DATA_ATTR BatteryGauge battery_gauge;

}  // namespace pk

#endif  // USE_PARAM_KEY_REMOTE
