// ESP-IDF glue shared by remote and bridge: a flash value written through at
// once, and the hardware random source for mbedtls.
//
// Stored: a value kept in flash and written through at once. ESPHome's restore_value
// globals only reach flash at the next flash_write_interval, which is too late
// for the send-counter lease (pk_crypto.h): a crash in between would reuse
// counters, and with them CCM nonces.
#pragma once

#include "esp_random.h"
#include "esphome/core/preferences.h"

namespace pk {

// mbedtls-style RNG on the hardware generator (Wi-Fi is on, so it's a true RNG).
inline int esp_rng(void *, unsigned char *buf, size_t len) {
  esp_fill_random(buf, len);
  return 0;
}

template<typename T> struct Stored {
  T value{};

  // key: a fixed, unique number per value (the preference's hash).
  void load(uint32_t key) {
    this->pref_ = esphome::global_preferences->make_preference<T>(key, true);
    if (!this->pref_.load(&this->value))
      this->value = T{};
  }

  bool save() { return this->pref_.save(&this->value) && esphome::global_preferences->sync(); }

 protected:
  esphome::ESPPreferenceObject pref_;
};

}  // namespace pk
