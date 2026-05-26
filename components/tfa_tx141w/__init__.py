"""TFA Dostmann 30.3222.02 (LaCrosse TX141W) wired-tap decoder.

External component for ESPHome that decodes the baseband 433 MHz protocol
tapped before the modulator in the WEATHERMAN 2.1 hardware.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@SoulSolistice"]
# Multiple instances are now supported (the C++ side uses attachInterruptArg
# to pass `this` to the ISR, eliminating the previous singleton requirement).
MULTI_CONF = True

CONF_PIN = "pin"
CONF_DEBUG = "debug"
CONF_T_SYNC = "t_sync"
CONF_T_SHORT = "t_short"
CONF_T_LONG = "t_long"
CONF_T_TOLERANCE = "t_tolerance"
CONF_T_RESET = "t_reset"
CONF_MIN_SYNC_PULSES = "min_sync_pulses"

tfa_tx141w_ns = cg.esphome_ns.namespace("tfa_tx141w")
TFATX141W = tfa_tx141w_ns.class_("TFATX141W", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TFATX141W),
        cv.Required(CONF_PIN): cv.int_range(min=0, max=39),
        cv.Optional(CONF_DEBUG, default=False): cv.boolean,
        cv.Optional(CONF_T_SYNC, default=833): cv.int_range(min=100, max=5000),
        cv.Optional(CONF_T_SHORT, default=208): cv.int_range(min=50, max=1000),
        cv.Optional(CONF_T_LONG, default=417): cv.int_range(min=100, max=2000),
        cv.Optional(CONF_T_TOLERANCE, default=200): cv.int_range(min=20, max=500),
        cv.Optional(CONF_T_RESET, default=2000): cv.int_range(min=500, max=10000),
        cv.Optional(CONF_MIN_SYNC_PULSES, default=8): cv.int_range(min=2, max=12),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pin(config[CONF_PIN]))
    cg.add(var.set_debug(config[CONF_DEBUG]))
    cg.add(var.set_t_sync(config[CONF_T_SYNC]))
    cg.add(var.set_t_short(config[CONF_T_SHORT]))
    cg.add(var.set_t_long(config[CONF_T_LONG]))
    cg.add(var.set_t_tolerance(config[CONF_T_TOLERANCE]))
    cg.add(var.set_t_reset(config[CONF_T_RESET]))
    cg.add(var.set_min_sync_pulses(config[CONF_MIN_SYNC_PULSES]))
