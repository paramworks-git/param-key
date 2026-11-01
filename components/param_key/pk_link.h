// The remote's end of the secured link (docs/protocol.md, "Security" and
// "Pairing"): the paired bridge, key and channel in flash, counters across deep
// sleep, and pairing with its screens. Board-independent.
//
// Pairing: an unpaired remote (or any, with home + back held for 5 s)
// broadcasts P with a fresh X25519 public key, moving through channels 1-13. HA
// answers through a bridge with A. Both derive the key and a 6-digit code; the
// remote shows the code, the user confirms it in HA, and HA gives the bridge the
// key. The remote then sends hellos under the new key until the bridge answers
// one, and only that answer makes it keep the key, bridge and channel.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_PARAM_KEY_REMOTE  // param_key: role: remote

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "esp_attr.h"
#include "esp_wifi.h"
#include "pk_crypto.h"
#include "pk_protocol.h"
#include "pk_store.h"
#include "pk_ui.h"

namespace pk {

// In flash: what pairing settled, and the send counter's lease.
struct LinkStore {
  uint8_t key[crypto::KEY_LEN];
  uint8_t bridge[6];
  uint8_t channel;
  uint8_t paired;
  uint32_t ceiling;
};

// In RTC memory: the counters, which deep sleep keeps and a reset clears.
struct LinkRtc {
  uint32_t magic;
  uint32_t tx_next;
  uint32_t tx_ceiling;
  uint64_t rx_floor;  // the bridge's next acceptable sequence (epoch, counter)
  uint8_t rx_bridge[6];
};

constexpr uint32_t LINK_RTC_MAGIC = 0x504B4C33;
static RTC_DATA_ATTR LinkRtc link_rtc;

constexpr uint32_t PAIR_STEP_MS = 250;        // dwell per channel while scanning
constexpr uint32_t PAIR_HELLO_MS = 1000;      // hello interval while waiting for confirmation
constexpr uint32_t PAIR_TIMEOUT_MS = 5 * 60 * 1000;
constexpr uint32_t REPAIR_HOLD_MS = 5000;     // home + back
constexpr uint32_t NO_ANSWER_MS = 4000;
constexpr uint8_t MAX_CHANNEL = 13;
constexpr uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

class Link {
 public:
  using SendFn = std::function<void(const uint8_t *mac, const std::vector<uint8_t> &frame)>;
  using MacFn = std::function<void(const uint8_t *mac)>;
  using ChannelFn = std::function<void(uint8_t channel)>;

  SendFn send;
  MacFn add_peer;
  ChannelFn set_channel;
  const char *firmware{""};

  bool pairing{false};

  // At boot, once ESP-NOW is up and the callbacks are set.
  void begin(uint32_t now) {
    this->store_.load(0x504B5210);
    esp_wifi_get_mac(WIFI_IF_STA, this->own_mac_);
    const LinkStore &s = this->store_.value;
    if (link_rtc.magic == LINK_RTC_MAGIC) {
      this->tx_.next = link_rtc.tx_next;
      this->tx_.ceiling = link_rtc.tx_ceiling;
      if (memcmp(link_rtc.rx_bridge, s.bridge, 6) == 0)
        this->rx_.floor = link_rtc.rx_floor;
    } else {
      this->tx_.resume(s.ceiling);
    }
    this->boot_ms_ = now;
    if (this->paired()) {
      this->set_channel(s.channel);
      this->add_peer(s.bridge);
      ESP_LOGI("param_key", "Paired with bridge %s on channel %u", format_mac(s.bridge).c_str(), s.channel);
    } else {
      this->start_pairing(now);
    }
  }

  bool paired() const { return this->store_.value.paired == 1; }

  // Key events: to the paired bridge. An unpaired remote starts pairing instead.
  void send_key(const std::vector<uint8_t> &plain, uint32_t now) {
    if (!this->paired()) {
      if (!this->pairing)
        this->start_pairing(now);
      return;
    }
    this->send_sealed_(this->store_.value.key, this->store_.value.bridge, plain);
  }

  // Hello: to the paired bridge, or while waiting for pairing to be confirmed,
  // under the new key to the bridge that answered.
  void send_hello(const std::vector<uint8_t> &plain) {
    if (this->answered_)
      this->send_sealed_(this->cand_key_, this->cand_bridge_, plain);
    else if (this->paired())
      this->send_sealed_(this->store_.value.key, this->store_.value.bridge, plain);
  }

  // A frame from src. Returns true with the plaintext packet if it is an
  // authentic, new packet from our bridge; pairing answers are handled here.
  bool receive(const uint8_t *src, const uint8_t *data, size_t size, uint32_t now, std::vector<uint8_t> &plain) {
    char type = packet_type(data, size);
    if (type == 0)
      return false;
    if (type == 'A') {
      this->on_answer_(src, data, size, now);
      return false;
    }
    if (is_plain_type(type))
      return false;
    uint64_t seq;
    if (this->answered_ && memcmp(src, this->cand_bridge_, 6) == 0 &&
        crypto::open(this->cand_key_, src, data, size, seq, plain)) {
      this->commit_(seq, now);
      this->last_rx_ = now;
      return true;
    }
    const LinkStore &s = this->store_.value;
    if (!this->paired() || memcmp(src, s.bridge, 6) != 0 || !crypto::open(s.key, src, data, size, seq, plain) ||
        !this->rx_.accept(seq))
      return false;
    this->save_rtc_();
    this->last_rx_ = now;
    return true;
  }

  void start_pairing(uint32_t now) {
    if (!crypto::make_keypair(this->kp_, esp_rng, nullptr)) {
      ESP_LOGE("param_key", "Pairing: no key pair");
      return;
    }
    this->pairing = true;
    this->answered_ = false;
    this->pairing_since_ = now;
    this->last_step_ = 0;
    ESP_LOGI("param_key", "Pairing: looking for a bridge");
    show_status("Pairing", "Pair with Home Assistant",
                "Open Settings > Devices & services in Home Assistant: it shows this remote as discovered.", now);
  }

  // Call often (every 20 ms is fine): scan, re-send, time out, and watch for
  // home + back held to pair again.
  void tick(uint32_t now, uint32_t home_held, uint32_t back_held) {
    // Also while pairing: starts over if someone else's answer got in first
    // (the codes wouldn't match). Once per hold.
    bool combo = home_held >= REPAIR_HOLD_MS && back_held >= REPAIR_HOLD_MS;
    if (combo && !this->combo_latched_) {
      this->combo_latched_ = true;
      this->start_pairing(now);
      return;
    }
    if (home_held == 0 || back_held == 0)
      this->combo_latched_ = false;
    if (!this->pairing) {
      if (this->paired() && !this->warned_ && this->last_rx_ == 0 && now - this->boot_ms_ >= NO_ANSWER_MS) {
        this->warned_ = true;
        show_status("PARAM KEY", "No answer from the bridge",
                    "Is the bridge on? If this remote was removed from Home Assistant, hold home and back for "
                    "5 seconds to pair it again.",
                    now);
      }
      return;
    }
    if (now - this->pairing_since_ >= PAIR_TIMEOUT_MS) {
      this->stop_pairing_();
      show_status("Pairing", "Pairing stopped", this->paired() ? "Still paired as before." :
                  "Press any key to try again.", now);
      return;
    }
    if (!this->answered_) {
      if (now - this->last_step_ < PAIR_STEP_MS)
        return;
      this->last_step_ = now;
      this->channel_ = this->channel_ % MAX_CHANNEL + 1;
      this->set_channel(this->channel_);
      auto p = header('P');
      p.insert(p.end(), this->kp_.pub, this->kp_.pub + crypto::PUB_LEN);
      p.insert(p.end(), this->firmware, this->firmware + strlen(this->firmware));
      this->send(BROADCAST, p);
    } else if (now - this->last_step_ >= PAIR_HELLO_MS) {
      this->last_step_ = now;
      this->hello_due = true;
    }
  }

  // Set by tick() while waiting for confirmation: the caller sends a hello.
  bool hello_due{false};

  static void show_status(const std::string &mode, const std::string &title, const std::string &subtitle,
                          uint32_t now) {
    ui.handle('S', {mode, title, subtitle, "-1", ""}, now);
    ui.handle('B', {"", "", "", ""}, now);
  }

 protected:
  void send_sealed_(const uint8_t *key, const uint8_t *dest, const std::vector<uint8_t> &plain) {
    uint32_t counter;
    auto persist = [this](uint32_t ceiling) {
      this->store_.value.ceiling = ceiling;
      return this->store_.save();
    };
    if (!this->tx_.take(counter, persist)) {
      ESP_LOGE("param_key", "Couldn't save the send counter; not sending");
      return;
    }
    this->save_rtc_();
    // Remotes send under epoch 0: their counter alone never repeats (SendCounter).
    this->send(dest, crypto::seal(key, this->own_mac_, 0, counter, plain));
  }

  void on_answer_(const uint8_t *src, const uint8_t *data, size_t size, uint32_t now) {
    if (!this->pairing || this->answered_ || size < 3 + crypto::PUB_LEN)
      return;
    uint8_t shared[32];
    crypto::Pairing p;
    bool ok = crypto::x25519(this->kp_.priv, data + 3, shared, esp_rng, nullptr) &&
              crypto::derive_pairing(shared, this->kp_.pub, data + 3, this->own_mac_, p);
    crypto::wipe(shared, sizeof(shared));
    if (!ok)
      return;
    memcpy(this->cand_key_, p.key, crypto::KEY_LEN);
    memcpy(this->cand_bridge_, src, 6);
    this->cand_channel_ = this->channel_;
    this->answered_ = true;
    this->last_step_ = now;
    char code[8];
    snprintf(code, sizeof(code), "%03u %03u", static_cast<unsigned>(p.code / 1000), static_cast<unsigned>(p.code % 1000));
    crypto::wipe(&p, sizeof(p));
    this->add_peer(src);
    ESP_LOGI("param_key", "Pairing: answer from %s on channel %u", format_mac(src).c_str(), this->channel_);
    show_status("Pairing", std::string("Code ") + code,
                "Check that Home Assistant shows the same code, then confirm it there.", now);
  }

  void commit_(uint64_t seq, uint32_t now) {
    LinkStore &s = this->store_.value;
    memcpy(s.key, this->cand_key_, crypto::KEY_LEN);
    memcpy(s.bridge, this->cand_bridge_, 6);
    s.channel = this->cand_channel_;
    s.paired = 1;
    this->store_.save();
    this->rx_.floor = seq + 1;
    this->save_rtc_();
    this->stop_pairing_();
    ESP_LOGI("param_key", "Paired with bridge %s on channel %u", format_mac(s.bridge).c_str(), s.channel);
    // Until Home Assistant draws a screen, don't leave the code showing.
    show_status("PARAM KEY", "Paired", "Waiting for Home Assistant", now);
  }

  void stop_pairing_() {
    this->pairing = false;
    this->answered_ = false;
    this->hello_due = false;
    crypto::wipe(&this->kp_, sizeof(this->kp_));
    crypto::wipe(this->cand_key_, sizeof(this->cand_key_));
    if (this->paired())
      this->set_channel(this->store_.value.channel);
  }

  void save_rtc_() {
    link_rtc.magic = LINK_RTC_MAGIC;
    link_rtc.tx_next = this->tx_.next;
    link_rtc.tx_ceiling = this->tx_.ceiling;
    link_rtc.rx_floor = this->rx_.floor;
    memcpy(link_rtc.rx_bridge, this->store_.value.bridge, 6);
  }

  Stored<LinkStore> store_;
  crypto::SendCounter tx_;
  crypto::ReplayFloor rx_;
  uint8_t own_mac_[6]{};
  uint32_t boot_ms_{0};
  uint32_t last_rx_{0};
  bool warned_{false};
  bool combo_latched_{false};

  crypto::KeyPair kp_{};
  bool answered_{false};
  uint8_t cand_key_[crypto::KEY_LEN]{};
  uint8_t cand_bridge_[6]{};
  uint8_t cand_channel_{1};
  uint8_t channel_{0};
  uint32_t pairing_since_{0};
  uint32_t last_step_{0};
};

inline Link link;

}  // namespace pk

#endif  // USE_PARAM_KEY_REMOTE
