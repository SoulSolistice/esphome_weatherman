"""Debug text sensors for the TFA TX141W decoder."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import TFATX141W

DEPENDENCIES = ["tfa_tx141w"]

CONF_TFA_TX141W_ID = "tfa_tx141w_id"
CONF_LAST_PACKET_HEX = "last_packet_hex"
CONF_LAST_STATUS = "last_status"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TFA_TX141W_ID): cv.use_id(TFATX141W),
        cv.Optional(CONF_LAST_PACKET_HEX): text_sensor.text_sensor_schema(
            icon="mdi:identifier",
        ),
        cv.Optional(CONF_LAST_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:information-outline",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_TFA_TX141W_ID])
    if conf := config.get(CONF_LAST_PACKET_HEX):
        s = await text_sensor.new_text_sensor(conf)
        cg.add(hub.set_last_packet_hex_sensor(s))
    if conf := config.get(CONF_LAST_STATUS):
        s = await text_sensor.new_text_sensor(conf)
        cg.add(hub.set_last_status_sensor(s))
