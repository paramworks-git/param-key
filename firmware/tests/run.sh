#!/bin/sh
# Host tests for the link crypto and IR codec (components/param_key/pk_crypto.h,
# components/param_key/pk_protocol.h) against a Python reference. Runs in Docker with
# mbedtls 3, as ESP-IDF 5.5 uses.
#   ./tests/run.sh            run the tests
#   ./tests/run.sh --write    regenerate docs/test-vectors.json (protocol change only)
# HOST_DIR: the repository root as the Docker daemon sees it.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST_DIR="${HOST_DIR:-$ROOT}"
docker build -q -t param-key-crypto-test - >/dev/null <<'EOF'
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ libmbedtls-dev python3-cryptography \
 && rm -rf /var/lib/apt/lists/*
EOF
MOUNT=rw
[ "$1" = "--write" ] || MOUNT=ro
exec docker run --rm -v "$HOST_DIR":/src:$MOUNT param-key-crypto-test sh -c "
  g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/crypto_cli /src/firmware/tests/crypto_cli.cpp -lmbedcrypto &&
  python3 -I /src/firmware/tests/test_crypto.py $*"
