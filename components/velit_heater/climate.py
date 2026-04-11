import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID
from . import VelitHeater, CONF_VELIT_HEATER_ID, velit_heater_ns

VelitHeaterClimate = velit_heater_ns.class_(
    "VelitHeaterClimate", climate.Climate, cg.Component
)

CONFIG_SCHEMA = climate.climate_schema(VelitHeaterClimate).extend(
    {
        cv.GenerateID(CONF_VELIT_HEATER_ID): cv.use_id(VelitHeater),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    parent = await cg.get_variable(config[CONF_VELIT_HEATER_ID])
    cg.add(parent.set_climate(var))
