"""Sub-sensors published by the TFA TX141W decoder."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_HUMIDITY,
    CONF_TEMPERATURE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    ICON_THERMOMETER,
    ICON_WATER_PERCENT,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_DEGREES,
    UNIT_EMPTY,
    UNIT_PERCENT,
)

from . import TFATX141W

DEPENDENCIES = ["tfa_tx141w"]

CONF_TFA_TX141W_ID = "tfa_tx141w_id"
CONF_WIND_SPEED = "wind_speed"
CONF_WIND_DIRECTION = "wind_direction"
CONF_BATTERY_LEVEL = "battery_level"
CONF_SENSOR_ID = "sensor_id"
CONF_PACKETS_VALID = "packets_valid"
CONF_PACKETS_INVALID = "packets_invalid"
CONF_SYNC_DETECTIONS = "sync_detections"

UNIT_KMH = "km/h"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TFA_TX141W_ID): cv.use_id(TFATX141W),
        cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            icon=ICON_THERMOMETER,
        ),
        cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_HUMIDITY,
            state_class=STATE_CLASS_MEASUREMENT,
            icon=ICON_WATER_PERCENT,
        ),
        cv.Optional(CONF_WIND_SPEED): sensor.sensor_schema(
            unit_of_measurement=UNIT_KMH,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:weather-windy",
        ),
        cv.Optional(CONF_WIND_DIRECTION): sensor.sensor_schema(
            unit_of_measurement=UNIT_DEGREES,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:compass",
        ),
        cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_SENSOR_ID): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            icon="mdi:identifier",
        ),
        cv.Optional(CONF_PACKETS_VALID): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            icon="mdi:check-network",
        ),
        cv.Optional(CONF_PACKETS_INVALID): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            icon="mdi:close-network",
        ),
        cv.Optional(CONF_SYNC_DETECTIONS): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            icon="mdi:sync",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_TFA_TX141W_ID])
    if conf := config.get(CONF_TEMPERATURE):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_temperature_sensor(s))
    if conf := config.get(CONF_HUMIDITY):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_humidity_sensor(s))
    if conf := config.get(CONF_WIND_SPEED):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_wind_speed_sensor(s))
    if conf := config.get(CONF_WIND_DIRECTION):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_wind_direction_sensor(s))
    if conf := config.get(CONF_BATTERY_LEVEL):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_battery_level_sensor(s))
    if conf := config.get(CONF_SENSOR_ID):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_sensor_id_sensor(s))
    if conf := config.get(CONF_PACKETS_VALID):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_packets_valid_sensor(s))
    if conf := config.get(CONF_PACKETS_INVALID):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_packets_invalid_sensor(s))
    if conf := config.get(CONF_SYNC_DETECTIONS):
        s = await sensor.new_sensor(conf)
        cg.add(hub.set_sync_detections_sensor(s))
