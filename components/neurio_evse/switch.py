import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID
from . import NeurioEvseComponent, neurio_evse_ns

CONF_NEURIO_EVSE_ID = "neurio_evse_id"
CONF_REQUIRE_HOURLY_ACK = "require_hourly_ack"

NeurioEvseSwitch = neurio_evse_ns.class_("NeurioEvseSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NEURIO_EVSE_ID): cv.use_id(NeurioEvseComponent),
        cv.Required(CONF_REQUIRE_HOURLY_ACK): switch.switch_schema(
            NeurioEvseSwitch,
            icon="mdi:check-network",
        ),
    }
)

async def to_code(config):
    hub = await cg.get_variable(config[CONF_NEURIO_EVSE_ID])
    sw_conf = config[CONF_REQUIRE_HOURLY_ACK]
    sw = await switch.new_switch(sw_conf)
    await cg.register_component(sw, sw_conf)
    cg.add(hub.set_require_ack_switch(sw))
    cg.add(sw.set_parent(hub))
