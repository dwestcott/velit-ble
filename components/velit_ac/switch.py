import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID
from . import VelitAC, CONF_VELIT_AC_ID, velit_ac_ns

VelitSwingSwitch = velit_ac_ns.class_("VelitSwingSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = switch.switch_schema(VelitSwingSwitch).extend(
    {
        cv.GenerateID(CONF_VELIT_AC_ID): cv.use_id(VelitAC),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_VELIT_AC_ID])
    cg.add(parent.set_swing_switch(var))
