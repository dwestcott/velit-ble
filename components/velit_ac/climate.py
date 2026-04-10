import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID
from . import VelitAC, CONF_VELIT_AC_ID, velit_ac_ns

VelitACClimate = velit_ac_ns.class_("VelitACClimate", climate.Climate, cg.Component)

CONFIG_SCHEMA = climate.CLIMATE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(VelitACClimate),
        cv.GenerateID(CONF_VELIT_AC_ID): cv.use_id(VelitAC),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    parent = await cg.get_variable(config[CONF_VELIT_AC_ID])
    cg.add(parent.set_climate(var))
