import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID, UNIT_WATT
from . import NeurioEvseComponent, neurio_evse_ns

CONF_NEURIO_EVSE_ID = "neurio_evse_id"
CONF_GRID_OFFSET = "grid_offset"

NeurioEvseNumber = neurio_evse_ns.class_("NeurioEvseNumber", number.Number, cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NEURIO_EVSE_ID): cv.use_id(NeurioEvseComponent),
        cv.Required(CONF_GRID_OFFSET): number.number_schema(
            NeurioEvseNumber,
            unit_of_measurement=UNIT_WATT,
        ),
    }
)

async def to_code(config):
    hub = await cg.get_variable(config[CONF_NEURIO_EVSE_ID])
    num_conf = config[CONF_GRID_OFFSET]
    num = await number.new_number(
        num_conf,
        min_value=-10000.0,
        max_value=10000.0,
        step=50.0,
    )
    await cg.register_component(num, num_conf)
    cg.add(num.set_parent(hub))
