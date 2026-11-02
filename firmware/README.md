# PARAM KEY firmware

ESPHome firmware for the PARAM KEY remote and its bridge. The remote (ESP32-S3, 20 keys, a 2.0"
screen) talks ESP-NOW to the bridge, an ESP32 on your Wi-Fi. Home Assistant does all the device
logic: the remote reports key presses and shows what HA tells it to, so remapping a key never
needs a reflash. The Home Assistant side is the PARAM KEY integration
([param-key-homeassistant](https://github.com/paramworks-git/param-key-homeassistant)).

**To set one up, you don't need this folder:** the [install page](https://paramworks-git.github.io/param-key/)
flashes the bridge from your browser, and the [README](../README.md#set-up) walks through the
rest. This is for building and changing the firmware. MIT licensed (see [LICENSE](../LICENSE)).

## How it works

```
 remote (ESP32-S3, LiPo)              bridge (any ESP32, mains)           Home Assistant
 ───────────────────────              ─────────────────────────           ──────────────
 key press  ──── ESP-NOW 'K' ────▶    fires event               ──API──▶  param_key_key
 boot/wake  ──── ESP-NOW 'H' ────▶    replies with cached screen          (PARAM KEY integration)
 battery    ──── (in the hello) ──▶   fires event               ──API──▶  param_key_hello
 screen     ◀─── ESP-NOW 'S/L/B/M' ── show_status / show_list  ◀──API───
 IR LED     ◀─── ESP-NOW 'I' ──────── send_ir                  ◀──API───  param_key.send_ir
```

Every packet except the pairing exchange is encrypted and authenticated with the remote's own
key, and a counter stops recorded packets from being played back. The bridge only hears remotes
paired in Home Assistant. Packet formats, pairing and event fields: [docs/protocol.md](../docs/protocol.md).

- **Remote:** deep-sleeps when idle. Picking it up (accelerometer) or any key except settings and
  stop wakes it in well under a second. It has no Wi-Fi: ESP-NOW has no association step, so a
  key press reaches HA right after wake.
- **Bridge:** joins Wi-Fi and HA's API, turns remote packets into HA events, exposes the actions
  HA calls, and caches the last screen so a waking remote is up to date at once. Use a dedicated
  board, not one that is also a Bluetooth proxy: the radio would time-slice between Wi-Fi and
  BLE, and the bridge has to listen to ESP-NOW all the time.
- **Channel:** ESP-NOW follows the bridge's access point channel. The remote finds it while
  pairing and keeps it, so fix the access point's 2.4 GHz channel (1, 6 or 11) rather than auto.

## Layout

| Path | What |
|---|---|
| `bridge.yaml` | The bridge, as released: one file, any ESP32 (pass `board`) |
| `remote-v1.yaml` | The remote, PCB v1 |
| `common/remote_core.yaml`, `common/key_gpio.yaml` | Board-independent remote logic, one key |
| `../components/param_key/` | ESPHome external component: the link, crypto, pairing and screen helpers (C++), and the mbedtls options they need |
| `tests/` | Host tests: link crypto and IR codec against a Python reference |

Both configs load the component from a tagged release on GitHub (`pk_component`); `build.sh`
points them at this checkout instead. Version: `version` in `bridge.yaml` and
`common/remote_core.yaml`, which must match the release tag.

## Build

Everything runs in Docker, with a pinned ESPHome. If the Docker daemon runs on another host, set
`HOST_DIR` to the repository root as that host sees it.

```sh
./build.sh config bridge.yaml                                  # validate
./build.sh compile bridge.yaml                                 # classic ESP32
./build.sh -s board esp32-s3-devkitc-1 compile bridge.yaml     # or esp32-c3-devkitm-1
./build.sh compile remote-v1.yaml
./tests/run.sh                                                 # crypto and IR codec
```

The flashable image is `.esphome/build/<name>/build/firmware.factory.bin` (bootloader and
partition table included, for a blank chip). Flash it with `./build.sh run …` over USB, or at
https://web.esphome.io.

If a remote won't connect over USB, put it in the ROM bootloader: hold **home** (GPIO0) while
pressing reset or plugging in USB, and replug it after flashing.

**Your own bridge config.** In ESPHome Builder, a bridge from the install page offers **Take
control**: it imports `bridge.yaml` from the release as a package and adds an encryption key.
Released bridges accept updates only from the release feed (HA's update entity), not over the
network. To upload your own builds wirelessly from Builder, add to your config:

```yaml
ota:
  - platform: esphome
    password: !secret ota_password
```

Install that once over USB (or let the update entity install it); after that, Builder uploads
wirelessly.

## Release

Tag `vX.Y.Z` on `main`, matching `version` in both configs. GitHub Actions
(`.github/workflows/release.yml`) builds the bridge for ESP32, ESP32-S3 and ESP32-C3 and the v1
remote, attaches the images to a GitHub Release, and publishes the install page with
`firmware/bridge.manifest.json` and `firmware/remote-v1.manifest.json`. Bridges in HA pick up the
new version as an update; remotes are updated over USB from the install page. Every push and pull
request builds everything and runs the tests (`ci.yml`).

`tests/run.sh` also checks `docs/test-vectors.json`, which the HA integration tests against too:
regenerate it with `./tests/run.sh --write` only when the protocol changes on purpose, and copy it
to the integration's `tests/vectors.json`.

## Troubleshooting

**"No answer from the bridge" on the screen.** The remote is paired but the bridge didn't
acknowledge its hello. In this order:

1. The bridge is on and connected to HA. A bridge that just booted ignores remotes until HA has
   sent it their keys, a few seconds after it connects.
2. The remote is still in HA (Settings → Devices & services → PARAM KEY). If it was removed, hold
   home and back for 5 seconds to pair it again.
3. The access point is still on the channel the remote paired on. Compare the remote's boot log
   (`Paired with bridge ... on channel`) with the bridge's. If it moved, fix the channel and pair
   again.

**Keys do nothing.** Developer tools → Events → listen to `esphome.param_key_key` and press keys.
Events arrive: the radio works, look at the HA side. None arrive: check that the bridge is added
as an ESPHome device and connected (its ESPHome page shows it online).

**HA never shows the remote as discovered.** Listen to `esphome.param_key_pair_request` while the
remote says "Pair with Home Assistant". No events: the bridge doesn't hear it (distance, or the
same "Allow the device…" setting). Events but no discovery: check the integration is installed.

**Watching the radio.** Add `level: VERBOSE` under `logger:` on the bridge or remote,
temporarily. ESP-NOW then logs every received frame (`<<< [remote -> bridge] 50:02:...`) and every
send result (`>>> [mac] OK`, or a failure when the other side didn't acknowledge). Byte 2 is the
packet type: `48` hello, `4B` key, `50` pairing request. The bridge also logs each frame it drops as
not authentic or replayed.

**The remote wakes again a second after going to sleep.** A wake pin is low during sleep. The
remote's last-wake sensor shows the EXT1 mask: bit 21 is the accelerometer (`acc_int_at_boot: low`
means its interrupt held the line), bits 0-18 are keys.

**"Home Assistant offline" on the screen.** The bridge has had no connection to HA for 5 s (a
restart, an update, the network). Keys do nothing until HA is back; the screen then returns on its
own.

**`invalid header: 0xffffffff` on boot.** The flash is blank or was erased: flash the factory image.

## Writing a board file

A board provides the display, backlight, battery sensors, charging input, keys and the `pk_ir`
script (IR LED, or a log line on a board without one); see the header of
`common/remote_core.yaml`. Write its `on_boot` as a list (`- priority: ...`), like the core's:
ESPHome concatenates the two lists, but a single entry in one file replaces the other's. The core's
hello runs at priority -150, after ESP-NOW (-100) has started.
