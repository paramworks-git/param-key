"""PARAM KEY: the remote <-> bridge link and protocol (docs/protocol.md), as an ESPHome component.

    external_components:
      - source: github://paramworks-git/param-key@v0.2.0
        components: [param_key]
    param_key:
      role: bridge        # or remote

It brings in the C++ helpers that the YAML's lambdas call (namespace pk) and turns on the mbedtls
features the link needs (AES-CCM, X25519), which ESPHome otherwise trims out as TLS-only. That way a
device config is a single YAML file, with nothing to copy next to it.
"""

import esphome.codegen as cg
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv

CODEOWNERS = []
CONF_ROLE = "role"

# Header-only, included into main.cpp in this order.
HEADERS = {
    "bridge": ["pk_protocol.h", "pk_crypto.h", "pk_bridge.h"],
    "remote": ["pk_protocol.h", "pk_crypto.h", "pk_store.h", "pk_keys.h", "pk_ui.h", "pk_link.h",
               "pk_battery.h", "pk_board_v1.h"],
}

CONFIG_SCHEMA = cv.All(cv.Schema({cv.Required(CONF_ROLE): cv.one_of(*HEADERS, lower=True)}), cv.only_on_esp32)


async def to_code(config):
    # ESPHome's esphome.h includes every header in this folder; the role-specific ones
    # compile to nothing without their define.
    cg.add_define(f"USE_PARAM_KEY_{config[CONF_ROLE].upper()}")
    for header in HEADERS[config[CONF_ROLE]]:
        cg.add_global(cg.RawStatement(f'#include "esphome/components/param_key/{header}"'))
    # The link's crypto (pk_crypto.h). ESPHome turns these off when nothing else uses TLS.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CCM_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECP_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED", True)
