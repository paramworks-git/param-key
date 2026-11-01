// Key state machine for the remote: turns key down/up into press, tap, hold,
// repeat and release events (docs/protocol.md). Board-independent.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_PARAM_KEY_REMOTE  // param_key: role: remote

#include <functional>
#include <string>
#include <vector>

#include "esp_sleep.h"
#include "pk_protocol.h"

namespace pk {

constexpr uint32_t HOLD_MS = 500;
constexpr uint32_t REPEAT_MS = 150;
constexpr size_t MAX_ACTIVE_KEYS = 4;

class Keys {
 public:
  using SendFn = std::function<void(const std::vector<uint8_t> &)>;

  void set_sender(SendFn send) { this->send_ = std::move(send); }

  void down(const char *name, uint32_t now) {
    this->last_activity = now;
    for (auto &k : this->active_)
      if (k.name == nullptr) {
        k = {name, now, now, false};
        this->emit_(PRESS, name);
        return;
      }
  }

  void up(const char *name, uint32_t now) {
    this->last_activity = now;
    for (auto &k : this->active_)
      if (k.name != nullptr && strcmp(k.name, name) == 0) {
        if (!k.held)
          this->emit_(TAP, name);
        this->emit_(RELEASE, name);
        k.name = nullptr;
        return;
      }
  }

  // Call from a short interval (e.g. 20 ms) to generate hold and repeat.
  void tick(uint32_t now) {
    for (auto &k : this->active_) {
      if (k.name == nullptr)
        continue;
      if (!k.held && now - k.down_at >= HOLD_MS) {
        k.held = true;
        k.last_sent = now;
        this->last_activity = now;
        this->emit_(HOLD, k.name);
      } else if (k.held && now - k.last_sent >= REPEAT_MS) {
        k.last_sent = now;
        this->last_activity = now;
        this->emit_(REPEAT, k.name);
      }
    }
  }

  // How long the key has been down, 0 if it isn't.
  uint32_t held_for(const char *name, uint32_t now) const {
    for (const auto &k : this->active_)
      if (k.name != nullptr && strcmp(k.name, name) == 0)
        return now - k.down_at;
    return 0;
  }

  bool any_down() const {
    for (const auto &k : this->active_)
      if (k.name != nullptr)
        return true;
    return false;
  }

  uint32_t last_activity{0};

 protected:
  struct Active {
    const char *name{nullptr};
    uint32_t down_at{0};
    uint32_t last_sent{0};
    bool held{false};
  };

  void emit_(KeyEvent event, const char *name) {
    auto packet = header('K');
    packet.push_back(this->seq_++);
    packet.push_back(event);
    packet.insert(packet.end(), name, name + strlen(name));
    if (this->send_)
      this->send_(packet);
  }

  Active active_[MAX_ACTIVE_KEYS];
  uint8_t seq_{0};
  SendFn send_;
};

inline Keys keys;
// Board wake diagnostics sent in the hello (docs/protocol.md): bit 0 the
// accelerometer answered, bit 1 its interrupt line was high (idle) at boot.
inline uint8_t wake_diag{0};

}  // namespace pk

#endif  // USE_PARAM_KEY_REMOTE
