import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone, speaker
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER

CODEOWNERS = ["@formatBCE"]
DEPENDENCIES = ["esp32", "network", "microphone", "speaker"]

CONF_BRIDGE_URL = "bridge_url"
CONF_DEVICE_ID = "device_id"
CONF_CREDENTIAL = "credential"
CONF_IDLE_TIMEOUT = "idle_timeout"
CONF_INPUT_CHANNEL = "input_channel"

respeaker_realtime_ns = cg.esphome_ns.namespace("respeaker_realtime")
RespeakerRealtime = respeaker_realtime_ns.class_("RespeakerRealtime", cg.Component)


def _bridge_url(value):
    value = cv.string_strict(value)
    if not value.startswith(("ws://", "wss://")):
        raise cv.Invalid("bridge_url must use ws:// or wss://")
    return value


def _json_safe_device_id(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", value):
        raise cv.Invalid("device_id must be 1..128 JSON-safe identifier characters")
    return value


def _json_safe_credential(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[A-Za-z0-9._~:/+=-]{1,256}", value):
        raise cv.Invalid("credential must be a 1..256 character opaque token")
    return value


def _idle_timeout(value):
    value = cv.positive_time_period_seconds(value)
    if value.total_seconds < 5 or value.total_seconds > 300:
        raise cv.Invalid("idle_timeout must be between 5s and 300s")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RespeakerRealtime),
            cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Required(CONF_BRIDGE_URL): _bridge_url,
            cv.Required(CONF_DEVICE_ID): _json_safe_device_id,
            cv.Required(CONF_CREDENTIAL): cv.sensitive(_json_safe_credential),
            cv.Optional(CONF_IDLE_TIMEOUT, default="30s"): _idle_timeout,
            cv.Optional(CONF_INPUT_CHANNEL, default=0): cv.int_range(min=0, max=1),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    spkr = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_microphone(mic))
    cg.add(var.set_speaker(spkr))
    cg.add(var.set_bridge_url(config[CONF_BRIDGE_URL]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    cg.add(var.set_credential(config[CONF_CREDENTIAL]))
    cg.add(var.set_idle_timeout_seconds(config[CONF_IDLE_TIMEOUT].total_seconds))
    cg.add(var.set_input_channel(config[CONF_INPUT_CHANNEL]))

    # ESP-IDF WebSocket client is an Espressif managed component. Pin it so the
    # generated firmware dependency graph is reproducible.
    add_idf_component(name="espressif/esp_websocket_client", ref="1.6.1")
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
