# Remote ⇄ bridge protocol (v2)

ESP-NOW between a remote and its bridge. Every packet starts with a 3-byte
header:

| Byte | Value |
|---|---|
| 0 | `0x50` (`'P'`), magic |
| 1 | protocol version, `2` |
| 2 | packet type (ASCII letter) |

Everything except the two pairing packets (`P`, `A`) is sealed (see
[Security](#security)): the header and a counter travel in the clear, the body
is encrypted and authenticated. The tables below describe the body as it is
before sealing.

Text fields are UTF-8, separated by `0x1F` (ASCII unit separator). Text packets
are cut to 250 bytes before sealing. Both ends use ESP-NOW v2 frames (up to 1470
bytes), which IR codes need.

## Remote → bridge

| Type | Name | Body |
|---|---|---|
| `H` | hello | `battery` (uint8, percent, 255 = unknown), `flags` (uint8, bit 0 = charging, bit 1 = wake diagnostics follow), if bit 1: `wake` (uint8, `esp_sleep_wakeup_cause_t`), `wake_mask` (uint32 LE, EXT1 wake status by GPIO), `diag` (uint8, bit 0 = accelerometer found, bit 1 = its interrupt line high at boot); then the firmware version as ASCII |
| `K` | key | `seq` (uint8, increments per event), `event` (uint8), then the key name as ASCII |
| `P` | pairing request (clear) | the remote's X25519 public key (32 bytes), then the firmware version as ASCII. Broadcast |

`H` is sent on every boot or wake, and again with each battery reading (about 2 s
after waking, then every 60 s while awake), which is how HA logs the battery. The
bridge replies straight away with its cached screen, hotkey and settings packets,
then `Y`, so the screen is right before the first key press.

### Key events

| Value | Event | Sent |
|---|---|---|
| 0 | `press` | key down, immediately |
| 1 | `tap` | key up, if released before the hold threshold (500 ms) |
| 2 | `hold` | once, when held for 500 ms |
| 3 | `repeat` | every 150 ms after `hold`, while held |
| 4 | `release` | key up, always (after `tap`, or ending a hold) |

HA picks which event a key reacts to:

- Instant keys (volume, d-pad, media) act on `press` and `repeat`.
- Dual-function keys (home: tap = home, hold = mode picker) act on `tap` and `hold`.

### Key names

`power`, `h1` `h2` `h3` `h4`, `up` `down` `left` `right` `ok`, `back` `home`
`settings`, `vol_down` `mute` `vol_up`, `rew` `play` `fwd` `stop`.

## Bridge → remote

| Type | Name | Body |
|---|---|---|
| `S` | status screen | `mode`, `title`, `subtitle`, `value` (integer 0–100, or -1 to hide the bar), `value_label` |
| `L` | list screen | `mode`, `title`, `cursor` (index into the items sent), then up to 6 items |
| `B` | hotkey bar | `h1`, `h2`, `h3`, `h4` labels (empty = unused) |
| `M` | message | `text`, `seconds` (overlay, hidden after this long) |
| `C` | settings | `brightness` (percent), `backlight` (seconds idle before the screen turns off), `sleep` (seconds idle before deep sleep), all decimal |
| `I` | IR code | `carrier` (uint16 LE, units of 100 Hz, 0 = unmodulated), then up to 725 timings, each int16 LE in units of 4 µs: positive = carrier on (mark), negative = off (space) |
| `Y` | hello acknowledged | empty. Tells the remote the bridge has its key |
| `A` | pairing answer (clear) | Home Assistant's X25519 public key (32 bytes) |

For long lists, HA sends only the visible window of items; the remote doesn't
scroll on its own. The bridge caches the last `S` or `L` packet, the last `B`
packet and the last `C` packet, for each remote (see the `*_for` actions), to answer `H`. The remote keeps the settings from
`C` in flash, so they apply from the first frame after a wake.

The remote sends an `I` code as soon as it arrives. The bridge never caches one: a
remote that is asleep misses it, rather than firing it when it next wakes.

## Security

Without this, anyone in radio range could send key presses that run HA actions,
or record one and play it back. Each remote has its own 128-bit key, set during
pairing.

**Sealed frame:**

| Bytes | Field |
|---|---|
| 0–2 | header (as above) |
| 3–5 | epoch (uint24 LE) |
| 6–9 | counter (uint32 LE) |
| 10… | body, encrypted (AES-128-CCM) |
| last 8 | tag |

- **Associated data** (authenticated, not encrypted): bytes 0–9.
- **Nonce** (13 bytes): the sender's MAC (6), the epoch (3), the counter (4), as
  in the frame. The remote and each bridge have their own MAC, so they never share
  a nonce under one key.
- **Sequence:** a frame's place in its sender's sequence is (epoch, counter),
  compared as one 56-bit number. Every frame takes the next one.
  - **Remote:** epoch 0. It keeps its counter in RTC memory across deep sleep, and
    leases ranges of 4096 ahead in flash; after a reset it continues from the end
    of the lease, so it never repeats a counter. Erasing its flash erases its key
    too, and it pairs again under a new one.
  - **Bridge:** keeps nothing in flash. Each time HA connects it sends the bridge a
    new epoch (`set_epoch`), higher than any before: HA stores the last one per
    bridge and never goes below the minutes since 2026-01-01, which also covers an
    HA restored from an older backup. The bridge's counter starts at 0 under each
    epoch, and it sends nothing until it has one. So a bridge that is erased and
    reflashed can't repeat a nonce, and its remotes accept it again at once.
- **Replay:** a receiver accepts a frame only if its sequence is higher than the
  last one it accepted from that sender. A recorded frame played back, or a
  MAC-layer retry, is dropped. ESP-NOW's own encryption isn't used: it can't
  encrypt broadcast, allows only 6 encrypted peers, and isn't documented to stop
  replay.
- **Keys:** Home Assistant holds them, with the last counter it saw from each
  remote. The bridge keeps them in RAM only. Each time HA connects
  (`param_key_bridge_online`), it calls `set_epoch`, then `set_remote` for every
  paired remote, so a bridge that just booted accepts nothing until it has the
  keys and the counters to start from. The remote keeps its key, its bridge and the
  channel in flash.

**Accepted risks:**

- After a remote is reset (not woken from sleep), it forgets the bridge's last
  sequence, so an old screen or IR packet could be played to it once. That shows
  an old screen, or sends an IR code someone could also just blast at the TV.
- Someone holding the remote can read its key from flash over USB. They could
  also just press its keys. Remove a lost remote in HA, which removes its key
  from the bridges.
- IR itself has no security: a TV can't tell the remote from a copy. Never use IR,
  or anything else unauthenticated, for locks, garage doors or alarms.

## Pairing

1. **Request.** An unpaired remote, or any remote with home and back held for 5 s,
   makes a fresh X25519 key pair and broadcasts `P` on channels 1–13 in turn,
   250 ms each. Its screen says to open Home Assistant.
2. **Discovery.** A bridge that hears `P` fires `param_key_pair_request` (at
   most every 5 s per remote). HA offers the remote as a discovered device.
3. **Answer.** When the user starts the flow, HA makes its own key pair and calls
   `pair_reply` on the bridge. The bridge sends `A` the next time that remote asks
   with the same public key (within 5 minutes). The remote stops scanning:
   that bridge's channel is its channel.
4. **Code.** Both sides compute the X25519 shared secret and run HKDF-SHA256
   (salt `"param-key pair v2"`, info = remote public key ‖ HA public key ‖
   remote MAC) to 20 bytes: bytes 0–15 are the key, bytes 16–19 (big-endian) mod
   1 000 000 the code. The remote shows the code, and HA asks the user to
   confirm it matches. A matching code means no one else took part in the exchange.
5. **Confirm.** HA stores the key and calls `set_remote` on its bridges. The
   remote sends `H` under the new key every second; the first authentic reply (`Y`
   or a cached screen) makes it save the key, bridge and channel to flash.

Pairing gives up after 5 minutes. A remote that is paired keeps its old pairing
until a new one completes. `docs/test-vectors.json` has a worked example of
every step, and `firmware/tests` checks the firmware against it.

## Bridge ⇄ Home Assistant

The bridge is an ordinary ESPHome device. It only fires events (`esphome.*`), which
HA accepts from any added ESPHome device, so "Allow the device to perform Home
Assistant actions" stays off.

Events fired by the bridge (only for authentic frames from paired remotes, apart
from the pairing request). Each also has `bridge`, the bridge's ESPHome node name:
its actions are `esphome.<bridge, with - as _>_<action>`, so HA answers through
the bridge that heard the remote.

| Event | Data |
|---|---|
| `esphome.param_key_key` | `remote` (MAC), `key`, `type` (event name), `rssi`, `counter` |
| `esphome.param_key_hello` | `remote`, `battery`, `charging`, `firmware`, `rssi`, `wake` (`reset`, `ext1`, ...), `wake_mask` (hex), `diag` (-1 if absent), `counter` |
| `esphome.param_key_pair_request` | `remote`, `public_key` (64 hex digits), `firmware`, `rssi` |
| `esphome.param_key_bridge_online` | only `bridge`: HA is connected again after the bridge booted or showed "Home Assistant offline" (no HA connection for 5 s). HA sends the remote keys and redraws the screen |

Actions the bridge exposes (called from HA as `esphome.<bridge_name>_<action>`):

| Action | Variables |
|---|---|
| `show_status` | `mode`, `title`, `subtitle`, `value` (int), `value_label` |
| `show_list` | `mode`, `title`, `cursor` (int), `items` (string list) |
| `set_hotkeys` | `labels` (string list, up to 4) |
| `show_message` | `message`, `seconds` (int) |
| `set_config` | `brightness`, `backlight_seconds`, `sleep_seconds` (int) |
| `show_status_for`, `show_list_for`, `set_hotkeys_for`, `set_config_for`, `show_message_for` | `remote` (MAC), then the same as above: for that remote only (bridge 0.3.0 and later) |
| `send_ir` | `remote` (MAC), `carrier` (Hz), `timings` (int list, µs, positive = mark) |
| `set_epoch` | `epoch` (1 to 16 777 215, higher than the bridge's current one): the bridge's send epoch, sent first each time HA connects |
| `set_remote` | `remote` (MAC), `key` (32 hex digits), `floor` (the lowest counter to accept: one more than the last HA saw, 0 for a new pairing) |
| `forget_remote` | `remote` |
| `pair_reply` | `remote`, `remote_public` (from the pair request), `ha_public` (64 hex digits each) |

The plain screen actions go to every paired remote the bridge has heard from; the `*_for`
twins go to one remote, and the bridge keeps that remote's own screen, hotkeys and settings
(the integration uses them, so each remote has its own menu). A plain action replaces what each
remote had of its own. A sleeping remote misses an update, then gets the cached state on its
next `H`; messages aren't cached.

## Radio channel

ESP-NOW has to use the same Wi-Fi channel as the bridge, which follows its access
point. The remote finds the channel while pairing and keeps it. If the access
point moves to another channel, the remote stops reaching the bridge until it is
paired again, so **fix the access point's 2.4 GHz channel** rather than using
auto. Scanning for the channel on every wake is planned.
