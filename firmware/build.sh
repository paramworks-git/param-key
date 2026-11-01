#!/bin/sh
# Validate or compile firmware in the official ESPHome container, with the param_key
# component from this checkout (../components) instead of the released one.
#   ./build.sh config  bridge.yaml
#   ./build.sh compile remote-v1.yaml
#   ./build.sh -s board esp32-s3-devkitc-1 compile bridge.yaml   # another chip
# HOST_DIR: the repository root as the Docker daemon sees it (differs when Docker runs
# on another host, e.g. a NAS or home server). Flash the resulting
# .esphome/build/<name>/build/firmware.factory.bin from the install page or esptool.
set -e
ESPHOME_VERSION=2026.10.1
HOST_DIR="${HOST_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
exec docker run --rm -v "$HOST_DIR":/src -v pk-pio:/root/.platformio -v pk-cache:/cache \
  -w /src/firmware "ghcr.io/esphome/esphome:$ESPHOME_VERSION" -s pk_component ../components "$@"
