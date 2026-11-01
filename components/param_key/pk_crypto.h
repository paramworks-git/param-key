// Link security for the remote ⇄ bridge protocol (docs/protocol.md, "Security"):
// AES-128-CCM frames under a per-remote key, replay counters, and X25519 pairing.
// Uses mbedtls only (no ESPHome), so it also builds on a host for the tests in
// firmware/tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "mbedtls/ccm.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"

#include "pk_protocol.h"

namespace pk {
namespace crypto {

constexpr size_t KEY_LEN = 16;
constexpr size_t PUB_LEN = 32;
constexpr size_t EPOCH_LEN = 3;
constexpr size_t CTR_LEN = 4;
constexpr size_t TAG_LEN = 8;
constexpr size_t NONCE_LEN = 13;
constexpr size_t HDR_LEN = 3;
// A sealed frame: header, epoch, counter, ciphertext (as long as the body), tag.
constexpr size_t AAD_LEN = HDR_LEN + EPOCH_LEN + CTR_LEN;
constexpr size_t OVERHEAD = AAD_LEN + TAG_LEN;
// Epochs are 24-bit (docs/protocol.md, "Security").
constexpr uint32_t MAX_EPOCH = 0xFFFFFF;
static_assert(OVERHEAD == FRAME_OVERHEAD, "keep pk_protocol.h in step");

// Random source, mbedtls style: fills buf, returns 0.
using Rng = int (*)(void *ctx, unsigned char *buf, size_t len);

inline void wipe(void *p, size_t n) {
  volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
  while (n--)
    *v++ = 0;
}

inline uint32_t get_le32(const uint8_t *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

inline void put_le32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]) {
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len, data, len, out) == 0;
}

// HKDF-SHA256 (RFC 5869), one output block (out_len <= 32). ESP-IDF builds
// mbedtls without MBEDTLS_HKDF_C by default, hence this.
inline bool hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                        const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len) {
  if (out_len > 32)
    return false;
  uint8_t prk[32], okm[32];
  std::vector<uint8_t> t(info, info + info_len);
  t.push_back(1);
  bool ok = hmac_sha256(salt, salt_len, ikm, ikm_len, prk) && hmac_sha256(prk, sizeof(prk), t.data(), t.size(), okm);
  if (ok)
    memcpy(out, okm, out_len);
  wipe(prk, sizeof(prk));
  wipe(okm, sizeof(okm));
  return ok;
}

// X25519 (RFC 7748): out = scalar * u, or scalar * base point if u is null.
// Fails on a low-order peer key (all-zero result).
inline bool x25519(const uint8_t scalar[32], const uint8_t *u, uint8_t out[32], Rng rng, void *rng_ctx) {
  uint8_t k[32];
  memcpy(k, scalar, 32);
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point p, r;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&p);
  mbedtls_ecp_point_init(&r);
  size_t olen = 0;
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
            mbedtls_mpi_read_binary_le(&d, k, sizeof(k)) == 0 &&
            (u == nullptr ? mbedtls_ecp_copy(&p, &grp.G) : mbedtls_ecp_point_read_binary(&grp, &p, u, 32)) == 0 &&
            mbedtls_ecp_mul(&grp, &r, &d, &p, rng, rng_ctx) == 0 &&
            mbedtls_ecp_point_write_binary(&grp, &r, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, out, 32) == 0 &&
            olen == 32;
  mbedtls_ecp_point_free(&r);
  mbedtls_ecp_point_free(&p);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  wipe(k, sizeof(k));
  uint8_t any = 0;
  for (int i = 0; ok && i < 32; i++)
    any |= out[i];
  return ok && any != 0;
}

struct KeyPair {
  uint8_t priv[32];
  uint8_t pub[PUB_LEN];
};

inline bool make_keypair(KeyPair &kp, Rng rng, void *rng_ctx) {
  return rng(rng_ctx, kp.priv, sizeof(kp.priv)) == 0 && x25519(kp.priv, nullptr, kp.pub, rng, rng_ctx);
}

// What pairing yields on both ends: the link key, and the 6-digit code the
// remote's screen and Home Assistant both show, so the user can see that no one
// sat in the middle of the exchange.
struct Pairing {
  uint8_t key[KEY_LEN];
  uint32_t code;
};

constexpr char PAIR_SALT[] = "param-key pair v2";

inline bool derive_pairing(const uint8_t shared[32], const uint8_t remote_pub[PUB_LEN], const uint8_t ha_pub[PUB_LEN],
                           const uint8_t remote_mac[6], Pairing &out) {
  uint8_t info[2 * PUB_LEN + 6];
  memcpy(info, remote_pub, PUB_LEN);
  memcpy(info + PUB_LEN, ha_pub, PUB_LEN);
  memcpy(info + 2 * PUB_LEN, remote_mac, 6);
  uint8_t okm[KEY_LEN + 4];
  if (!hkdf_sha256(reinterpret_cast<const uint8_t *>(PAIR_SALT), sizeof(PAIR_SALT) - 1, shared, 32, info,
                   sizeof(info), okm, sizeof(okm)))
    return false;
  memcpy(out.key, okm, KEY_LEN);
  uint32_t c = static_cast<uint32_t>(okm[KEY_LEN]) << 24 | okm[KEY_LEN + 1] << 16 | okm[KEY_LEN + 2] << 8 |
               okm[KEY_LEN + 3];
  out.code = c % 1000000;
  wipe(okm, sizeof(okm));
  return true;
}

// A frame's position in its sender's sequence: epoch in the high bits, counter
// in the low 32. Receivers require it to increase.
inline uint64_t sequence(uint32_t epoch, uint32_t counter) { return static_cast<uint64_t>(epoch) << 32 | counter; }

// Nonce: sender MAC, epoch (24-bit LE), counter (LE). The sender's MAC keeps the
// remote's and each bridge's sequences apart under the shared key; the epoch
// keeps a bridge's sequences apart across reflashes (HA gives it a new, higher
// one each time it connects).
inline void make_nonce(const uint8_t sender_mac[6], uint32_t epoch, uint32_t counter, uint8_t out[NONCE_LEN]) {
  memcpy(out, sender_mac, 6);
  out[6] = static_cast<uint8_t>(epoch);
  out[7] = static_cast<uint8_t>(epoch >> 8);
  out[8] = static_cast<uint8_t>(epoch >> 16);
  put_le32(out + 9, counter);
}

// Seals a plaintext packet (3-byte header + body, as header() and
// text_packet() build it). The header, epoch and counter are authenticated, the
// body encrypted. Returns an empty vector on failure.
inline std::vector<uint8_t> seal(const uint8_t key[KEY_LEN], const uint8_t sender_mac[6], uint32_t epoch,
                                 uint32_t counter, const std::vector<uint8_t> &plain) {
  if (plain.size() < HDR_LEN || epoch > MAX_EPOCH)
    return {};
  size_t body = plain.size() - HDR_LEN;
  std::vector<uint8_t> frame(OVERHEAD + body);
  memcpy(frame.data(), plain.data(), HDR_LEN);
  uint8_t nonce[NONCE_LEN];
  make_nonce(sender_mac, epoch, counter, nonce);
  memcpy(frame.data() + HDR_LEN, nonce + 6, EPOCH_LEN + CTR_LEN);
  mbedtls_ccm_context ccm;
  mbedtls_ccm_init(&ccm);
  uint8_t *ct = frame.data() + AAD_LEN;
  bool ok = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, key, 8 * KEY_LEN) == 0 &&
            mbedtls_ccm_encrypt_and_tag(&ccm, body, nonce, NONCE_LEN, frame.data(), AAD_LEN,
                                        plain.data() + HDR_LEN, ct, ct + body, TAG_LEN) == 0;
  mbedtls_ccm_free(&ccm);
  if (!ok)
    frame.clear();
  return frame;
}

// Checks and decrypts a sealed frame from sender_mac. On success, plain is the
// packet as seal() took it, and seq the frame's sequence(epoch, counter) (the
// caller checks it against its replay floor).
inline bool open(const uint8_t key[KEY_LEN], const uint8_t sender_mac[6], const uint8_t *frame, size_t size,
                 uint64_t &seq, std::vector<uint8_t> &plain) {
  if (size < OVERHEAD || frame[0] != MAGIC || frame[1] != VERSION)
    return false;
  size_t body = size - OVERHEAD;
  uint32_t epoch = frame[HDR_LEN] | frame[HDR_LEN + 1] << 8 | frame[HDR_LEN + 2] << 16;
  uint32_t counter = get_le32(frame + HDR_LEN + EPOCH_LEN);
  seq = sequence(epoch, counter);
  uint8_t nonce[NONCE_LEN];
  make_nonce(sender_mac, epoch, counter, nonce);
  plain.assign(frame, frame + HDR_LEN);
  plain.resize(HDR_LEN + body);
  const uint8_t *ct = frame + AAD_LEN;
  mbedtls_ccm_context ccm;
  mbedtls_ccm_init(&ccm);
  bool ok = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, key, 8 * KEY_LEN) == 0 &&
            mbedtls_ccm_auth_decrypt(&ccm, body, nonce, NONCE_LEN, frame, AAD_LEN, ct,
                                     plain.data() + HDR_LEN, ct + body, TAG_LEN) == 0;
  mbedtls_ccm_free(&ccm);
  if (!ok)
    plain.clear();
  return ok;
}

// The remote's send counter (epoch 0): never repeats under one key, across deep
// sleep, resets and crashes. The running value survives deep sleep (RTC memory);
// a ceiling is leased ahead in flash, and a cold boot resumes from it, skipping
// what the previous run may have used. (A remote whose flash is erased loses its
// key too, and pairs again under a new one.)
struct SendCounter {
  static constexpr uint32_t LEASE = 4096;
  uint32_t next{0};
  uint32_t ceiling{0};

  // After a reset: everything below the stored ceiling may have been used.
  void resume(uint32_t stored_ceiling) {
    this->next = stored_ceiling;
    this->ceiling = stored_ceiling;
  }

  // persist(new_ceiling) must write it to flash before returning true.
  template<typename Persist> bool take(uint32_t &out, Persist persist) {
    if (this->next >= this->ceiling) {
      uint32_t c = this->next + LEASE;
      if (c < this->next || !persist(c))
        return false;
      this->ceiling = c;
    }
    out = this->next++;
    return true;
  }
};

// Receive side: accepts only sequences at or above the floor, and moves the
// floor past each accepted one, so a recorded frame can't be played again.
struct ReplayFloor {
  uint64_t floor{0};

  bool accept(uint64_t seq) {
    if (seq < this->floor || seq == UINT64_MAX)
      return false;
    this->floor = seq + 1;
    return true;
  }
};

// The bridge's send sequence. Home Assistant gives it a new, higher epoch each
// time it connects, and the counter starts over under it, so nothing needs to be
// kept in flash and an erased, reflashed bridge can't repeat a nonce: the remotes
// it talks to would otherwise reject it, or worse, see a reused nonce.
struct EpochCounter {
  uint32_t epoch{0};
  uint32_t next{0};
  bool started{false};

  // A lower or equal epoch is refused (a replayed or stale provisioning call).
  bool start(uint32_t new_epoch) {
    if (new_epoch == 0 || new_epoch > MAX_EPOCH || (this->started && new_epoch <= this->epoch))
      return false;
    this->epoch = new_epoch;
    this->next = 0;
    this->started = true;
    return true;
  }

  bool take(uint32_t &epoch, uint32_t &counter) {
    if (!this->started || this->next == UINT32_MAX)
      return false;
    epoch = this->epoch;
    counter = this->next++;
    return true;
  }
};

}  // namespace crypto
}  // namespace pk
