import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import fan
from esphome.const import CONF_ID, CONF_NAME, CONF_UPDATE_INTERVAL
from esphome.core import entity_helpers

from esphome.components.nrf905 import nRF905Component


DEPENDENCIES = ["nrf905"]

zehnder_ns = cg.esphome_ns.namespace("zehnder")
ZehnderRF = zehnder_ns.class_("ZehnderRF", cg.Component, fan.Fan)

CONF_NRF905 = "nrf905"

CONFIG_SCHEMA = cv.All(
    fan.fan_schema(ZehnderRF).extend(
        {
            cv.Required(CONF_NRF905): cv.use_id(nRF905Component),
            cv.Optional(CONF_UPDATE_INTERVAL, default="30s"): cv.update_interval,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on(["esp8266", "esp32"]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await fan.register_fan(var, config)

    nrf905 = await cg.get_variable(config[CONF_NRF905])
    cg.add(var.set_rf(nrf905))

    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
