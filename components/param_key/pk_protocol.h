// PARAM KEY wire protocol (docs/protocol.md), shared by remote and bridge.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pk {

constexpr uint8_t MAGIC = 0x50;  // 'P'
constexpr uint8_t VERSION = 2;
constexpr char SEP = '\x1f';
// Text packets (screen, hotkeys) are cut to this before sealing.
constexpr size_t MAX_PACKET = 250;
// ESP-NOW v2 payload limit; both ends set espnow max_payload_size to it.
constexpr size_t MAX_FRAME = 1470;
// Sealed frame overhead: header 3, epoch 3, counter 4, tag 8 (pk_crypto.h).
constexpr size_t FRAME_OVERHEAD = 18;

enum KeyEvent : uint8_t { PRESS = 0, TAP = 1, HOLD = 2, REPEAT = 3, RELEASE = 4 };

inline const char *event_name(uint8_t event) {
  static const char *const NAMES[] = {"press", "tap", "hold", "repeat", "release"};
  return event <= RELEASE ? NAMES[event] : "unknown";
}

// esp_sleep_wakeup_cause_t as sent in the hello.
inline std::string wake_name(uint8_t cause) {
  static const char *const NAMES[] = {"reset", "all", "ext0", "ext1", "timer", "touchpad", "ulp", "gpio", "uart"};
  return cause < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[cause] : "cause " + std::to_string(cause);
}

inline std::vector<uint8_t> header(char type) { return {MAGIC, VERSION, static_cast<uint8_t>(type)}; }

// Pairing packets are the only ones sent in the clear (docs/protocol.md).
inline bool is_plain_type(char type) { return type == 'P' || type == 'A'; }

// Returns the packet type, or 0 if data isn't a v2 packet.
inline char packet_type(const uint8_t *data, size_t size) {
  if (size < 3 || data[0] != MAGIC || data[1] != VERSION)
    return 0;
  return static_cast<char>(data[2]);
}

// Text packet: header + fields joined by SEP, truncated to MAX_PACKET.
inline std::vector<uint8_t> text_packet(char type, const std::vector<std::string> &fields) {
  auto packet = header(type);
  for (size_t i = 0; i < fields.size(); i++) {
    if (i > 0)
      packet.push_back(SEP);
    packet.insert(packet.end(), fields[i].begin(), fields[i].end());
  }
  if (packet.size() > MAX_PACKET)
    packet.resize(MAX_PACKET);
  return packet;
}

inline std::vector<std::string> split_fields(const uint8_t *data, size_t size) {
  std::vector<std::string> fields(1);
  for (size_t i = 3; i < size; i++) {
    if (data[i] == SEP)
      fields.emplace_back();
    else
      fields.back().push_back(static_cast<char>(data[i]));
  }
  return fields;
}

// IR packet 'I': carrier (uint16 LE, units of 100 Hz, 0 = unmodulated), then
// timings as int16 LE in units of IR_UNIT_US: positive = carrier on (mark),
// negative = off (space). Returns an empty vector if the code doesn't fit.
constexpr int32_t IR_UNIT_US = 4;
constexpr size_t MAX_IR_TIMINGS = (MAX_FRAME - FRAME_OVERHEAD - 2) / 2;

inline std::vector<uint8_t> ir_packet(uint32_t carrier_hz, const std::vector<int32_t> &timings_us) {
  if (timings_us.empty() || timings_us.size() > MAX_IR_TIMINGS)
    return {};
  auto packet = header('I');
  uint32_t carrier = (carrier_hz + 50) / 100;
  packet.push_back(static_cast<uint8_t>(carrier));
  packet.push_back(static_cast<uint8_t>(carrier >> 8));
  for (int32_t t : timings_us) {
    int32_t mag = t < 0 ? -t : t;
    int32_t units = (mag + IR_UNIT_US / 2) / IR_UNIT_US;
    if (units < 1)
      units = 1;
    if (units > 32767)
      units = 32767;
    auto v = static_cast<uint16_t>(static_cast<int16_t>(t < 0 ? -units : units));
    packet.push_back(static_cast<uint8_t>(v));
    packet.push_back(static_cast<uint8_t>(v >> 8));
  }
  return packet;
}

// Parses an 'I' packet (header included) back to Hz and microseconds.
inline bool parse_ir(const uint8_t *data, size_t size, uint32_t &carrier_hz, std::vector<int32_t> &timings_us) {
  if (size < 7 || (size - 5) % 2 != 0)
    return false;
  carrier_hz = 100u * (data[3] | data[4] << 8);
  timings_us.clear();
  for (size_t i = 5; i + 1 < size; i += 2) {
    auto v = static_cast<int16_t>(data[i] | data[i + 1] << 8);
    if (v == 0)
      return false;
    timings_us.push_back(static_cast<int32_t>(v) * IR_UNIT_US);
  }
  return true;
}

inline bool parse_mac(const std::string &text, uint8_t out[6]) {
  unsigned int b[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
    return false;
  for (int i = 0; i < 6; i++)
    out[i] = static_cast<uint8_t>(b[i]);
  return true;
}

// Exactly 2 * len hex digits into out.
inline bool parse_hex(const std::string &text, uint8_t *out, size_t len) {
  if (text.size() != 2 * len)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned int b;
    if (sscanf(text.c_str() + 2 * i, "%2x", &b) != 1)
      return false;
    out[i] = static_cast<uint8_t>(b);
  }
  return true;
}

inline std::string to_hex(const uint8_t *data, size_t len) {
  static const char DIGITS[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < len; i++) {
    s.push_back(DIGITS[data[i] >> 4]);
    s.push_back(DIGITS[data[i] & 15]);
  }
  return s;
}

inline std::string format_mac(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

}  // namespace pk
