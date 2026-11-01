// Bridge-side state (docs/protocol.md): the remotes Home Assistant paired, their
// keys and replay floors, the bridge's send sequence, pairing replies waiting for
// their remote, and the cached screen packets for answering hello.
//
// Nothing is kept in flash. Each time Home Assistant connects it sends a new
// epoch (set_epoch) and the keys (set_remote), so a bridge that just booted
// accepts nothing and sends nothing until HA has given it the keys and the
// counters it last saw, a stolen bridge holds no keys, and an erased, reflashed
// bridge can't repeat a nonce.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_PARAM_KEY_BRIDGE  // param_key: role: bridge

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "esp_wifi.h"
#include "pk_crypto.h"
#include "pk_protocol.h"

namespace pk {

using Mac = std::array<uint8_t, 6>;

inline Mac to_mac(const uint8_t *p) {
  Mac m;
  memcpy(m.data(), p, 6);
  return m;
}

struct Remote {
  Mac mac;
  uint8_t key[crypto::KEY_LEN];
  crypto::ReplayFloor rx;
  // Sent a valid frame since HA provisioned it: gets screen updates.
  bool heard{false};
  // HA's screen (S or L), hotkey (B) and settings (C) packets for this remote alone
  // (the *_for actions); empty = the shared one.
  std::array<std::vector<uint8_t>, 3> ui{};
};

// Which UI packet: the screen, the hotkey bar, the settings.
enum Ui : int { UI_SCREEN = 0, UI_HOTKEYS = 1, UI_CONFIG = 2 };

// HA's half of a pairing exchange, sent when the remote next asks (it scans
// channels, so it is only on ours now and then).
struct PendingPair {
  Mac mac;
  uint8_t remote_pub[crypto::PUB_LEN];
  uint8_t ha_pub[crypto::PUB_LEN];
  uint32_t until;
};

constexpr uint32_t PAIR_REPLY_MS = 5 * 60 * 1000;
constexpr uint32_t PAIR_EVENT_MS = 5000;

struct Bridge {
  std::vector<Remote> remotes;
  std::vector<PendingPair> pending;
  std::vector<std::pair<Mac, uint32_t>> pair_events;  // last pair-request event per remote
  // The last S or L, B and C packets for every remote (plaintext), answering hellos.
  std::array<std::vector<uint8_t>, 3> ui{};
  uint8_t own_mac[6]{};
  crypto::EpochCounter tx;

  // HA's packet for one remote (its MAC) or, with "", for all: that replaces any
  // remote's own. False if the MAC isn't a paired remote.
  bool set_ui(const std::string &remote, int which, std::vector<uint8_t> packet) {
    if (remote.empty()) {
      this->ui[which] = std::move(packet);
      for (auto &r : this->remotes)
        r.ui[which].clear();
      return true;
    }
    uint8_t mac[6];
    Remote *r = parse_mac(remote, mac) ? this->find(mac) : nullptr;
    if (r == nullptr)
      return false;
    r->ui[which] = std::move(packet);
    return true;
  }

  const std::vector<uint8_t> &ui_for(const Remote &r, int which) const {
    return r.ui[which].empty() ? this->ui[which] : r.ui[which];
  }

  Remote *find(const uint8_t *mac) {
    for (auto &r : this->remotes)
      if (memcmp(r.mac.data(), mac, 6) == 0)
        return &r;
    return nullptr;
  }

  void provision(const Mac &mac, const uint8_t key[crypto::KEY_LEN], uint32_t floor) {
    Remote *r = this->find(mac.data());
    if (r == nullptr) {
      this->remotes.emplace_back();
      r = &this->remotes.back();
      r->mac = mac;
    } else if (memcmp(r->key, key, crypto::KEY_LEN) != 0) {
      // Paired again: the new key starts its own counter history.
      r->rx.floor = 0;
    }
    memcpy(r->key, key, crypto::KEY_LEN);
    // Never lower a floor this bridge already moved past.
    if (floor > r->rx.floor)
      r->rx.floor = floor;
    this->drop_pending(mac.data());
  }

  bool forget(const uint8_t *mac) {
    this->drop_pending(mac);
    for (auto it = this->remotes.begin(); it != this->remotes.end(); ++it)
      if (memcmp(it->mac.data(), mac, 6) == 0) {
        crypto::wipe(it->key, sizeof(it->key));
        this->remotes.erase(it);
        return true;
      }
    return false;
  }

  // A frame from a provisioned remote: authentic and newer than the last one?
  // A MAC-layer retry repeats the counter, so this also drops duplicates.
  bool open(Remote &r, const uint8_t *data, size_t size, uint64_t &seq, std::vector<uint8_t> &plain) {
    if (!crypto::open(r.key, r.mac.data(), data, size, seq, plain) || !r.rx.accept(seq))
      return false;
    r.heard = true;
    return true;
  }

  // Empty until HA has sent an epoch.
  std::vector<uint8_t> seal_for(const Remote &r, const std::vector<uint8_t> &plain) {
    uint32_t epoch, counter;
    if (plain.empty() || !this->tx.take(epoch, counter))
      return {};
    return crypto::seal(r.key, this->own_mac, epoch, counter, plain);
  }

  void drop_pending(const uint8_t *mac) {
    for (auto it = this->pending.begin(); it != this->pending.end();)
      it = memcmp(it->mac.data(), mac, 6) == 0 ? this->pending.erase(it) : it + 1;
  }

  // HA's reply to this remote's pairing request, if HA gave one for this
  // public key and it hasn't expired.
  const PendingPair *pending_for(const uint8_t *mac, const uint8_t *remote_pub, uint32_t now) {
    for (auto it = this->pending.begin(); it != this->pending.end();) {
      if (static_cast<int32_t>(now - it->until) >= 0) {
        it = this->pending.erase(it);
        continue;
      }
      if (memcmp(it->mac.data(), mac, 6) == 0 && memcmp(it->remote_pub, remote_pub, crypto::PUB_LEN) == 0)
        return &*it;
      ++it;
    }
    return nullptr;
  }

  // Rate limit for pair-request events: the remote asks every few seconds.
  bool should_report_pairing(const uint8_t *mac, uint32_t now) {
    for (auto &e : this->pair_events)
      if (memcmp(e.first.data(), mac, 6) == 0) {
        if (now - e.second < PAIR_EVENT_MS)
          return false;
        e.second = now;
        return true;
      }
    if (this->pair_events.size() >= 16)
      this->pair_events.erase(this->pair_events.begin());
    this->pair_events.emplace_back(to_mac(mac), now);
    return true;
  }
};

inline Bridge bridge;

}  // namespace pk

#endif  // USE_PARAM_KEY_BRIDGE
