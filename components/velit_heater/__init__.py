import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID

CODEOWNERS = ["@dwestcott"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor", "switch", "button", "binary_sensor", "climate", "text_sensor"]
MULTI_CONF = True

CONF_VELIT_HEATER_ID = "velit_heater_id"
CONF_IDLE_INTERVAL = "idle_interval"

velit_heater_ns = cg.esphome_ns.namespace("velit_heater")
VelitHeater = velit_heater_ns.class_(
    "VelitHeater", cg.PollingComponent, ble_client.BLEClientNode
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VelitHeater),
            # Poll rate in settled states (Standby / Normal / Overtemp Standby);
            # update_interval applies during transitions and after commands.
            cv.Optional(
                CONF_IDLE_INTERVAL, default="15s"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.polling_component_schema("5s"))
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_idle_interval(config[CONF_IDLE_INTERVAL]))
