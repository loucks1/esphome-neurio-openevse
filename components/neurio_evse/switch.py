import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from . import NeurioEvseComponent, neurio_evse_ns

CONF_NEURIO_EVSE_ID = "neurio_evse_id"
CONF_REQUIRE_HOURLY_ACK = "require_hourly_ack"
CONF_OPENEVSE_FEED = "openevse_feed"

NeurioEvseSwitch = neurio_evse_ns.class_("NeurioEvseSwitch", switch.Switch, cg.Component)
NeurioEvseFeedSwitch = neurio_evse_ns.class_("NeurioEvseFeedSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NEURIO_EVSE_ID): cv.use_id(NeurioEvseComponent),
        cv.Required(CONF_REQUIRE_HOURLY_ACK): switch.switch_schema(
            NeurioEvseSwitch,
            icon="mdi:check-network",
        ),
        cv.Optional(CONF_OPENEVSE_FEED): switch.switch_schema(
            NeurioEvseFeedSwitch,
            icon="mdi:sun-angle",
            default_restore_mode="RESTORE_DEFAULT_ON",
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

    if CONF_OPENEVSE_FEED in config:
        feed_conf = config[CONF_OPENEVSE_FEED]
        feed = await switch.new_switch(feed_conf)
        await cg.register_component(feed, feed_conf)
        cg.add(hub.set_feed_switch(feed))
        cg.add(feed.set_parent(hub))
