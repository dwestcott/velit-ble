import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID

CODEOWNERS = ["@dwestcott"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor", "switch", "button", "climate"]
MULTI_CONF = True

CONF_VELIT_AC_ID = "velit_ac_id"

velit_ac_ns = cg.esphome_ns.namespace("velit_ac")
VelitAC = velit_ac_ns.class_(
    "VelitAC", cg.PollingComponent, ble_client.BLEClientNode
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VelitAC),
        }
    )
    .extend(cv.polling_component_schema("6s"))
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
