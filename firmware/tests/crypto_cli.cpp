// Command-line wrapper around components/param_key/pk_crypto.h and the IR codec in
// components/param_key/pk_protocol.h, driven by test_crypto.py. Hex in, hex out.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../components/param_key/pk_crypto.h"

using namespace pk;

static int urandom(void *, unsigned char *buf, size_t len) {
  FILE *f = fopen("/dev/urandom", "rb");
  size_t n = f ? fread(buf, 1, len, f) : 0;
  if (f)
    fclose(f);
  return n == len ? 0 : -1;
}

static std::vector<uint8_t> unhex(const std::string &s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<uint8_t>(strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

static std::string hex(const uint8_t *p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02x", p[i]);
    s += b;
  }
  return s;
}

static int fail() {
  puts("FAIL");
  return 1;
}

static int self_test() {
  // SendCounter: persists before handing out a counter past the ceiling, and
  // resuming from the stored ceiling never repeats one.
  crypto::SendCounter c;
  uint32_t stored = 0, v = 0, last = 0;
  int writes = 0;
  auto persist = [&](uint32_t ceiling) {
    stored = ceiling;
    writes++;
    return true;
  };
  for (int i = 0; i < 10000; i++) {
    if (!c.take(v, persist) || (i > 0 && v != last + 1) || v >= stored)
      return fail();
    last = v;
  }
  if (writes != 3)
    return fail();
  crypto::SendCounter after_reset;
  after_reset.resume(stored);
  if (!after_reset.take(v, persist) || v <= last)
    return fail();
  // A failed flash write hands out nothing.
  crypto::SendCounter d;
  d.resume(10);
  if (d.take(v, [](uint32_t) { return false; }) || d.next != 10)
    return fail();
  // ReplayFloor: strictly increasing sequences, gaps allowed; a later epoch beats
  // any counter of an earlier one, and an earlier epoch is never accepted again.
  using crypto::sequence;
  crypto::ReplayFloor f;
  if (!f.accept(0) || f.accept(0) || !f.accept(5) || f.accept(3) || f.accept(5) || !f.accept(6))
    return fail();
  if (!f.accept(sequence(2, 0)) || f.accept(sequence(1, 999999)) || f.accept(sequence(2, 0)) ||
      !f.accept(sequence(2, 1)) || !f.accept(sequence(3, 0)))
    return fail();
  crypto::ReplayFloor g{100};
  if (g.accept(99) || !g.accept(100) || g.accept(UINT64_MAX))
    return fail();
  // EpochCounter: nothing before an epoch; each new epoch must be higher and
  // restarts the counter; out-of-range epochs are refused.
  crypto::EpochCounter e;
  uint32_t ep, ct;
  if (e.take(ep, ct) || e.start(0) || e.start(crypto::MAX_EPOCH + 1))
    return fail();
  if (!e.start(7) || !e.take(ep, ct) || ep != 7 || ct != 0 || !e.take(ep, ct) || ct != 1)
    return fail();
  if (e.start(7) || e.start(6) || !e.start(8) || !e.take(ep, ct) || ep != 8 || ct != 0)
    return fail();
  puts("OK");
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return fail();
  std::string cmd = argv[1];
  auto arg = [&](int i) { return unhex(i < argc ? argv[i] : ""); };
  if (cmd == "selftest")
    return self_test();
  if (cmd == "pub" && argc == 3) {
    auto priv = arg(2);
    uint8_t out[32];
    if (priv.size() != 32 || !crypto::x25519(priv.data(), nullptr, out, urandom, nullptr))
      return fail();
    puts(hex(out, 32).c_str());
  } else if (cmd == "keypair") {
    crypto::KeyPair kp;
    if (!crypto::make_keypair(kp, urandom, nullptr))
      return fail();
    printf("%s %s\n", hex(kp.priv, 32).c_str(), hex(kp.pub, 32).c_str());
  } else if (cmd == "shared" && argc == 4) {
    auto priv = arg(2), pub = arg(3);
    uint8_t out[32];
    if (priv.size() != 32 || pub.size() != 32 || !crypto::x25519(priv.data(), pub.data(), out, urandom, nullptr))
      return fail();
    puts(hex(out, 32).c_str());
  } else if (cmd == "derive" && argc == 6) {
    auto shared = arg(2), rpub = arg(3), hpub = arg(4), mac = arg(5);
    crypto::Pairing p;
    if (shared.size() != 32 || rpub.size() != 32 || hpub.size() != 32 || mac.size() != 6 ||
        !crypto::derive_pairing(shared.data(), rpub.data(), hpub.data(), mac.data(), p))
      return fail();
    printf("%s %06u\n", hex(p.key, 16).c_str(), static_cast<unsigned>(p.code));
  } else if (cmd == "seal" && argc == 7) {
    auto key = arg(2), mac = arg(3), plain = arg(6);
    uint32_t epoch = strtoul(argv[4], nullptr, 10), ctr = strtoul(argv[5], nullptr, 10);
    if (key.size() != 16 || mac.size() != 6)
      return fail();
    auto frame = crypto::seal(key.data(), mac.data(), epoch, ctr, plain);
    if (frame.empty())
      return fail();
    puts(hex(frame.data(), frame.size()).c_str());
  } else if (cmd == "open" && argc == 5) {
    auto key = arg(2), mac = arg(3), frame = arg(4);
    uint64_t seq;
    std::vector<uint8_t> plain;
    if (key.size() != 16 || mac.size() != 6 || !crypto::open(key.data(), mac.data(), frame.data(), frame.size(), seq, plain))
      return fail();
    printf("%u %u %s\n", static_cast<unsigned>(seq >> 32), static_cast<unsigned>(seq & 0xFFFFFFFFu),
           hex(plain.data(), plain.size()).c_str());
  } else if (cmd == "ir" && argc >= 3) {
    std::vector<int32_t> t;
    for (int i = 3; i < argc; i++)
      t.push_back(atoi(argv[i]));
    auto p = ir_packet(strtoul(argv[2], nullptr, 10), t);
    if (p.empty())
      return fail();
    puts(hex(p.data(), p.size()).c_str());
  } else if (cmd == "parseir" && argc == 3) {
    auto p = arg(2);
    uint32_t carrier;
    std::vector<int32_t> t;
    if (!parse_ir(p.data(), p.size(), carrier, t))
      return fail();
    printf("%u", static_cast<unsigned>(carrier));
    for (auto v : t)
      printf(" %d", static_cast<int>(v));
    puts("");
  } else {
    return fail();
  }
  return 0;
}
